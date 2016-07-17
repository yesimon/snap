/*++

Module Name:

	TenXClusterAligner.h

Abstract:

	A paired-end aligner calls into a different paired-end aligner, and if
	it fails to find an alignment, aligns each of the reads singly.  This handles
	chimeric reads that would otherwise be unalignable.

Authors:

	Bill Bolosky, June, 2013

Environment:

	User mode service.

Revision History:

--*/

#pragma once

#include "TenXSingleAligner.h"
#include "BaseAligner.h"
#include "BigAlloc.h"

class TenXClusterAligner : public PairedEndAligner {
public:
	TenXClusterAligner(
		GenomeIndex			*index_,
		unsigned			maxReadSize,
		unsigned			maxHits,
		unsigned			maxK,
		unsigned			maxSeedsFromCommandLine,
		double				seedCoverage,
		unsigned			minWeightToCheck,
		bool				forceSpacing_,
		unsigned			extraSearchDepth,
		bool				noUkkonen,
		bool				noOrderedEvaluation,
		bool				noTruncation,
		bool				ignoreALignmentAdjustmentsForOm,
		TenXSingleAligner	**underlyingTenXSingleAligner_,
		unsigned			maxBarcodeSize_,
		unsigned			minReadLength_,
		int					maxSecondaryAlignmentsPerContig,
		BigAllocator		*allocator);

	virtual ~TenXClusterAligner();

	static size_t getBigAllocatorReservation(GenomeIndex * index, unsigned maxReadSize, unsigned maxHits, unsigned seedLen, unsigned maxSeedsFromCommandLine,
		double seedCoverage, unsigned maxEditDistanceToConsider, unsigned maxExtraSearchDepth, unsigned maxCandidatePoolSize,
		int maxSecondaryAlignmentsPerContig);

	void *operator new(size_t size, BigAllocator *allocator) { _ASSERT(size == sizeof(TenXClusterAligner)); return allocator->allocate(size); }
	void operator delete(void *ptr, BigAllocator *allocator) {/* do nothing.  Memory gets cleaned up when the allocator is deleted.*/ }

	// return true if all pairs within the cluster have been processed. No secondary results overflow.
	virtual bool align(
		Read					**pairedReads,
		unsigned				barcodeSize,
		PairedAlignmentResult	**result,
		int						maxEditDistanceForSecondaryResults,
		_int64					*secondaryResultBufferSize,
		_int64					*nSecondaryResults,
		_int64					*singleSecondaryBufferSize,
		_int64					maxSecondaryAlignmentsToReturn,
		_int64					*nSingleEndSecondaryResults,
		SingleAlignmentResult	**singleEndSecondaryResults,	// Single-end secondary alignments for when the paired-end alignment didn't work properly
		bool					*notFinished					// True if the pair is done
	);

	/*
	 * this align is just a place holder, to comply with the pure virtual function in PairedEndAligner
	 */
	virtual bool align(
		Read					*read0,
		Read					*read1,
		PairedAlignmentResult	*result,
		int						maxEditDistanceForSecondaryResults,
		_int64					secondaryResultBufferSize,
		_int64					*nSecondaryResults,
		PairedAlignmentResult	*secondaryResults,             // The caller passes in a buffer of secondaryResultBufferSize and it's filled in by AlignRead()
		_int64					singleSecondaryBufferSize,
		_int64					maxSecondaryAlignmentsToReturn,
		_int64					*nSingleEndSecondaryResultsForFirstRead,
		_int64					*nSingleEndSecondaryResultsForSecondRead,
		SingleAlignmentResult	*singleEndSecondaryResults     // Single-end secondary alignments for when the paired-end alignment didn't work properly
	) {
		return true;
	};

	void *operator new(size_t size) { return BigAlloc(size); }
	void operator delete(void *ptr) { BigDealloc(ptr); }

	virtual _int64 getLocationsScored() const {
		_int64 locationsScored = 0;
		for (int readIdx = 0; readIdx < barcodeSize; readIdx++) {
			locationsScored += underlyingTenXSingleAligner[readIdx]->getLocationsScored();
		}
		return locationsScored + singleAligner->getLocationsScored();
	}

private:

	bool				forceSpacing;
	BaseAligner			*singleAligner;
	unsigned			maxBarcodeSize;
	TenXSingleAligner	**underlyingTenXSingleAligner;

	// avoid allocation in aligner calls
	IdPairVector*		singleSecondary[2];

	LandauVishkin<1>	lv;
	LandauVishkin<-1>	reverseLV;

	GenomeIndex			*index;
	unsigned			minReadLength;
	unsigned			barcodeSize;
};
