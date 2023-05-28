// Minimum_Userspace_RCU.cpp : This file contains the 'main' function. Program
// execution begins and ends there.
//
#include <atomic>
#include <thread>

#include "RCUApi.h"
#include "RCUTypes.h"

namespace yrcu
{
namespace
{
	std::hash<std::thread::id> threadIdHasher;

	uint32_t upperBoundPowerOf2(uint32_t v)
	{
		if (v == 0)
			++v;
		v--;
		v |= v >> 1;
		v |= v >> 2;
		v |= v >> 4;
		v |= v >> 8;
		v |= v >> 16;
		v++;
		return v;
	}

	int nrBucketsPerEpoch(RCUZone& zone)
	{
		return zone.nrHashThreadBuckets + RCUReaderThreadRegistry::nrNonOverlappingBucketCount;
	}
}	 // namespace

std::atomic<int> RCUReaderThreadRegistry::nextBucketId = 0;
const int RCUReaderThreadRegistry::nrNonOverlappingBucketCount =
		static_cast<int>(upperBoundPowerOf2(std::thread::hardware_concurrency()));
thread_local int RCUReaderThreadRegistry::tlsReaderBucketId = -1;

// return if registration is successful
bool rcuRegisterReaderThread()
{
	if (RCUReaderThreadRegistry::tlsReaderBucketId != -1)
		return false;	 // already registered
	auto bucketId = RCUReaderThreadRegistry::nextBucketId++;
	if (bucketId >= RCUReaderThreadRegistry::nrNonOverlappingBucketCount)
		return false;	 // too many threads already registered, fail this
									 // registration
	RCUReaderThreadRegistry::tlsReaderBucketId = bucketId;
	return true;
}

void rcuInitZoneWithBucketCounts(RCUZone& zone, uint32_t nrHashThreadBuckets)
{
	if (nrHashThreadBuckets < 1)
		nrHashThreadBuckets = 1;
	zone.nrHashThreadBuckets = upperBoundPowerOf2(nrHashThreadBuckets);
	const size_t nrBucketsOneEpoch = nrBucketsPerEpoch(zone);
	const size_t nrTotalRefCounts = nrBucketsOneEpoch * c_maxEpoches;
	RCUReaderRefCountBucket* pStart = new RCUReaderRefCountBucket[nrTotalRefCounts];
	for (size_t i = 0; i < nrTotalRefCounts; ++i)
		(pStart + i)->count.store(0);
	for (auto iEpoch = 0; iEpoch < c_maxEpoches; ++iEpoch)
		zone.epochsRing[iEpoch].pBuckets = pStart + iEpoch * nrBucketsOneEpoch;

	zone.epochLatest = 0;
	zone.epochOldest = 0;
}

void rcuInitZone(RCUZone& zone)
{
	uint32_t nrHardwareConcurrency = std::thread::hardware_concurrency();
	rcuInitZoneWithBucketCounts(zone, nrHardwareConcurrency * c_nrRCUBucketsPerHardwareThread);
}

void rcuReleaseZone(RCUZone& zone)
{
	const size_t nrBucketsOneEpoch = nrBucketsPerEpoch(zone);
	const size_t nrTotalRefCounts = nrBucketsOneEpoch * c_maxEpoches;
	delete[] (zone.epochsRing[0].pBuckets);
	zone.epochsRing[0].pBuckets = nullptr;
}

namespace
{
	int fetchThreadBucketId(RCUZone& zone)
	{
		if (RCUReaderThreadRegistry::tlsReaderBucketId != -1)
			return RCUReaderThreadRegistry::tlsReaderBucketId;
		// fetch by hashing thread id
		auto hash = threadIdHasher(std::this_thread::get_id());
		return RCUReaderThreadRegistry::nrNonOverlappingBucketCount +
					 (int)(hash & (zone.nrHashThreadBuckets - 1));
	}
}	 // namespace

// epoch is returned to be used for unlocking
int64_t rcuReadLock(RCUZone& zone)
{
	auto threadBucketId = fetchThreadBucketId(zone);
	while (true)
	{
		// use relaxed here since we are going to do the acquire for the
		// revalidation
		int64_t epochId = zone.epochLatest.load(std::memory_order_relaxed);
		auto epochRowId = epochId & c_epochMask;
		std::atomic<int>& count = zone.epochsRing[epochRowId].pBuckets[threadBucketId].count;
		count.fetch_add(1, std::memory_order_acq_rel);

		int64_t epochIdRevalidate = zone.epochLatest.load(std::memory_order_acquire);

		if (epochIdRevalidate == epochId)
			return epochId;
		else
		{
			// writer updated the epoch after we firstly read out the epoch id
			// revert the ref-count and
			count.fetch_add(-1, std::memory_order_relaxed);
		}
	}
}

void rcuReadUnlock(RCUZone& zone, int64_t epoch)
{
	auto threadBucketId = fetchThreadBucketId(zone);
	auto epochRowId = epoch & c_epochMask;
	std::atomic<int>& count = zone.epochsRing[epochRowId].pBuckets[threadBucketId].count;
	count.fetch_add(-1, std::memory_order_acq_rel);
}

void rcuSynchronize(RCUZone& zone)
{
	auto lastEpoch = zone.epochLatest.fetch_add(1, std::memory_order_release);
	// wait for all the other readers to finish
	auto nrBucketsOneZone = nrBucketsPerEpoch(zone);
	for (; zone.epochOldest <= lastEpoch; ++zone.epochOldest)
	{
		int64_t epochRowId = zone.epochOldest & c_epochMask;
		for (int iBucket = 0; iBucket < nrBucketsOneZone; ++iBucket)
			while (zone.epochsRing[epochRowId].pBuckets[iBucket].count.load(std::memory_order_acquire) >
						 0)
				continue;
	}
}

RCUZone::~RCUZone()
{
	if (epochsRing[0].pBuckets)
		rcuReleaseZone(*this);
}
}	 // namespace yrcu
