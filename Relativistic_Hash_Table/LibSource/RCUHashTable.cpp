#include <cassert>
#include <thread>

#include "include/RCUApi.h"
#include "include/RCUHashTableApi.h"
#include "include/RCUHashTableCoreApi.h"
#include "include/RCUHashTableTypes.h"
#include "include/RCUTypes.h"

namespace yrcu
{
namespace
{
	uint32_t upperBoundPowerOf2(uint32_t v)
	{
		if (v == 0)
			return 1;
		v--;
		v |= v >> 1;
		v |= v >> 2;
		v |= v >> 4;
		v |= v >> 8;
		v |= v >> 16;
		v++;
		return v;
	}

	RTableCore::BucketsInfo* allocateAndInitBuckets(size_t nrBucketsPowerOf2)
	{
		assert(nrBucketsPowerOf2 > 0);
		// allocate buckets info and the buckets together, better cache perf
		size_t allocSize =
				sizeof(RTableCore::BucketsInfo) + nrBucketsPowerOf2 * sizeof(RTableCore::Bucket);
		void* p = malloc(allocSize);

		RTableCore::BucketsInfo* pBucketsInfo = new (p) RTableCore::BucketsInfo();
		pBucketsInfo->nrBucketsPowerOf2 = nrBucketsPowerOf2;

		char* pBucketsChar = (char*)p + sizeof(RTableCore::BucketsInfo);
		RTableCore::Bucket* pBuckets = new (pBucketsChar) RTableCore::Bucket[nrBucketsPowerOf2];

		pBucketsInfo->pBuckets = pBuckets;
		// seems that for some implemenation, we cannot assume std::atomic int is
		// triavially constructable otherwise, we should just do a memset to 0;
		for (int iBucket = 0; iBucket < nrBucketsPowerOf2; ++iBucket)
			(pBuckets + iBucket)->list.head.next.store(nullptr, std::memory_order_release);
		return pBucketsInfo;
	}

	void destroyAndFreeBuckets(RTableCore::BucketsInfo* p)
	{
		static_assert(std::is_trivially_destructible_v<RTableCore::Bucket>);
		static_assert(std::is_trivially_destructible_v<RTableCore::BucketsInfo>);
		free(p);
	}

	// pSrc0->next is pointing to the 0th element of the src0
	// pSrc1->next is pointing to the 0th element of the src1
	// pSrc and pSrc1 are not nullptr themselves
	void
	shrinkTwoBucketsToOne(RcuSlist* pSrc0, RcuSlist* pSrc1, RcuSlist* pDst)
	{
		RcuSlistHead* pSrc0First = pSrc0->head.next.load(std::memory_order_relaxed);
		RcuSlistHead* pSrc1First = pSrc1->head.next.load(std::memory_order_relaxed);

		// Splice list 1 onto the end of list 0, and then let pDst point to the
		// first element of list 0. This would mean that we need to walk list 0 to
		// find its tail. But we could skip that if list 1 is empty.
		if (pSrc1First == nullptr)
		{
			pDst->head.next.store(pSrc0First, std::memory_order_release);
			return;
		}

		// If list 0 is empty, we directly let the pDst point to the first element
		// of list 1.
		if (pSrc0First == nullptr)
		{
			pDst->head.next.store(pSrc1First, std::memory_order_release);
			return;
		}

		// search so that p is the last element of src0
		RcuSlistHead* pSrc0Last = pSrc0First;
		while (true)
		{
			RcuSlistHead* pNext = pSrc0Last->next.load(std::memory_order_relaxed);
			if (pNext == nullptr)
				break;
			pSrc0Last = pNext;
		}

		pSrc0Last->next.store(pSrc1First, std::memory_order_release);
		pDst->head.next.store(pSrc0First, std::memory_order_release);
	}

	size_t computeHashBucketId(RcuSlistHead* p, size_t hashMask)
	{
		return YJ_CONTAINER_OF(p, RNode, head)->hash & hashMask;
	}

