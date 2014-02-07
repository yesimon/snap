/*++

Module Name:

    MultiInputReadSupplier.cp

Abstract:

    A read supplier that combines other read suppliers.  It's used when there are muliple input files to process.

Authors:

    Bill Bolosky, November, 2012

Environment:

    User mode service.

Revision History:


--*/

#include "stdafx.h"
#include <map>
#include "Compat.h"
#include "Util.h"
#include "Read.h"
#include "DataReader.h"
#include "VariableSizeMap.h"
#include "DynamicList.h"

// turn on to debug matching process
//#define VALIDATE_MATCH

// turn on to gather paired stats
//#define STATISTICS

using std::pair;


class PairedReadMatcher: public PairedReadReader
{
public:
    PairedReadMatcher(ReadReader* i_single, bool i_quicklyDropUnpairedReads);

    // PairedReadReader

    virtual ~PairedReadMatcher();

    virtual bool getNextReadPair(Read *read1, Read *read2);
    
    virtual void reinit(_int64 startingOffset, _int64 amountOfFileToProcess)
    { single->reinit(startingOffset, amountOfFileToProcess); }

    virtual void holdBatch(DataBatch batch)
    { single->holdBatch(batch); }

    virtual bool releaseBatch(DataBatch batch)
    { return single->releaseBatch(batch); }

private:

    ReadWithOwnMemory* allocOverflowRead();
    void freeOverflowRead(ReadWithOwnMemory* read);
    
    ReadReader* single; // reader for single reads
    typedef _uint64 StringHash;
    typedef VariableSizeMap<StringHash,Read> ReadMap;
    DataBatch currentBatch; // for dropped reads
    bool allDroppedInCurrentBatch;
    DataBatch batch[2]; // 0 = current, 1 = previous
    ReadMap unmatched[2]; // read id -> Read
    typedef VariableSizeMap<PairedReadMatcher::StringHash,ReadWithOwnMemory*,150,MapNumericHash<PairedReadMatcher::StringHash>,80,0,true> OverflowMap;
    OverflowMap overflow; // read id -> Read
    typedef VariableSizeVector<ReadWithOwnMemory*> OverflowReadVector;
    typedef VariableSizeMap<_uint64,OverflowReadVector*> OverflowReadReleaseMap;
    OverflowReadReleaseMap overflowRelease;
#ifdef VALIDATE_MATCH
    typedef VariableSizeMap<StringHash,char*> StringMap;
    StringMap strings;
    typedef VariableSizeMap<StringHash,int> HashSet;
    HashSet overflowUsed;
#endif
    _int64 overflowMatched;

    bool quicklyDropUnpairedReads;
    _uint64 nReadsQuicklyDropped;

    Read localRead;

#ifdef STATISTICS
    typedef struct
    {
        _int64 oldPairs; // # pairs matched from overflow
        _int64 oldBatches; // # distinct matches matched from overflow
        _int64 internalPairs; // #pairs matched within batch
        _int64 previousPairs; // #pairs matched with previous batch
        _int64 overflowPairs; // #pairs left over
        _int64 totalReads; // total reads in batch
        void clear() { memset(this, 0, sizeof(*this)); }
    } BatchStats;
    BatchStats currentStats, totalStats;
    VariableSizeMap<_int64,int> currentBatches;
#endif
};

PairedReadMatcher::PairedReadMatcher(
    ReadReader* i_single,
    bool i_quicklyDropUnpairedReads)
    : single(i_single),
    overflowMatched(0),
    quicklyDropUnpairedReads(i_quicklyDropUnpairedReads),
    nReadsQuicklyDropped(0),
    currentBatch(0, 0), allDroppedInCurrentBatch(false)
{
    new (&unmatched[0]) VariableSizeMap<StringHash,Read>(10000);
    new (&unmatched[1]) VariableSizeMap<StringHash,Read>(10000);

#ifdef STATISTICS
    currentStats.clear();
    totalStats.clear();
#endif
}
    
PairedReadMatcher::~PairedReadMatcher()
{
    delete single;
}

    ReadWithOwnMemory*
PairedReadMatcher::allocOverflowRead()
{
    return new ReadWithOwnMemory();
}

    void
PairedReadMatcher::freeOverflowRead(
    ReadWithOwnMemory* read)
{
    delete read;
}

    bool
