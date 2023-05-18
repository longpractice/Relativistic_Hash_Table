This repository implements a relativistic hash table (in the repo, as a `RcuHashTable` ) using epoch based RCU in C++.

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
The headers arrangement splits the data structure definitions and API definitions into different header files, in order to make user headers that mostly only needs to include the `RCUZone` or `RcuHashTable` data definitions small to speed up compilation. For example, `RCUHashTableTypes.h` includes all the data definition for `RcuHashTable` while `RCUHashTableApi.h` declares all the API function.



