#pragma once
#include <cstdint>
namespace yj
{
    struct RCUZone;
    bool rcuRegisterReaderThread();
    void rcuInitZoneWithBucketCounts(RCUZone& zone, uint32_t nrHashThreadBuckets);
    void rcuInitZone(RCUZone& zone);
    void rcuReleaseZone(RCUZone& zone);
    int64_t rcuReadLock(RCUZone& zone);
    void rcuReadUnlock(RCUZone& zone, int64_t epoch);
    void rcuSynchronize(RCUZone& zone);
}
