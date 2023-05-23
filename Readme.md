
Welcome to use, comment, report bugs and contribute to this new library, I will usually reply fast. The library here is well tested.


Implements a relativistic hash table ( `RcuHashTable` ) using epoch-based-RCU in C++.

Paper defining relativistic hash table:

```
Triplett J, McKenney P E, Walpole J. Scalable concurrent hash tables via relativistic programming. 
ACM SIGOPS Operating Systems Review, 2010, 44(3): 102-109.
```

Another nice description:

```
https://lwn.net/Articles/612021/
```

---


**A quick example**

See `void rcuHashTableQuickExample()` in main.cpp.


---


**Functionalities**

An `RCUZone` defines a single RCU synchronization unit. It could be used by the user to synchronize any RCU data structures. Reader threads (usually from a thread pool) could optionally be registered before any `RCUZone` initialization. Registered threads have no contentions with each other. Unregistered reader threads fallbacks to thread-id hashing based RCU reader count with minimal contention. 

The `RTable` implements the relativistic hash table which links
```
struct RTableEntry
{
    size_t hash;
    AtomicSingleHead head;
};
```
The user could embed these entries into their data structure. A non-intrusive manner could define "nodes" to point to their own data structures by defining:

```
struct RTableEntryAdapt
{
    MyData* pMyData;
    RTableEntry entry;

    static MyData* rTableEntryToPtrMyData(RTableEntry* pEntry)   
    {
        return CONTAINER_OF(pEntry, RTableEntryAdapt, entry)->pMyData;
    } 
};
```

A `RTable` has a `RCUZone` as its member. However, sometimes, it might be beneficial for the user to use one `RCUZone` to protect multiple data structures, and `RTableCore` does not include a `RCUZone` as member and 
the user can use an external `RCUZone` which can be shared by multiple pieces of data.

---

Benchmark: RCU hash table usually performs ~10x than std::unordered_map equiped with std::mutex or std::shared_mutex.

`PerfComparisonWithStdUnorderedSet` in RCUTableTests.cpp tests in `Intel i7-7700K CPU 4.20GHz 8 Logical Processors` shows this result:

```
UnorderedMap read-only(no mutex, no rcu): 302_ms
RCUHashTable: : 435_ms
UnorderedMap with std::shared_mutex: 4003_ms
UnorderedMap with std::mutex: 6148_ms
```

Another benchmark `RCUBenchmark` in `RCUTests.cpp` related to a general usage of RCUZone to protect a piece of "read mostly" data, where also includes a comparison with the atomic shared pointer which is the slowest: 

```
ReadOnly (no lock or RCU): 127_ms
RCU(with registered threads): 252_ms
RCU(without registered threads): 278_ms
Atomic shared pointer: 5018_ms
SharedMutex: 2062_ms
Mutex: 1654_ms
```

---

**Header arrangement**

Headers are in "LibSource\include". 

The data structure definitions and API definitions are split into different header files. For example, `RCUHashTableTypes.h` includes all the data definition for `RcuHashTable` while `RCUHashTableApi.h` declares all the API functions.