	// caller should make sure that pZipStart->next is not null
	void unzipOneSegment(RcuSlist* pZipStart, size_t hashMaskExpanded)
	{
		// pZipStart.next points to an element whose next has a different expanded
		// hash bucket
		//
		// An example old hash bucket's element chain(4 segments):
		//|--seg0--|  |--Seg1--|  |--Seg2--|  |--Seg3---|
		//  x   x  X    y y  y Y    x  x   x    y y y y y
		//(All x or X above will hash to the same expanded hash bucket.)
		//(All y or Y above will hash to the same expanded hash bucket.)
		//
		// Before this function pZipStart->next points to X (jumpStart, last element
		// of seg0) and yyyY(Seg1) are guaranteed to be reached from readers
		// starting from a correct expanded hash bucket.
		//
		// After this function pZipStart->next points to Y (newJumpStart, end of
		// seg1) and X->next points to the first x of seg2. Thus yyyY is "unzipped"
		//
		// After this function, the caller needs to call rcuSynchronize() to ensure
		// that all the readers reading xxx(seg2) is doing so through a correct
		// expanded hash bucket (otherwise,
		// since some reader looking for elements in seg2 might be still traversing
		// seg1, next unzip to let
		//  Y to point to first y in Seg3 will make these readers not reaching what
		//  they want)
		RcuSlistHead* pJumpStart = pZipStart->head.next.load(std::memory_order_relaxed);
		assert(pJumpStart && "Caller should rule out");
		assert(pJumpStart->next.load(std::memory_order_relaxed) && "caller should rule out");
		size_t jumpStartHashBucketId = computeHashBucketId(pJumpStart, hashMaskExpanded);
		RcuSlistHead* pNextJumpStart = pJumpStart->next.load(std::memory_order_relaxed);
		RcuSlistHead* pNext;
		assert(computeHashBucketId(pNextJumpStart, hashMaskExpanded) != jumpStartHashBucketId);
		while (true)
		{
			pNext = pNextJumpStart->next.load(std::memory_order_relaxed);
			if (pNext == nullptr)
			{
				// no more unzip needed for current bucket
				pNextJumpStart = nullptr;
				break;
			}
			if (computeHashBucketId(pNext, hashMaskExpanded) == jumpStartHashBucketId)
				break;	// zip target found
			pNextJumpStart = pNext;
		}
		pZipStart->head.next.store(pNextJumpStart, std::memory_order_release);
		pJumpStart->next.store(pNext, std::memory_order_release);
	}

	// initialize initial two dst buckets corresponds to one src bucket
	void initTwinDstBuckets(
			RTableCore::Bucket* pSrc,
			RTableCore::Bucket* pDst0,
			RTableCore::Bucket* pDst1,
			size_t bucketDstId0,
			size_t bucketDstId1,
			size_t bucketIdMask)
	{
		RcuSlistHead* pFirstDst0 = nullptr;
		RcuSlistHead* pFirstDst1 = nullptr;
		for (auto p = pSrc->list.head.next.load(std::memory_order_relaxed); p != nullptr;
				 p = p->next.load(std::memory_order_relaxed))
		{
			auto bucketId = computeHashBucketId(p, bucketIdMask);
			assert(bucketId == bucketDstId0 || bucketId == bucketDstId1);

			if (!pFirstDst0 && bucketId == bucketDstId0)
				pFirstDst0 = p;
			else if (!pFirstDst1 && bucketId == bucketDstId1)
				pFirstDst1 = p;

			if (pFirstDst0 && pFirstDst1)
				break;
		}
		pDst0->list.head.next.store(pFirstDst0, std::memory_order_release);
		pDst1->list.head.next.store(pFirstDst1, std::memory_order_release);
	}

	// caller should make sure that pSrc->list.next is not null
	bool findFirstUnzipPoint(RTableCore::Bucket* pSrc, size_t hashMask)
	{
		RcuSlistHead* p = pSrc->list.head.next.load(std::memory_order_relaxed);
		size_t hashBucketInitial = computeHashBucketId(p, hashMask);
		while (true)
		{
			RcuSlistHead* pNext = p->next.load(std::memory_order_relaxed);
			if (pNext == nullptr)
			{
				// no more unzip needed for current bucket
				p = nullptr;
				break;
			}
			if (computeHashBucketId(pNext, hashMask) != hashBucketInitial)
				break;	// zip target found
			p = pNext;
		}
		pSrc->list.head.next.store(p, std::memory_order_release);
		return p;
	}

