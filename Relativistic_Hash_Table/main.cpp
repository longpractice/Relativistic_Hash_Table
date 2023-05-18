#include <iostream>
#include <future>
#include <cstdlib>
#include <random>
#include <shared_mutex>
#include <memory>
#include "TestsAndExamples/RCUTests.h"
#include "TestsAndExamples/RCUTableTests.h"

#include "RCUApi.h"
#include "RCUHashTableApi.h"
#include "RCUHashTableTypes.h"
#include <array>
#include <cassert>



namespace yrcu
{
    void rcuHashTableQuickExample()
    {
        RcuHashTable rTable;
        rcuHashTableInit(rTable);

        //user data to be tracked by the hash table
        struct MyElement
        {
            int value;
            //to be linked by the hash table
            RcuHashTableEntry entry;
            bool valid;
        };

        //insert all array elements into the hash table
        std::vector<MyElement> myData {100000};
        for (auto item = 0; item < myData.size(); ++item)
        {
            myData[item].value = static_cast<int>(item);
            myData[item].valid = true;
            auto hashVal = std::hash<int>{}(item);
            bool inserted = rcuHashTableTryInsert(rTable, &myData[item].entry, hashVal, [](RcuHashTableEntry* p1, RcuHashTableEntry* p2)
                {
                    return YJ_CONTAINER_OF(p1, MyElement, entry)->value == YJ_CONTAINER_OF(p2, MyElement, entry)->value;
                });
            assert(inserted);
        }

        //reader thread looks up
        auto readerFunc = [&rTable, &myData]()
        {
            for (auto i = 0; i < myData.size(); ++i)
            {
                RcuHashTableReadLockGuard l(rTable);
                auto hashVal = std::hash<int>{}(myData[i].value);
                RcuHashTableEntry* pEntry = rcuHashTableFind(rTable, hashVal, [i](const RcuHashTableEntry* p)
                    {
                        return YJ_CONTAINER_OF(p, MyElement, entry)->value == i;
                    });
                //writer might erase elements that is not multiples of 8
                //but other elements should remain found
                if (i % 8 == 0)
                {
                    assert(pEntry);
                    assert(pEntry == &myData[i].entry);
                    assert(YJ_CONTAINER_OF(pEntry, MyElement, entry)->valid);
                }
            }
        };

        //push reader threads
        const auto nrThreads = std::thread::hardware_concurrency();
        std::vector<std::future<void>> readerFutures{nrThreads};
        for (auto& f : readerFutures)
            f = std::async(std::launch::async, readerFunc);

        //a single writer removes and add back elements in several rounds
        for (auto i = 0; i < myData.size(); ++i)
        {
            if (i % 8 == 0)
                continue;
            auto hash = std::hash<int>{}(myData[i].value);

            RcuHashTableEntry* pEntry = rcuHashTableTryDetachAndSynchronize(rTable, hash, [val = myData[i].value](const RcuHashTableEntry* p)
                {return YJ_CONTAINER_OF(p, MyElement, entry)->value == val; }
            );
            assert(pEntry);
            YJ_CONTAINER_OF(pEntry, MyElement, entry)->valid = false;
        }

        for (auto& f : readerFutures)
            f.get();
    }

    void printRCUTable(RcuHashTable& rTable)
    {
        std::cout << "-------------------\n";
        auto pBucket = rTable.pBucketsInfo.load();
        for (auto iBucket = 0; iBucket < pBucket->nrBucketsPowerOf2; ++iBucket)
        {
            std::cout << "\nBucket " << iBucket << std::endl;
            auto pSrc = pBucket->pBuckets + iBucket;

            for (auto p = pSrc->list.next.load(std::memory_order_relaxed); p != nullptr; p = p->next.load(std::memory_order_relaxed))
            {
                size_t hashV = YJ_CONTAINER_OF(p, RcuHashTableEntry, head)->hash;
                std::cout << " hash: " << hashV;
            }
        }
        std::cout << "\n-------------------\n";
    }
}

int main()
{
    yrcu::rcuHashTableQuickExample();

    yrcu::rcuTests();
    yrcu::rcuHashTableTests();
}