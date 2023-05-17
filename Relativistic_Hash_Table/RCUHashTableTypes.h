#pragma once
#include "RCUTypes.h"
namespace yj
{
#define YJ_OFFSET_OF(Type, Field) __builtin_offsetof(Type, Field)
#define YJ_CONTAINER_OF(ptr, type, member) ((type*)((char*)ptr-YJ_OFFSET_OF(type, member)))
    struct AtomicSingleHead
    {
        std::atomic<AtomicSingleHead*> next;
    };

    struct RcuHashTableEntry
    {
        size_t hash;
        AtomicSingleHead head;
    };

    struct RcuHashTable
    {
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

        //expand when element/buckets-count grows over this factor
        float expandFactor = 256.f;
        //shrink when element/buckets-count is less than this factor
        float shrinkFactor = 8.f;

        RCUZone rcuZone;
        std::atomic<BucketsInfo*> pBucketsInfo = nullptr;

        ~RcuHashTable();
    };
}