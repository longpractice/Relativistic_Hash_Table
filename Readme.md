

Implements a relativistic hash table ( `RcuHashTable` ) using epoch-based-RCU in C++.

Paper defining relativistic hash table:

```
Triplett J, McKenney P E, Walpole J. Scalable concurrent hash tables via relativistic programming[J]. ACM SIGOPS Operating Systems Review, 2010, 44(3): 102-109.
```

Another nice description:

```
https://lwn.net/Articles/612021/
```



**Functionalities**

An `RCUZone` defines a single RCU synchronization unit. It could be used by the user to synchronize any RCU data structures. Reader threads (usually from a thread pool) could optionally be registered before any `RCUZone` initialization. Registered threads have no contentions with each other. Unregistered reader threads fallbacks to thread-id hashing based RCU reader count with minimal contention. 

The `RCUHashTable` implements the relativistic hash table which links
```
struct RcuHashTableEntry
{
    size_t hash;
    AtomicSingleHead head;
};
```
The user could embed these entries into their data structure (C manner). C++ users could define "nodes" to point to their own data structures(C++ manner):

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

**Header arrangement**

Headers are in "LibSource\include". 

The data structure definitions and API definitions are split into different header files. For example, `RCUHashTableTypes.h` includes all the data definition for `RcuHashTable` while `RCUHashTableApi.h` declares all the API functions.




