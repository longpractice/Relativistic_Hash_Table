#pragma once
#include <cstdint>
namespace yj
{
    //An RCU zone is a rcu synchronization unit.
    //For a particular rcu zone, the reader calls rcuReadLock and rcuReadUnlock to 
    //make a reader side critical session.
    //The writer calls rcuSynchronize to ensure that any ongoing readers' critical session
    //started from the call expires.
    //The writer can then safely garbage collect resources.
    //
    //The scope of the protection of RCUZone can be freely defined by the user.
    //
    //Nested locking for RCUZones, it is not allowed to nest read-lock or call rcuSynchronize
    //on a single RCUZone. Different RCUZones can be nest read-locked and rcuSynchronized.
    //But it might trigger a deadlock. 
    // The same rule/validation to avoid deadlock as mutex applies: 
    // rcu-readLock/unlock() is equivalent to mutex-readLock/readUnlock
    // rcuSynchronize() is equivalent to mutex-write-lock and then immediately mutex-write-unlock.
    struct RCUZone;

    //It is recommended (but not required) that reader threads register itself 
    //globally before any RCUZone is initialized.
    //Registered threads are guaranteed to have no contention with other readers. 
    //(They have a unique bucket for its read count).
    //Other threads that are not registered will have to share nrHashThreadBuckets
    // buckets, dependent on the hashing of the thread id.
    bool rcuRegisterReaderThread();


    //Before any operation on the rcuZone, the
    //Init rcu zone with a specified nrHashThreadBuckets to be shared
    //for all unregistered threads who will read lock the rcu zone.
    void rcuInitZoneWithBucketCounts(RCUZone& zone, uint32_t nrHashThreadBuckets);

    //Similar to rcuInitZoneWithBucketCounts(RCUZone& zone, uint32_t nrHashThreadBuckets);
    //nrHashThreadsBuckets is configured as a multiple factor of hardware_concurrency.
    //factor is c_nrBucketsPerHardwareThread
    constexpr size_t c_nrRCUBucketsPerHardwareThread = 64;
    void rcuInitZone(RCUZone& zone);
    
    //After the usage of rcuZone, it must be released.
    void rcuReleaseZone(RCUZone& zone);

    //reader critical session start
    int64_t rcuReadLock(RCUZone& zone);

    //reader critical session end
    void rcuReadUnlock(RCUZone& zone, int64_t epoch);

    //writer to wait for all on going reader critical sessions 
    //before the call to expire
    void rcuSynchronize(RCUZone& zone);
}
