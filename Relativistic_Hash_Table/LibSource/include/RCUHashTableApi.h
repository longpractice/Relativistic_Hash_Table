#pragma once
#include "RCUHashTableTypes.h"
#include "RCUHashTableCoreApi.h"
#include "RCUApi.h"

namespace yrcu
{
    ///////////////////////////////////////////////////////////////////////////////////////////
    //--------------------------Advanced API-------------------------------------------------//
    ///////////////////////////////////////////////////////////////////////////////////////////
    struct RTableConfig
    {
        int nrBuckets = 64;
        int nrRcuBucketsForUnregisteredThreads = 128;
        float expandFactor = 1.1f;
        float shrinkFactor = 0.25f;
    };

    void rTableInitDetailed(RTable& table, const RTableConfig& conf);

    //Op is of function signature of bool(RNode* p0), which returns if the entry is what you are looking
    //try erase but no synchronize
    //This enables the caller to do several rTableTryDetach operations, do one rTableSynchronize and then
    //do all the garbage collections.
    template<typename UnaryPredicate>
    RNode* rTableTryDetachNoShrink(RTable& table, size_t hashVal, UnaryPredicate matchOp)
    {
        rTableCoreCoreTryDetachNoShrink(table.core, hashVal, std::move(matchOp));
    }

    //might shrink automatically
    //But will not do any synchronization
    template<typename Op>
    RNode* rTableTryDetachAutoShrink(RTable& table, size_t hashVal, Op matchOp, bool* outIfAlreadyRcuSynrhonized = nullptr)
    {
        return rTableCoreTryDetachAutoShrink(table.core, hashVal, matchOp, table.rcuZone, outIfAlreadyRcuSynrhonized);
    }

    template<typename Op>
    bool rTableTryInsertNoExpand(RTable& table, RNode* pEntry, size_t hashVal, Op matchOp)
    {
        return rTableCoreTryInsertNoExpand(table.core, pEntry, hashVal, matchOp, table.rcuZone);
    }

    //it is allowed to have multiple detach before a single synchronize operation
    //and after the synchronize operation, the detached nodes can be safely freed.
    void rTableSynchronize(RTable& table);

    //can only be called if the user is sure that no dup exists
    void rTableInsertNoExpand(RTable& table, RNode* pEntry);

    void rTableExpandBuckets2x(RTable& table);

    bool rTableShrinkBuckets2x(RTable& table);

    //-----------------------------------------------------------------------------------------------//


    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //--------------------------------Basic API------------------------------------------------------//
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //hash table must be initialized, nrBuckets is advised to be bigger than the predicted element count
    void rTableInit(RTable& table, int nrBuckets = 64);

    //read lock and unlock creates a RCU read critical session
    //the writer waits for all the critical sessions for the epoch to finish 
    //before deleting the resource
    //returns the epoch to be passed into the rTableReadUnlock
    int64_t rTableReadLock(RTable& table);
    void rTableReadUnlock(RTable& table, int64_t epoch);
    struct RTableReadLockGuard
    {
        explicit RTableReadLockGuard(RTable& table) :tbl{ table }
        {
            epoch = rTableReadLock(table);
        }
        RTableReadLockGuard(const RTableReadLockGuard&) = delete;
        RTableReadLockGuard(RTableReadLockGuard&&) = delete;
        RTableReadLockGuard& operator=(const RTableReadLockGuard&) = delete;
        RTableReadLockGuard& operator=(RTableReadLockGuard&&) = delete;

        ~RTableReadLockGuard()
        {
            rTableReadUnlock(tbl, epoch);
        }
        RTable& tbl;
        int64_t epoch = 0;
    };

    //Read operation
    //\parameter hashVal should be the hash value of the find target, only table entries with a hash value equals to `hashVal` is checked for
    // equality with matchOp.
    //op should have function signature of bool(RNode*) to identify if the RNode equals to what is to be found
    template<typename Op>
    RNode* rTableFind(const RTable& table, size_t hashVal, Op matchOp)
    {
        RTableCore::BucketsInfo* pBucketsInfo = table.pBucketsInfo.load(std::memory_order_acquire);
        size_t bucketHash = pBucketsInfo->nrBucketsPowerOf2 - 1;
        auto bucketId = hashVal & bucketHash;
        RTableCore::Bucket* pBucket = pBucketsInfo->pBuckets + bucketId;
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
    bool rTableTryInsert(RTable& table, RNode* pEntry, size_t hashVal, Op matchOp)
    {
        bool inserted = rTableTryInsertNoExpand(table, pEntry, hashVal, matchOp);
        if (!inserted)
            return false;
        auto currentSize = table.size.load(std::memory_order_relaxed);
        size_t nrBuckets = table.pBucketsInfo.load(std::memory_order_acquire)->nrBucketsPowerOf2;
        rTableDetail::expandBucketsByFac2IfNecessary(currentSize, nrBuckets, table);
        return true;
    }

    //Write operation: all writers must be serialized
    //Op is of function signature of bool(RNode* p0), which returns if the entry is what you are looking
    //for.
    //rcuSynchronize is called internally and it is safe to delete resource of RNode.
    template<typename Op>
    RNode* rTableTryDetachAndSynchronize(RTable& table, size_t hashVal, Op matchOp)
    {
        bool ifAlreadyRcuSynchronized = false;
        RNode* pEntry = rTableTryDetachAutoShrink(table, hashVal, matchOp, &ifAlreadyRcuSynchronized);
        if (pEntry == nullptr)
            return pEntry;
        if (!ifAlreadyRcuSynchronized)
            rcuSynchronize(table.rcuZone);
        return pEntry;
    }
}

