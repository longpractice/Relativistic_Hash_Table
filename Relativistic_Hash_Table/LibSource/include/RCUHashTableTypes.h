#pragma once
#include "AtomicSinglyLinkedListTypes.h"
#include "RCUTypes.h"
namespace yrcu
{
struct RNode
{
	size_t hash;
	AtomicSingleHead head;
};

// RTableCore does not include the RCUZone and thus is feasible for shared RCUZone
struct RTableCore
{
	~RTableCore();

	std::atomic<size_t> size = 0;
	struct Bucket
	{
		AtomicSingleHead list;
	};

	struct BucketsInfo
	{
		size_t nrBucketsPowerOf2;
		Bucket* pBuckets;
	};

	// expand when element/buckets-count grows over this factor
	float expandFactor = 256.f;
	// shrink when element/buckets-count is less than this factor
	float shrinkFactor = 8.f;

	std::atomic<BucketsInfo*> pBucketsInfo = nullptr;
};

struct RTable
{
	RTableCore core;
	RCUZone rcuZone;
};
}	 // namespace yrcu