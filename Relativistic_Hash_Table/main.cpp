#include <iostream>
#include <future>
#include <cstdlib>
#include <random>
#include <shared_mutex>
#include <memory>
#include "RCUTypes.h"
#include "RCUApi.h"
#include "RCUHashTableApi.h"
#include "RCUHashTableTypes.h"

#include <array>
namespace yj
{
    void rcuTableTest()
    {
        yj::RcuHashTable rTable;
        yj::rcuHashTableInit(rTable, 4);

        struct Val
        {
            size_t v;
            yj::RcuHashTableEntry entry;
            bool valid = false;
        };

        size_t sizeTotal = 888888;
        size_t sizePersist = 888;

        std::vector<Val> arr = std::vector<Val>{ sizeTotal };

        for (size_t i = 0; i < sizePersist; ++i)
        {
            arr[i].v = i;
            arr[i].valid = true;
            auto hash = std::hash<size_t>{}(i);

            bool inserted = yj::rcuHashTableTryInsert(rTable, &arr[i].entry, hash,
                [](yj::RcuHashTableEntry* p1, yj::RcuHashTableEntry* p2)
                {
                    return YJ_CONTAINER_OF(p1, Val, entry)->v == YJ_CONTAINER_OF(p2, Val, entry)->v;
                }
            );
            if (!inserted)
                throw std::exception("Broken");

            //yj::rcuHashTableInsert(rTable, &arr[i].entry);
            //yj::printRCUTable(rTable);
        }

        std::atomic<bool> finished = false;

        auto f = [&]()
        {
            while (!finished.load(std::memory_order_relaxed))
            {
                for (size_t i = 0; i < sizeTotal; ++i)
                {
                    size_t hashVal = std::hash<size_t>{}(i);
                    auto epoch = yj::rcuHashTableReadLock(rTable);
                    bool found = yj::rcuHashTableFind(rTable, hashVal, [&](yj::RcuHashTableEntry* p)
                        {
                            return YJ_CONTAINER_OF(p, Val, entry)->v == i;
                        }
                    );
                    if (found && !arr[i].valid)
                        throw std::exception("ElementNotValid in reader critical session.");
                    yj::rcuHashTableReadUnlock(rTable, epoch);
                    if (i < sizePersist && !found)
                        throw std::exception("Broken");
                }
            }
        };

        std::future<void> futures[7];
        for (auto& future : futures)
            future = std::async(std::launch::async, f);


        yj::rcuHashTableSynchronize(rTable);
        for (auto i = sizePersist; i < sizeTotal; ++i)
        {
            arr[i].v = i;
            arr[i].valid = true;
            auto hash = std::hash<size_t>{}(i);
            bool inserted = yj::rcuHashTableTryInsert(rTable, &arr[i].entry, hash,
                [](yj::RcuHashTableEntry* p1, yj::RcuHashTableEntry* p2)
                {
                    return YJ_CONTAINER_OF(p1, Val, entry)->v == YJ_CONTAINER_OF(p2, Val, entry)->v;
                }
            );
            if (!inserted)
                throw std::exception("Broken");
        }

        //periodically remove and insert
        for (int j = 0; j < 100000; ++j)
        {
            std::cout << j << " round of erasing and inserting." << std::endl;
            for (auto i = sizePersist; i < sizeTotal; ++i)
            {
                auto hash = std::hash<size_t>{}(i);

                yj::RcuHashTableEntry* pEntry = yj::rcuHashTableTryDetachAndSynchronize(rTable, hash, [i](yj::RcuHashTableEntry* p)
                    {return YJ_CONTAINER_OF(p, Val, entry)->v == i; }
                );

                yj::rcuHashTableSynchronize(rTable);
                YJ_CONTAINER_OF(pEntry, Val, entry)->valid = false;
                if (!pEntry)
                    throw std::exception("Broken");
            }

            for (auto i = sizePersist; i < sizeTotal; ++i)
            {
                arr[i].v = i;
                arr[i].valid = true;
                auto hash = std::hash<size_t>{}(i);

                bool inserted = yj::rcuHashTableTryInsert(rTable, &arr[i].entry, hash,
                    [](yj::RcuHashTableEntry* p1, yj::RcuHashTableEntry* p2)
                    {
                        return YJ_CONTAINER_OF(p1, Val, entry)->v == YJ_CONTAINER_OF(p2, Val, entry)->v;
                    }
                );
                if (!inserted)
                    throw std::exception("Broken");
            }
        }

        finished.store(true, std::memory_order_relaxed);

        for (auto& future : futures)
            future.get();


        for (auto i = 0; i < sizePersist; ++i)
        {
            size_t hashVal = std::hash<size_t>{}(i);
            bool found = yj::rcuHashTableFind(rTable, hashVal, [&](yj::RcuHashTableEntry* p)
                {
                    return YJ_CONTAINER_OF(p, Val, entry)->v == i;
                }
            );
            if (!found)
                throw std::exception("Broken");
        }

    }
}


namespace yj
{
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
    yj::rcuTableTest();

    //yj::printRCUTable(rTable);
}