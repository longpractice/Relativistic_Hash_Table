#pragma once
#include "RCUHashTableTypes.h"
#include "RCUApi.h"

namespace yrcu
{
    namespace rTableCoreDetail
    {
        void expandBucketsByFac2IfNecessary(size_t nrElements, size_t nrBuckets, RTableCore& table, RCUZone& zone);
        bool shrinkBucketsByFac2IfNecessary(size_t nrElements, size_t nrBuckets, RTableCore& table, RCUZone& zone);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    //--------------------------Advanced API-------------------------------------------------//
    ///////////////////////////////////////////////////////////////////////////////////////////


    struct RTableCoreConfig
    {
        int nrBuckets = 64;
        float expandFactor = 1.1f;
        float shrinkFactor = 0.25f;
    };

    void rTableCoreInitDetailed(RTableCore& table, const RTableCoreConfig& conf);

    //Op is of function signature of bool(RNode* p0), which returns if the entry is what you are looking
    //try erase but no synchronize
    //This enables the caller to do several rcuHashTableTryDetach operations, do one rcuHashTableSynchronize and then
    //do all the garbage collections.
    template<typename UnaryPredicate>
    RNode* rTableCoreTryDetachNoShrink(RTableCore& table, size_t hashVal, UnaryPredicate matchOp)
    {
        RTableCore::BucketsInfo* pBucketsInfo = table.pBucketsInfo.load(std::memory_order_acquire);
        size_t bucketHash = pBucketsInfo->nrBucketsPowerOf2 - 1;
        auto bucketId = hashVal & bucketHash;
        RTableCore::Bucket* pBucket = pBucketsInfo->pBuckets + bucketId;
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
    RNode* rTableCoreTryDetachAutoShrink(RTableCore& table, size_t hashVal, Op matchOp, RCUZone& zone, bool* outIfAlreadyRcuSynrhonized = nullptr)
    {
        RNode* p = rTableCoreTryDetachNoShrink(table, hashVal, std::move(matchOp));
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
            bool ifAlreadyRcuSynrhonized = rTableCoreDetail::shrinkBucketsByFac2IfNecessary(currentSize, nrBuckets, table, zone);
            if (outIfAlreadyRcuSynrhonized)
                *outIfAlreadyRcuSynrhonized = ifAlreadyRcuSynrhonized;
            return p;
        }
    }

    template<typename Op>
    bool rTableCoreTryInsertNoExpand(RTableCore& table, RNode* pEntry, size_t hashVal, Op matchOp)
    {
        pEntry->hash = hashVal;
        RTableCore::BucketsInfo* pBucketsInfo = table.pBucketsInfo.load(std::memory_order_relaxed);
        size_t bucketMask = pBucketsInfo->nrBucketsPowerOf2 - 1;
        auto bucketId = pEntry->hash & bucketMask;
        RTableCore::Bucket* pBucket = pBucketsInfo->pBuckets + bucketId;
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

    //can only be called if the user is sure that no dup exists
    void rTableCoreInsertNoExpand(RTableCore& table, RNode* pEntry);

    void rTableCoreExpandBuckets2x(RTableCore& table, RCUZone& zone);

    bool rTableCoreShrinkBuckets2x(RTableCore& table, RCUZone& zone);

    //-----------------------------------------------------------------------------------------------//


    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //--------------------------------Basic API------------------------------------------------------//
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //hash table must be initialized, nrBuckets is advised to be bigger than the predicted element count
    void rTableCoreInit(RTableCore& table);

    //Read operation
    //\parameter hashVal should be the hash value of the find target, only table entries with a hash value equals to `hashVal` is checked for
    // equality with matchOp.
    //op should have function signature of bool(RNode*) to identify if the RNode equals to what is to be found
    template<typename Op>
    RNode* rTableCoreFind(const RTableCore& table, size_t hashVal, Op matchOp)
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
    bool rTableCoreTryInsert(RTableCore& table, RCUZone& rcuZone, RNode* pEntry, size_t hashVal, Op matchOp)
    {
        bool inserted = rTableCoreTryInsertNoExpand(table, pEntry, hashVal, matchOp);
        if (!inserted)
            return false;
        auto currentSize = table.size.load(std::memory_order_relaxed);
        size_t nrBuckets = table.pBucketsInfo.load(std::memory_order_acquire)->nrBucketsPowerOf2;
        rTableCoreDetail::expandBucketsByFac2IfNecessary(currentSize, nrBuckets, table, rcuZone);
        return true;
    }

    //Write operation: all writers must be serialized
    //Op is of function signature of bool(RNode* p0), which returns if the entry is what you are looking
    //for.
    //rcuSynchronize is called internally and it is safe to delete resource of RNode.
    template<typename Op>
    RNode* rTableCoreTryDetachAndSynchronize(RTableCore& table, RCUZone& rcuZone, size_t hashVal, Op matchOp)
    {
        bool ifAlreadyRcuSynchronized = false;
        RNode* pEntry = rTableCoreTryDetachAutoShrink(table, hashVal, matchOp, rcuZone, &ifAlreadyRcuSynchronized);
        if (pEntry == nullptr)
            return pEntry;
        if (!ifAlreadyRcuSynchronized)
            rcuSynchronize(rcuZone);
        return pEntry;
    }
}