	// returns if all finished
	bool findFirstUnzipStarts(RTableCore::BucketsInfo* bucketsInfoOld, size_t bucketMaskNew)
	{
		bool allFinished = true;
		for (size_t iHalf = 0; iHalf < bucketsInfoOld->nrBucketsPowerOf2; ++iHalf)
		{
			RTableCore::Bucket* pSrc = bucketsInfoOld->pBuckets + iHalf;
			if (pSrc->list.head.next.load(std::memory_order_relaxed) != nullptr)
			{
				bool needUnzip = findFirstUnzipPoint(pSrc, bucketMaskNew);
				if (needUnzip)
					allFinished = false;
			}
		}

		if (allFinished)
			return bucketsInfoOld;
		return allFinished;
	}

	void unzip(RTableCore::BucketsInfo* bucketsInfoOld, size_t bucketMaskNew, RCUZone& rcuZone)
	{
		while (true)
		{
			bool allFinished = true;
			for (size_t iHalf = 0; iHalf < bucketsInfoOld->nrBucketsPowerOf2; ++iHalf)
			{
				RTableCore::Bucket* pSrc = bucketsInfoOld->pBuckets + iHalf;
				if (pSrc->list.head.next.load(std::memory_order_relaxed) != nullptr)
				{
					allFinished = false;
					unzipOneSegment(&pSrc->list, bucketMaskNew);
				}
			}
			if (allFinished)
				break;
			rcuSynchronize(rcuZone);
		}
	}

	RTableCore::BucketsInfo* expandBucketsByFac2ReturnOld(RTableCore& table, RCUZone& zone)
	{
		auto* bucketsInfoOld = table.pBucketsInfo.load(std::memory_order_relaxed);
		size_t nrBucketsOld = bucketsInfoOld->nrBucketsPowerOf2;
		size_t nrBucketsNew = nrBucketsOld * 2;
		size_t bucketMaskNew = nrBucketsNew - 1;
		RTableCore::BucketsInfo* bucketsInfo = allocateAndInitBuckets(nrBucketsNew);
		for (size_t iHalf = 0; iHalf < nrBucketsOld; ++iHalf)
		{
			RTableCore::Bucket* pSrc = bucketsInfoOld->pBuckets + iHalf;
			RTableCore::Bucket* dst0 = bucketsInfo->pBuckets + iHalf;
			size_t iSecondHalf = iHalf + nrBucketsOld;
			RTableCore::Bucket* dst1 = bucketsInfo->pBuckets + iSecondHalf;
			initTwinDstBuckets(pSrc, dst0, dst1, iHalf, iSecondHalf, bucketMaskNew);
		}

		// publish new buckets info
		table.pBucketsInfo.store(bucketsInfo, std::memory_order_release);
		// synchronize so that no one is reading the old buckets
		// since we are going to use the old buckets for unzipping
		rcuSynchronize(zone);

		bool noNeedToUnzip = findFirstUnzipStarts(bucketsInfoOld, bucketMaskNew);
		if (noNeedToUnzip)
			return bucketsInfoOld;

		unzip(bucketsInfoOld, bucketMaskNew, zone);
		return bucketsInfoOld;
	}

