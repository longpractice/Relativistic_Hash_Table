#pragma once
#include "RCUHashTableTypes.h"
#include "RCUApi.h"
#include <iostream>
namespace yrcu
{
    namespace rcuHashTableDetail
    {
        void expandBucketsByFac2IfNecessary(size_t nrElements, size_t nrBuckets, RcuHashTable& table);
        bool shrinkBucketsByFac2IfNecessary(size_t nrElements, size_t nrBuckets, RcuHashTable& table);
    }



    ///////////////////////////////////////////////////////////////////////////////////////////
    //--------------------------Advanced API-------------------------------------------------//
    ///////////////////////////////////////////////////////////////////////////////////////////


    struct RcuHashTableConfig
    {
        int nrBuckets = 64;
        int nrRcuBucketsForUnregisteredThreads = 128;
        float expandFactor = 1.1f;
        float shrinkFactor = 0.25f;
    };

    void rcuHashTableInitDetailed(RcuHashTable& table, const RcuHashTableConfig& conf);

    //Op is of function signature of bool(RNode* p0), which returns if the entry is what you are looking
    //try erase but no synchronize
    //This enables the caller to do several rcuHashTableTryDetach operations, do one rcuHashTableSynchronize and then
    //do all the garbage collections.
    template<typename UnaryPredicate>
    RNode* rcuHashTableTryDetachNoShrink(RcuHashTable& table, size_t hashVal, UnaryPredicate matchOp)
    {
        RcuHashTable::BucketsInfo* pBucketsInfo = table.pBucketsInfo.load(std::memory_order_acquire);
        size_t bucketHash = pBucketsInfo->nrBucketsPowerOf2 - 1;
        auto bucketId = hashVal & bucketHash;
        RcuHashTable::Bucket* pBucket = pBucketsInfo->pBuckets + bucketId;
        AtomicSingleHead* pLast = &pBucket->list;
        for (AtomicSingleHead* p = pLast->next.load(std::memory_order_relaxed); p != nullptr;)
        {
            AtomicSingleHead* pNext = p->next.load(std::memory_order_relaxed);
            RNode* pEntry = YJ_CONTAINER_OF(p, RNode, head);
            if (pEntry->hash == hashVal && matchOp(pEntry))
            {
                pLast->next.store(pNext, std::memory_order_release);
                return pEntry;
            }
            pLast = p;
            p = pNext;
        }
        return nullptr;
    }

    //might shrink automatically
    //But will not do any synchronization
    template<typename Op>
    RNode* rcuHashTableTryDetachAutoShrink(RcuHashTable& table, size_t hashVal, Op matchOp, bool* outIfAlreadyRcuSynrhonized = nullptr)
    {
        RNode* p = rcuHashTableTryDetachNoShrink(table, hashVal, std::move(matchOp));
        if (!p)
        {
            if (outIfAlreadyRcuSynrhonized)
                *outIfAlreadyRcuSynrhonized = false;
            return p;
        }
        else
        {
            size_t currentSize = table.size.load(std::memory_order_relaxed);
            size_t nrBuckets = table.pBucketsInfo.load(std::memory_order_acquire)->nrBucketsPowerOf2;
            bool ifAlreadyRcuSynrhonized = rcuHashTableDetail::shrinkBucketsByFac2IfNecessary(currentSize, nrBuckets, table);
            if (outIfAlreadyRcuSynrhonized)
                *outIfAlreadyRcuSynrhonized = ifAlreadyRcuSynrhonized;
            return p;
        }
    }

    template<typename Op>
    bool rcuHashTableTryInsertNoExpand(RcuHashTable& table, RNode* pEntry, size_t hashVal, Op matchOp)
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
            RNode* pEntryExisting = YJ_CONTAINER_OF(p, RNode, head);
            if (pEntry->hash == pEntryExisting->hash && matchOp(pEntry, pEntryExisting))
                break;
        }
        if (p != nullptr)
            return false;
        //put the new element in the front
        pEntry->head.next.store(pFirst, std::memory_order_release);
        pBucket->list.next.store(&pEntry->head, std::memory_order_release);
        table.size.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    //it is allowed to have multiple detach before a single synchronize operation
    //and after the synchronize operation, the detached nodes can be safely freed.
    void rcuHashTableSynchronize(RcuHashTable& table);

    //can only be called if the user is sure that no dup exists
    void rcuHashTableInsertNoExpand(RcuHashTable& table, RNode* pEntry);

    void rcuHashTableExpandBuckets2x(RcuHashTable& table);

    bool rcuHashTableShrinkBuckets2x(RcuHashTable& table);
    
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
    //op should have function signature of bool(RNode*) to identify if the RNode equals to what is to be found
    template<typename Op>
    RNode* rcuHashTableFind(const RcuHashTable& table, size_t hashVal, Op matchOp)
    {
        RcuHashTable::BucketsInfo* pBucketsInfo = table.pBucketsInfo.load(std::memory_order_acquire);
        size_t bucketHash = pBucketsInfo->nrBucketsPowerOf2 - 1;
        auto bucketId = hashVal & bucketHash;
        RcuHashTable::Bucket* pBucket = pBucketsInfo->pBuckets + bucketId;
        for (auto p = pBucket->list.next.load(std::memory_order_acquire); p != nullptr; p = p->next.load(std::memory_order_acquire))
        {
            RNode* pEntry = YJ_CONTAINER_OF(p, RNode, head);
            if (pEntry->hash == hashVal && matchOp(pEntry))
                return pEntry;
        }
        return nullptr;
    }

    //Write operation: all writers must be serialized
    //Op is of function signature of bool(RNode* p0, RcuHashTaleEntry* p1), which returns if
    //two hash table entries are equivalent.
    //Expand if necessary
    template<typename Op>
    bool rcuHashTableTryInsert(RcuHashTable& table, RNode* pEntry, size_t hashVal, Op matchOp)
    {
        bool inserted = rcuHashTableTryInsertNoExpand(table, pEntry, hashVal, matchOp);
        if (!inserted)
            return false;
        auto currentSize = table.size.load(std::memory_order_relaxed);
        size_t nrBuckets = table.pBucketsInfo.load(std::memory_order_acquire)->nrBucketsPowerOf2;
        rcuHashTableDetail::expandBucketsByFac2IfNecessary(currentSize, nrBuckets, table);
        return true;
    }

    //Write operation: all writers must be serialized
    //Op is of function signature of bool(RNode* p0), which returns if the entry is what you are looking
    //for.
    //rcuSynchronize is called internally and it is safe to delete resource of RNode.
    template<typename Op>
    RNode* rcuHashTableTryDetachAndSynchronize(RcuHashTable& table, size_t hashVal, Op matchOp)
    {
        bool ifAlreadyRcuSynchronized = false;
        RNode* pEntry = rcuHashTableTryDetachAutoShrink(table, hashVal, matchOp, &ifAlreadyRcuSynchronized);
        if (pEntry == nullptr)
            return pEntry;
        if (!ifAlreadyRcuSynchronized)
            rcuSynchronize(table.rcuZone);
        return pEntry;
    }
}