PairedReadMatcher::getNextReadPair(
    Read *read1,
    Read *read2)
{
    int skipped = 0;
    while (true) {
        if (skipped++ == 10000) {
            fprintf(stderr, "warning: no matching read pairs in 10,000 reads, input file might be unsorted or have unexpected read id format\n");
        }

        if (! single->getNextRead(&localRead)) {
            int n = unmatched[0].size() + unmatched[1].size();
            int n2 = (int) (overflow.size() - overflowMatched);
            if (n + n2 > 0) {
                fprintf(stderr, " warning: PairedReadMatcher discarding %d+%d unpaired reads at eof\n", n, n2);
#ifdef VALIDATE_MATCH
                for (int i = 0; i < 2; i++) {
                    fprintf(stdout, "unmatched[%d]\n", i);
                    for (ReadMap::iterator j = unmatched[i].begin(); j != unmatched[i].end(); j = unmatched[i].next(j)) {
                        fprintf(stdout, "%s\n", strings[j->key]);
                    }
                }
                int printed = 0;
                fprintf(stdout, "sample of overflow\n");
                for (OverflowMap::iterator o = overflow.begin(); printed < 500 && o != overflow.end(); o = overflow.next(o)) {
                    if (NULL == overflowUsed.tryFind(o->key)) {
                        printed++;
                        fprintf(stdout, "%s\n", strings[o->key]);
                    }
                }
#endif
            }
            if (nReadsQuicklyDropped > 0) {
                fprintf(stderr," warning: PairedReadMatcher dropped %lld reads because they didn't have RNEXT and PNEXT filled in.\n"
                               " If your input file was generated by a single-end alignment (or this seems too big), use the -ku flag\n",
                    nReadsQuicklyDropped);
            }
            single->releaseBatch(batch[0]);
            single->releaseBatch(batch[1]);
            return false;
        }

        if (quicklyDropUnpairedReads) {
            if (localRead.getOriginalPNEXT() == 0 || localRead.getOriginalRNEXTLength() == 1 && localRead.getOriginalRNEXT()[0] == '*') {
                nReadsQuicklyDropped++;
                skipped--;
                continue;
            }
        }

        // build key for pending read table, removing /1 or /2 at end
        const char* id = localRead.getId();
        unsigned idLength = localRead.getIdLength();
        // truncate at space or slash
        char* slash = (char*) memchr((void*)id, '/', idLength);
        if (slash != NULL) {
            idLength = (unsigned)(slash - id);
        }
        char* space = (char*) memchr((void*)id, ' ', idLength);
        if (space != NULL) {
            idLength = (unsigned)(space - id);
        }
        StringHash key = util::hash64(id, idLength);
#ifdef VALIDATE_MATCH
        char* s = new char[idLength+1];
        memcpy(s, id, idLength);
        s[idLength] = 0;
        char** p = strings.tryFind(key);
        if (p != NULL && strcmp(*p, s)) {
          fprintf(stderr, "hash collision %ld of %s and %s\n", key, *p, s);
          soft_exit(1);
        }
        if (p == NULL) {
          strings.put(key, s);
        }
#endif
        if (localRead.getBatch() != batch[0]) {
#ifdef STATISTICS
            currentStats.oldBatches = currentBatches.size();
            currentStats.overflowPairs = unmatched[1].size();
            totalStats.internalPairs += currentStats.internalPairs;
            totalStats.previousPairs += currentStats.previousPairs;
            totalStats.oldBatches += currentStats.oldBatches;
            totalStats.oldPairs += currentStats.oldPairs;
            totalStats.overflowPairs += currentStats.overflowPairs;
            totalStats.totalReads += currentStats.totalReads;
            printf("batch %d:%d: internal %d pairs, previous %d pairs, old %d pairs from %d batches, overflow %d pairs\n"
                "cumulative: internal %d pairs, previous %d pairs, old %d pairs from %d batches, overflow %d pairs\n",
                batch[0].fileID, batch[0].batchID, currentStats.internalPairs, currentStats.previousPairs, currentStats.oldPairs, currentStats.oldBatches, currentStats.overflowPairs,
                totalStats.internalPairs, totalStats.previousPairs, totalStats.oldPairs, totalStats.oldBatches, totalStats.overflowPairs);
            currentStats.clear();
            currentBatches.clear();
#endif
            // roll over batches
            if (unmatched[1].size() > 0) {
                // copy remaining reads into overflow map
                //printf("warning: PairedReadMatcher overflow %d unpaired reads from %d:%d\n", unmatched[1].size(), batch[1].fileID, batch[1].batchID); //!!
                char* buf = (char*) alloca(500);
                for (ReadMap::iterator r = unmatched[1].begin(); r != unmatched[1].end(); r = unmatched[1].next(r)) {
                    ReadWithOwnMemory* p = allocOverflowRead();
                    new (p) ReadWithOwnMemory(r->value);
                    overflow.put(r->key, p);
#ifdef VALIDATE_MATCH
                    char*s2 = *strings.tryFind(r->key);
                    int len = strlen(s2);
                    _ASSERT(! strncmp(s2, r->value.getId(), len));
                    ReadWithOwnMemory* rd = overflow.tryFind(r->key);
                    _ASSERT(! strncmp(s2, rd->getId(), len));
#endif
                    memcpy(buf, r->value.getId(), r->value.getIdLength());
                    buf[r->value.getIdLength()] = 0;
                    //printf("overflow add %d:%d %s\n", batch[1].fileID, batch[1].batchID, buf);
                }
            }
            for (ReadMap::iterator i = unmatched[1].begin(); i != unmatched[1].end(); i = unmatched[1].next(i)) {
                i->value.dispose();
            }
            unmatched[1].exchange(unmatched[0]);
            unmatched[0].clear();
            single->releaseBatch(batch[1]);
            batch[1] = batch[0];
            batch[0] = localRead.getBatch();
            single->holdBatch(batch[0]);
#ifdef STATISTICS
        currentStats.totalReads++;
#endif
        }

        ReadMap::iterator found = unmatched[0].find(key);
        if (found != unmatched[0].end()) {
            *read2 = found->value;
            //printf("current matched %d:%d->%d:%d %s\n", read2->getBatch().fileID, read2->getBatch().batchID, batch[0].fileID, batch[0].batchID, read2->getId()); //!!
            unmatched[0].erase(found->key);
#ifdef STATISTICS
            currentStats.internalPairs++;
#endif
        } else {
            // try previous batch
            found = unmatched[1].find(key);
            if (found == unmatched[1].end()) {
                // try overflow
                OverflowMap::iterator found2 = overflow.find(key);
                if (found2 == overflow.end()) {
                    // no match, remember it for later matching
                    unmatched[0].put(key, localRead);
                    //printf("unmatched add %d:%d %lx\n", batch[0].fileID, batch[0].batchID, key); //!!
                    continue;
                } else {
                    // copy data into read, remember to release when this batch is released
                    new (read2) Read(*(Read*)found2->value);
                    overflowMatched++;
                    OverflowReadVector* v;
                    if (! overflowRelease.tryGet(batch[0].asKey(), &v)) {
                        v = new OverflowReadVector();
                        overflowRelease.put(batch[0].asKey(), v);
                        //printf("overflow fetch into %d:%d\n", batch[0].fileID, batch[0].batchID);
                    }
                    v->push_back(found2->value);
                    //printf("overflow matched %d:%d %s\n", read2->getBatch().fileID, read2->getBatch().batchID, read2->getId()); //!!
                    read2->setBatch(batch[0]); // overwrite batch so both reads have same batch, will track deps instead
#ifdef VALIDATE_MATCH
                    overflowUsed.put(key, 1);
#endif
#ifdef STATISTICS
                    currentStats.oldPairs++;
                    currentBatches.put(read2->getBatch().asKey(), 1);
#endif
                }
            } else {
                // found a match in preceding batch
                *read2 = found->value;
                //printf("prior matched %d:%d->%d:%d %s\n", read2->getBatch().fileID, read2->getBatch().batchID, batch[0].fileID, batch[0].batchID, read2->getId()); //!!
                unmatched[1].erase(found->key);
#ifdef STATISTICS
                currentStats.previousPairs++;
#endif
            }
        }

        // found a match
        *read1 = localRead;
        return true;
    }
}

// define static factory function

    PairedReadReader*
PairedReadReader::PairMatcher(
    ReadReader* single,
    bool quicklyDropUnpairedReads)
{
    return new PairedReadMatcher(single, quicklyDropUnpairedReads);
}
