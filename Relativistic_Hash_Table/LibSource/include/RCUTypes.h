#pragma once
#include <atomic>

namespace yrcu
{
    constexpr int c_maxEpoches = 2;
    constexpr int c_epochMask = c_maxEpoches - 1;

    struct alignas(64) RCUReaderRefCountBucket
    {
        std::atomic<int> count;
    };

    struct EpochBuckets
    {
        RCUReaderRefCountBucket* pBuckets = nullptr;
    };

    //There should be only one RCUGlobal globally 
    //Upto hardware_concurrency threads could register themselves once on the RCUGlobal
    struct RCUReaderThreadRegistry
    {
        static std::atomic<int> nextBucketId;
        static const int nrNonOverlappingBucketCount;
        static thread_local int tlsReaderBucketId;
    };
    
    struct RCUZone
    {
        int nrHashThreadBuckets = 0;
        EpochBuckets epochsRing[c_maxEpoches];
        std::atomic<int64_t> epochLatest = 0;
        int64_t epochOldest = 0;

        ~RCUZone();
    };
}