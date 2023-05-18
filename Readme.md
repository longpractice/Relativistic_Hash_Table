

Implements a relativistic hash table (in the repo, as a `RcuHashTable` ) using epoch based RCU in C++.

Relativistic hash table was published in this paper:

```
Triplett J, McKenney P E, Walpole J. Scalable concurrent hash tables via relativistic programming[J]. ACM SIGOPS Operating Systems Review, 2010, 44(3): 102-109.
```
This article also gives a nice description:

```
https://lwn.net/Articles/612021/
```

**Header arrangement**

Headers are in "LibSource\include" folder. 

The data structure definitions and API definitions are split into different header files, in order to make user headers that mostly only needs to include the `RCUZone` or `RcuHashTable` data definitions small to speed up compilation. For example, `RCUHashTableTypes.h` includes all the data definition for `RcuHashTable` while `RCUHashTableApi.h` declares all the API functions.

**Functionalities**


`RCUZone` data structure defines a single RCU synchronization unit. This `RCUZone` could be used by the user to synchronize any RCU data structures. The implementation is based on epoch-based-rcu. Reader threads (usually from a thread pool) could optionally be registered before any `RCUZone` initialization. Registered threads have no contentions with each other. Unregistered reader threads fallbacks to thread-id hashing based RCU reader count, and through configuration of using more memory, the unregistered reader threads contention is also very minimal. 

The `RCUHashTable` implements the relativistic hash table. The `RCUHashTable` hash buckets links 
```
struct RcuHashTableEntry
{
    size_t hash;
    AtomicSingleHead head;
};
```
The user could either embed these entries into their data structure (C manner) or define extra nodes to point to their own data structures(C++ manner):

```
struct RcuHashTableEntryAdapt
{
    MyData* pMyData;
    RcuHashTableEntry entry;

    static MyData* rcuHashTableEntryToPtrMyData(RcuHashTableEntry* pEntry)   
    {
        return CONTAINER_OF(pEntry, RcuHashTableEntryAdapt, entry)->pMyData;
    } 
};
```





