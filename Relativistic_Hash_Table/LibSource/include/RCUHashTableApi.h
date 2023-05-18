#pragma once
#include "RCUHashTableTypes.h"
#include "RCUApi.h"
namespace yj
{
    namespace rcuHashTableDetail
    {
        void expandBucketsByFac2IfNecessary(size_t nrElements, size_t nrBuckets, RcuHashTable& table);
        bool shrinkBucketsByFac2IfNecessary(size_t nrElements, size_t nrBuckets, RcuHashTable& table);
    }
    
    ///////////////////////////////////////////////////////////////////////////////////////////
    //--------------------------Advanced API-------------------------------------------------//
    ///////////////////////////////////////////////////////////////////////////////////////////
    template<typename Op>
    RcuHashTableEntry* rcuHashTableTryDetach(RcuHashTable& table, size_t hashVal, Op matchOp, bool& outIfAlreadyRcuSynrhonized)
    {
        RcuHashTable::BucketsInfo* pBucketsInfo = table.pBucketsInfo.load(std::memory_order_acquire);
        size_t bucketHash = pBucketsInfo->nrBucketsPowerOf2 - 1;
        auto bucketId = hashVal & bucketHash;
        RcuHashTable::Bucket* pBucket = pBucketsInfo->pBuckets + bucketId;
        AtomicSingleHead* pLast = &pBucket->list;
        for (AtomicSingleHead* p = pLast->next.load(std::memory_order_relaxed); p != nullptr; )
        {
            AtomicSingleHead* pNext = p->next.load(std::memory_order_relaxed);
            RcuHashTableEntry* pEntry = YJ_CONTAINER_OF(p, RcuHashTableEntry, head);
            if (pEntry->hash == hashVal && matchOp(pEntry))
            {
                pLast->next.store(pNext, std::memory_order_release);
                auto oldSize = table.size.fetch_add(-1, std::memory_order_relaxed);
                //shrinking happens much less often
                outIfAlreadyRcuSynrhonized = rcuHashTableDetail::shrinkBucketsByFac2IfNecessary(oldSize, pBucketsInfo->nrBucketsPowerOf2, table);
                return pEntry;
            }
            pLast = p;
            p = pNext;
        }
        return nullptr;
    }
    
    //it is allowed to have multiple detach before a single synchronize operation
    //and after the synchronize operation, the detached nodes can be safely freed.
    void rcuHashTableSynchronize(RcuHashTable& table);
    
    //can only be called if the user is sure that no dup exists
    void rcuHashTableInsert(RcuHashTable& table, RcuHashTableEntry* pEntry);
    void rcuHashTableInitWithRcuBucketsCount(RcuHashTable& table, int nrBuckets, int nrRcuBucketsForUnregisteredThreads);
    //-----------------------------------------------------------------------------------------------//


    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //--------------------------------Basic API------------------------------------------------------//
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //hash table must be initialized, nrBuckets is advised to be bigger than the predicted element count
    void rcuHashTableInit(RcuHashTable& table, int nrBuckets = 64);

    //read lock and unlock creates a RCU read critical session
    //the writer waits for all the critical sessions for the epoch to finish 
    //before deleting the resource
    //returns the epoch to be passed into the rcuHashTableReadUnlock
    int64_t rcuHashTableReadLock(RcuHashTable& table);
    void rcuHashTableReadUnlock(RcuHashTable& table, int64_t epoch);
    struct RcuHashTableReadLockGuard
    {
        explicit RcuHashTableReadLockGuard(RcuHashTable& table) :tbl{ table }
        {
            epoch = rcuHashTableReadLock(table);
        }
        RcuHashTableReadLockGuard(const RcuHashTableReadLockGuard&) = delete;
        RcuHashTableReadLockGuard(RcuHashTableReadLockGuard&&) = delete;
        RcuHashTableReadLockGuard& operator=(const RcuHashTableReadLockGuard&) = delete;
        RcuHashTableReadLockGuard& operator=(RcuHashTableReadLockGuard&&) = delete;

