#include "RCUApi.h"
#include "RCUTypes.h"
#include "RCUHashTableApi.h"
#include "RCUHashTableTypes.h"
#include <thread>
#include <cassert>

namespace yj
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

        RcuHashTable::BucketsInfo* allocateAndInitBuckets(size_t nrBucketsPowerOf2)
        {
            assert(nrBucketsPowerOf2 > 0);
            //allocate buckets info and the buckets together, better cache perf
            size_t allocSize = sizeof(RcuHashTable::BucketsInfo) + nrBucketsPowerOf2 * sizeof(RcuHashTable::Bucket);
            void* p = malloc(allocSize);

            RcuHashTable::BucketsInfo* pBucketsInfo = new (p) RcuHashTable::BucketsInfo();
            pBucketsInfo->nrBucketsPowerOf2 = nrBucketsPowerOf2;

            char* pBucketsChar = (char*)p + sizeof(RcuHashTable::BucketsInfo);
            RcuHashTable::Bucket* pBuckets = new (pBucketsChar)RcuHashTable::Bucket[nrBucketsPowerOf2];

            pBucketsInfo->pBuckets = pBuckets;
            //seems that for some implemenation, we cannot assume atomic int is triavially constructable
            //otherwise, we should just do a memset to 0;
            for (int iBucket = 0; iBucket < nrBucketsPowerOf2; ++iBucket)
                (pBuckets + iBucket)->list.next.store(nullptr, std::memory_order_release);
            return pBucketsInfo;
        }

        void destroyAndFreeBuckets(RcuHashTable::BucketsInfo* p)
        {
            static_assert(std::is_trivially_destructible_v<RcuHashTable::Bucket>);
            static_assert(std::is_trivially_destructible_v<RcuHashTable::BucketsInfo>);
            free(p);
        }

        //pSrc0->next is pointing to the 0th element of the src0
        //pSrc1->next is pointing to the 0th element of the src1
        //pSrc and pSrc1 are not nullptr themselves
        void shrinkTwoBucketsToOne(AtomicSingleHead* pSrc0, AtomicSingleHead* pSrc1, AtomicSingleHead* pDst)
        {
            AtomicSingleHead* pSrc0First = pSrc0->next.load(std::memory_order_relaxed);
            AtomicSingleHead* pSrc1First = pSrc1->next.load(std::memory_order_relaxed);

            //Splice list 1 onto the end of list 0, and then let pDst point to the first element of list 0.
            //This would mean that we need to walk list 0 to find its tail.
            //But we could skip that if list 1 is empty.
            if (pSrc1First == nullptr)
            {
                pDst->next.store(pSrc0First, std::memory_order_release);
                return;
            }

            //If list 0 is empty, we directly let the pDst point to the first element of list 1.
            if (pSrc0First == nullptr)
            {
                pDst->next.store(pSrc1First, std::memory_order_release);
                return;
            }

            //search so that p is the last element of src0
            AtomicSingleHead* pSrc0Last = pSrc0First;
            while (true)
            {
                AtomicSingleHead* pNext = pSrc0Last->next.load(std::memory_order_relaxed);
                if (pNext == nullptr)
                    break;
                pSrc0Last = pNext;
            }

            pSrc0Last->next.store(pSrc1First, std::memory_order_release);
            pDst->next.store(pSrc0First, std::memory_order_release);
        }

        size_t computeHashBucketId(AtomicSingleHead* p, size_t hashMask)
        {
            return YJ_CONTAINER_OF(p, RcuHashTableEntry, head)->hash & hashMask;
        }

        //caller should make sure that pZipStart->next is not null
        void unzipOneSegment(AtomicSingleHead* pZipStart, size_t hashMaskExpanded)
        {
            //pZipStart.next points to an element whose next has a different expanded hash bucket
            // 
            //An example old hash bucket's element chain(4 segments):
            //|--seg0--|  |--Seg1--|  |--Seg2--|  |--Seg3---|
            // x   x  X    y y  y Y    x  x   x    y y y y y
            //(All x or X above will hash to the same expanded hash bucket.)
            //(All y or Y above will hash to the same expanded hash bucket.)
            //
            //Before this function pZipStart->next points to X (jumpStart, last element of seg0)
            //and yyyY(Seg1) are guaranteed to be reached from readers starting from a correct expanded hash bucket.
            //            
            //After this function pZipStart->next points to Y (newJumpStart, end of seg1) and X->next points to the first x of seg2.
            //Thus yyyY is "unzipped"
            //
            //After this function, the caller needs to call rcuSynchronize() to ensure that all the readers reading
            //xxx(seg2) is doing so through a correct expanded hash bucket
            //(otherwise, since some reader looking for elements in seg2 might be still traversing seg1, next unzip to let
            // Y to point to first y in Seg3 will make these readers not reaching what they want)
            AtomicSingleHead* pJumpStart = pZipStart->next.load(std::memory_order_relaxed);
            assert(pJumpStart && "Caller should rule out");
            assert(pJumpStart->next.load(std::memory_order_relaxed) && "caller should rule out");
            size_t jumpStartHashBucketId = computeHashBucketId(pJumpStart, hashMaskExpanded);
            AtomicSingleHead* pNextJumpStart = pJumpStart->next.load(std::memory_order_relaxed);
            AtomicSingleHead* pNext;
            assert(computeHashBucketId(pNextJumpStart, hashMaskExpanded) != jumpStartHashBucketId);
            while (true)
            {
                pNext = pNextJumpStart->next.load(std::memory_order_relaxed);
                if (pNext == nullptr)
                {
                    //no more unzip needed for current bucket
                    pNextJumpStart = nullptr;
                    break;
                }
                if (computeHashBucketId(pNext, hashMaskExpanded) == jumpStartHashBucketId)
                    break; //zip target found
                pNextJumpStart = pNext;
            }
            pZipStart->next.store(pNextJumpStart, std::memory_order_release);
            pJumpStart->next.store(pNext, std::memory_order_release);
        }

        //initialize initial two dst buckets corresponds to one src bucket
        void initTwinDstBuckets(
            RcuHashTable::Bucket* pSrc,
            RcuHashTable::Bucket* pDst0,
            RcuHashTable::Bucket* pDst1,
            size_t bucketDstId0,
            size_t bucketDstId1,
            size_t bucketIdMask
        )
        {
            AtomicSingleHead* pFirstDst0 = nullptr;
            AtomicSingleHead* pFirstDst1 = nullptr;
            for (auto p = pSrc->list.next.load(std::memory_order_relaxed); p != nullptr; p = p->next.load(std::memory_order_relaxed))
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
            pDst0->list.next.store(pFirstDst0, std::memory_order_release);
            pDst1->list.next.store(pFirstDst1, std::memory_order_release);
        }

        //caller should make sure that pSrc->list.next is not null
        bool findFirstUnzipPoint(RcuHashTable::Bucket* pSrc, size_t hashMask)
        {
            AtomicSingleHead* p = pSrc->list.next.load(std::memory_order_relaxed);
            size_t hashBucketInitial = computeHashBucketId(p, hashMask);
            while (true)
            {
                AtomicSingleHead* pNext = p->next.load(std::memory_order_relaxed);
                if (pNext == nullptr)
                {
                    //no more unzip needed for current bucket
                    p = nullptr;
                    break;
                }
                if (computeHashBucketId(pNext, hashMask) != hashBucketInitial)
                    break; //zip target found
                p = pNext;
            }
            pSrc->list.next.store(p, std::memory_order_release);
            return p;
        }

        //returns if all finished
        bool findFirstUnzipStarts(RcuHashTable::BucketsInfo* bucketsInfoOld, size_t bucketMaskNew)
        {
            bool allFinished = true;
            for (size_t iHalf = 0; iHalf < bucketsInfoOld->nrBucketsPowerOf2; ++iHalf)
            {
                RcuHashTable::Bucket* pSrc = bucketsInfoOld->pBuckets + iHalf;
                if (pSrc->list.next.load(std::memory_order_relaxed) != nullptr)
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

        void unzip(RcuHashTable::BucketsInfo* bucketsInfoOld, size_t bucketMaskNew, RCUZone& rcuZone)
        {
            while (true)
            {
                bool allFinished = true;
                for (size_t iHalf = 0; iHalf < bucketsInfoOld->nrBucketsPowerOf2; ++iHalf)
                {
                    RcuHashTable::Bucket* pSrc = bucketsInfoOld->pBuckets + iHalf;
                    if (pSrc->list.next.load(std::memory_order_relaxed) != nullptr)
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

        RcuHashTable::BucketsInfo* expandBucketsByFac2ReturnOld(RcuHashTable& table)
        {
            auto* bucketsInfoOld = table.pBucketsInfo.load(std::memory_order_relaxed);
            size_t nrBucketsOld = bucketsInfoOld->nrBucketsPowerOf2;
            size_t nrBucketsNew = nrBucketsOld * 2;
            size_t bucketMaskNew = nrBucketsNew - 1;
            RcuHashTable::BucketsInfo* bucketsInfo = allocateAndInitBuckets(nrBucketsNew);
            for (size_t iHalf = 0; iHalf < nrBucketsOld; ++iHalf)
            {
                RcuHashTable::Bucket* pSrc = bucketsInfoOld->pBuckets + iHalf;
                RcuHashTable::Bucket* dst0 = bucketsInfo->pBuckets + iHalf;
                size_t iSecondHalf = iHalf + nrBucketsOld;
                RcuHashTable::Bucket* dst1 = bucketsInfo->pBuckets + iSecondHalf;
                initTwinDstBuckets(pSrc, dst0, dst1, iHalf, iSecondHalf, bucketMaskNew);
            }

            //publish new buckets info
            table.pBucketsInfo.store(bucketsInfo, std::memory_order_release);
            //synchronize so that no one is reading the old buckets
            //since we are going to use the old buckets for unzipping
            rcuSynchronize(table.rcuZone);

            bool noNeedToUnzip = findFirstUnzipStarts(bucketsInfoOld, bucketMaskNew);
            if (noNeedToUnzip)
                return bucketsInfoOld;

            unzip(bucketsInfoOld, bucketMaskNew, table.rcuZone);
            return bucketsInfoOld;
        }

        RcuHashTable::BucketsInfo* shrinkBucketsByFac2ReturnOld(RcuHashTable& table)
        {
            auto* bucketsInfoOld = table.pBucketsInfo.load(std::memory_order_relaxed);
            size_t nrBucketsOld = bucketsInfoOld->nrBucketsPowerOf2;
            size_t nrBucketsNew = nrBucketsOld / 2;
            size_t bucketMask = nrBucketsNew - 1;
            RcuHashTable::BucketsInfo* bucketsInfoNew = allocateAndInitBuckets(nrBucketsNew);
            for (size_t iHalf = 0; iHalf < nrBucketsNew; ++iHalf)
            {
                RcuHashTable::Bucket* pDst = bucketsInfoNew->pBuckets + iHalf;
                RcuHashTable::Bucket* pSrc0 = bucketsInfoOld->pBuckets + iHalf;
                size_t iSecondHalf = iHalf + nrBucketsNew;
                RcuHashTable::Bucket* pSrc1 = bucketsInfoOld->pBuckets + iSecondHalf;
                shrinkTwoBucketsToOne(&pSrc0->list, &pSrc1->list, &pDst->list);
            }
            table.pBucketsInfo.store(bucketsInfoNew, std::memory_order_release);
            rcuSynchronize(table.rcuZone);
            return bucketsInfoOld;
        }
    }

    void rcuHashTableInitWithRcuBucketsCount(RcuHashTable& table, int nrBuckets, int nrRcuBucketsForUnregisteredThreads)
    {
        rcuInitZoneWithBucketCounts(table.rcuZone, nrRcuBucketsForUnregisteredThreads);
        auto nrBucketsPowerOf2 = upperBoundPowerOf2(nrBuckets);
        RcuHashTable::BucketsInfo* bucketsInfo = allocateAndInitBuckets(nrBucketsPowerOf2);
        table.pBucketsInfo.store(bucketsInfo, std::memory_order_relaxed);
    }

    void rcuHashTableInit(RcuHashTable& table, int nrBuckets)
    {
        auto nrThreadsBuckets = std::thread::hardware_concurrency() * 64;
        rcuHashTableInitWithRcuBucketsCount(table, nrBuckets, nrThreadsBuckets);
    }

    //can only be called if the user is sure that no dup exists
    void rcuHashTableInsert(RcuHashTable& table, RcuHashTableEntry* pEntry)
    {
        RcuHashTable::BucketsInfo* pBucketsInfo = table.pBucketsInfo.load(std::memory_order_relaxed);
        auto bucketMask = pBucketsInfo->nrBucketsPowerOf2 - 1;
        size_t bucketId = pEntry->hash & bucketMask;
        RcuHashTable::Bucket* pBucket = pBucketsInfo->pBuckets + bucketId;
        pEntry->head.next.store(pBucket->list.next, std::memory_order_release);
        pBucket->list.next.store(&pEntry->head, std::memory_order_release);
        auto oldSize = table.size.fetch_add(1, std::memory_order_relaxed);
        if (pBucketsInfo->nrBucketsPowerOf2 <= oldSize)
            rcuHashTableDetail::expandBucketsByFac2(table);
    }

    int64_t rcuHashTableReadLock(RcuHashTable& table)
    {
        return rcuReadLock(table.rcuZone);
    }
    void rcuHashTableReadUnlock(RcuHashTable& table, int64_t epoch)
    {
        rcuReadUnlock(table.rcuZone, epoch);
    }
    void rcuHashTableSynchronize(RcuHashTable& table)
    {
        rcuSynchronize(table.rcuZone);
    }

    namespace rcuHashTableDetail
    {
        void expandBucketsByFac2(RcuHashTable& table)
        {
            auto pOldInfo = expandBucketsByFac2ReturnOld(table);
            destroyAndFreeBuckets(pOldInfo);
        }

        void shrinkBucketsByFac2(RcuHashTable& table)
        {
            auto pOldInfo = shrinkBucketsByFac2ReturnOld(table);
            destroyAndFreeBuckets(pOldInfo);
        }
    }
}