	RTableCore::BucketsInfo* shrinkBucketsByFac2ReturnOld(RTableCore& table, RCUZone& rcuZone)
	{
		auto* bucketsInfoOld = table.pBucketsInfo.load(std::memory_order_relaxed);
		size_t nrBucketsOld = bucketsInfoOld->nrBucketsPowerOf2;
		size_t nrBucketsNew = nrBucketsOld / 2;
		if (nrBucketsNew == 0)
			return nullptr;
		size_t bucketMask = nrBucketsNew - 1;
		RTableCore::BucketsInfo* bucketsInfoNew = allocateAndInitBuckets(nrBucketsNew);
		for (size_t iHalf = 0; iHalf < nrBucketsNew; ++iHalf)
		{
			RTableCore::Bucket* pDst = bucketsInfoNew->pBuckets + iHalf;
			RTableCore::Bucket* pSrc0 = bucketsInfoOld->pBuckets + iHalf;
			size_t iSecondHalf = iHalf + nrBucketsNew;
			RTableCore::Bucket* pSrc1 = bucketsInfoOld->pBuckets + iSecondHalf;
			shrinkTwoBucketsToOne(&pSrc0->list, &pSrc1->list, &pDst->list);
		}
		table.pBucketsInfo.store(bucketsInfoNew, std::memory_order_release);
		rcuSynchronize(rcuZone);
		return bucketsInfoOld;
	}
}	 // namespace

void rTableCoreInitDetailed(RTableCore& table, const RTableCoreConfig& conf)
{
	auto nrBucketsPowerOf2 = upperBoundPowerOf2(conf.nrBuckets);
	RTableCore::BucketsInfo* bucketsInfo = allocateAndInitBuckets(nrBucketsPowerOf2);
	table.expandFactor = conf.expandFactor;
	table.shrinkFactor = conf.shrinkFactor;
	table.pBucketsInfo.store(bucketsInfo, std::memory_order_relaxed);
}

void rTableInitDetailed(RTable& table, const RTableConfig& conf)
{
	rcuInitZoneWithBucketCounts(table.rcuZone, conf.nrRcuBucketsForUnregisteredThreads);
	RTableCoreConfig confCore;
	confCore.expandFactor = conf.expandFactor;
	confCore.nrBuckets = conf.nrBuckets;
	confCore.shrinkFactor = conf.shrinkFactor;
	rTableCoreInitDetailed(table.core, confCore);
}

void rTableInit(RTable& table, int nrBuckets)
{
	auto nrThreadsBuckets = std::thread::hardware_concurrency() * 64;
	RTableConfig conf;
	conf.nrBuckets = nrBuckets;
	conf.nrRcuBucketsForUnregisteredThreads = nrThreadsBuckets;
	rTableInitDetailed(table, conf);
}

// can only be called if the user is sure that no dup exists
void rTableCoreInsertNoExpand(RTableCore& table, RNode* pEntry)
{
	RTableCore::BucketsInfo* pBucketsInfo = table.pBucketsInfo.load(std::memory_order_relaxed);
	auto bucketMask = pBucketsInfo->nrBucketsPowerOf2 - 1;
	size_t bucketId = pEntry->hash & bucketMask;
	RTableCore::Bucket* pBucket = pBucketsInfo->pBuckets + bucketId;
	pEntry->head.next.store(pBucket->list.head.next, std::memory_order_release);
	pBucket->list.head.next.store(&pEntry->head, std::memory_order_release);
	table.size.fetch_add(1, std::memory_order_relaxed);
}

int64_t rTableReadLock(RTable& table)
{
	return rcuReadLock(table.rcuZone);
}

void rTableReadUnlock(RTable& table, int64_t epoch)
{
	rcuReadUnlock(table.rcuZone, epoch);
}

void rTableSynchronize(RTable& table)
{
	rcuSynchronize(table.rcuZone);
}

void rTableCoreExpandBuckets2x(RTableCore& table, RCUZone& zone)
{
	auto pOldInfo = expandBucketsByFac2ReturnOld(table, zone);
	destroyAndFreeBuckets(pOldInfo);
}

void rTableExpandBuckets2x(RTable& table)
{
	rTableCoreExpandBuckets2x(table.core, table.rcuZone);
}

bool rTableCoreShrinkBuckets2x(RTableCore& table, RCUZone& zone)
{
	auto pOldInfo = shrinkBucketsByFac2ReturnOld(table, zone);
	if (!pOldInfo)
		return false;
	destroyAndFreeBuckets(pOldInfo);
	return true;
}

bool rTableShrinkBuckets2x(RTable& table)
{
	return rTableCoreShrinkBuckets2x(table.core, table.rcuZone);
}

RTableCore::~RTableCore()
{
	auto* p = pBucketsInfo.load();
	if (p)
		destroyAndFreeBuckets(p);
}

namespace rTableCoreDetail
{
	void expandBucketsByFac2IfNecessary(
			size_t nrElements,
			size_t nrBuckets,
			RTableCore& table,
			RCUZone& zone)
	{
		if ((float)nrElements > table.expandFactor * float(nrBuckets))
			rTableCoreExpandBuckets2x(table, zone);
	}

	bool shrinkBucketsByFac2IfNecessary(
			size_t nrElements,
			size_t nrBuckets,
			RTableCore& table,
			RCUZone& zone)
	{
		if ((float)nrElements < table.shrinkFactor * float(nrBuckets) && nrElements > 128)
			return rTableCoreShrinkBuckets2x(table, zone);
		return false;
	}
}	 // namespace rTableCoreDetail
}	 // namespace yrcu