        ~RcuHashTableReadLockGuard()
        {
            rcuHashTableReadUnlock(tbl, epoch);
        }
        RcuHashTable& tbl;
        int64_t epoch = 0;
    };
    //Read operation
    //\parameter hashVal should be the hash value of the find target, only table entries with a hash value equals to `hashVal` is checked for
    // equality with matchOp.
    //op should have function signature of bool(RcuHashTableEntry*) to identify if the RcuHashTableEntry equals to what is to be found
    template<typename Op>
    RcuHashTableEntry* rcuHashTableFind(const RcuHashTable& table, size_t hashVal, Op matchOp)
    {
        RcuHashTable::BucketsInfo* pBucketsInfo = table.pBucketsInfo.load(std::memory_order_acquire);
        size_t bucketHash = pBucketsInfo->nrBucketsPowerOf2 - 1;
        auto bucketId = hashVal & bucketHash;
        RcuHashTable::Bucket* pBucket = pBucketsInfo->pBuckets + bucketId;
        for (auto p = pBucket->list.next.load(std::memory_order_acquire); p != nullptr; p = p->next.load(std::memory_order_acquire))
        {
            RcuHashTableEntry* pEntry = YJ_CONTAINER_OF(p, RcuHashTableEntry, head);
            if (pEntry->hash == hashVal && matchOp(pEntry))
                return pEntry;
        }
        return nullptr;
    }

    //Write operation: all writers must be serialized
    //Op is of function signature of bool(RcuHashTableEntry* p0, RcuHashTaleEntry* p1), which returns if
    //two hash table entries are equivalent.
    template<typename Op>
    bool rcuHashTableTryInsert(RcuHashTable& table, RcuHashTableEntry* pEntry, size_t hashVal, Op matchOp)
    {
        pEntry->hash = hashVal;
        RcuHashTable::BucketsInfo* pBucketsInfo = table.pBucketsInfo.load(std::memory_order_relaxed);
        size_t bucketMask = pBucketsInfo->nrBucketsPowerOf2 - 1;
        auto bucketId = pEntry->hash & bucketMask;
        RcuHashTable::Bucket* pBucket = pBucketsInfo->pBuckets + bucketId;
        AtomicSingleHead* pFirst = pBucket->list.next.load(std::memory_order_relaxed);
        AtomicSingleHead* p;
        for (p = pFirst; p != nullptr; p = p->next.load(std::memory_order_relaxed))
        {
            RcuHashTableEntry* pEntryExisting = YJ_CONTAINER_OF(p, RcuHashTableEntry, head);
            if (pEntry->hash == pEntryExisting->hash && matchOp(pEntry, pEntryExisting))
                break;
        }
        if (p != nullptr)
            return false;
        //put the new element in the front
        pEntry->head.next.store(pFirst, std::memory_order_release);
        pBucket->list.next.store(&pEntry->head, std::memory_order_release);
        auto oldSize = table.size.fetch_add(1, std::memory_order_relaxed);
        rcuHashTableDetail::expandBucketsByFac2IfNecessary(oldSize, pBucketsInfo->nrBucketsPowerOf2, table);
        return true;
    }

    //Write operation: all writers must be serialized
    //Op is of function signature of bool(RcuHashTableEntry* p0), which returns if the entry is what you are looking
    //for.
    //rcuSynchronize is called internally and it is safe to delete resource of RcuHashTableEntry.
    template<typename Op>
    RcuHashTableEntry* rcuHashTableTryDetachAndSynchronize(RcuHashTable& table, size_t hashVal, Op matchOp)
    {
        bool ifAlreadyRcuSynchronized = false;
        RcuHashTableEntry* pEntry = rcuHashTableTryDetach(table, hashVal, matchOp, ifAlreadyRcuSynchronized);
        if (pEntry == nullptr)
            return pEntry;
        if (!ifAlreadyRcuSynchronized)
            rcuSynchronize(table.rcuZone);
        return pEntry;
    }
    //---------------------------------------------------------------------------------------------//
}

