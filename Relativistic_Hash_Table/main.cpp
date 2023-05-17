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

#include <charconv>
#include <array>
namespace yj
{
    std::random_device rd;  // a seed source for the random number engine
    std::mt19937 gen(rd()); // mersenne_twister_engine seeded with rd()
    std::uniform_int_distribution<int64_t> distrib(1, 100000000ll);
    std::shared_mutex sm;
    std::mutex m;
    std::atomic<int64_t*> pCurrent;
    std::atomic<std::shared_ptr<int64_t>> spCurrent;

    RCUZone zone;

    constexpr int c_nrLoops = (2ll << 21);
    constexpr int c_nrThreads = 8;
    constexpr int c_writeInterval = 10000;

    void func(int64_t v)
    {
        if (v < 0)
            throw std::exception("broken");
        int64_t res = 1;
        for (int i = 0; i < 25; ++i)
            res *= v + i;
        if (res % 10000000000 == 0)
            std::cout << v << "\n";
    }


    void fReadOnly()
    {
        std::future<void> futureModify = std::async(std::launch::async, [&]()
            {
                for (int64_t k = 0; k < c_nrLoops; ++k)
                {
                    if (k % c_writeInterval == 0)
                    {
                        auto* p = new int64_t(distrib(gen));
                        if (*p % 10000000000 == 0)
                            std::cout << "oo\n";
                    }
                }

            });

        std::future<void> futures[c_nrThreads];
        for (int i = 0; i < c_nrThreads; ++i)
        {
            futures[i] = std::async(std::launch::async,
                [&]()
                {
                    for (int64_t k = 0; k < c_nrLoops; ++k)
                        func(*(pCurrent.load(std::memory_order_relaxed)));
                }
            );
        }
        for (auto& f : futures)
            f.get();
        futureModify.get();
    }

    void fSharedMutex()
    {
        std::future<void> futureModify = std::async(std::launch::async, [&]()
            {
                for (int64_t k = 0; k < c_nrLoops; ++k)
                {
                    if (k % c_writeInterval == 0)
                    {
                        std::lock_guard<std::shared_mutex> l {sm};
                        auto pOld = pCurrent.load(std::memory_order_relaxed);
                        pCurrent.store(new int64_t(distrib(gen)), std::memory_order_relaxed);
                        delete pOld;
                    }
                }

            });
        std::future<void> futures[c_nrThreads];
        for (int i = 0; i < c_nrThreads; ++i)
        {
            futures[i] = std::async(std::launch::async,
                [&]()
                {
                    for (int64_t k = 0; k < c_nrLoops; ++k)
                    {
                        std::shared_lock<std::shared_mutex> l {sm};
                        func(*(pCurrent.load(std::memory_order_relaxed)));
                    }
                }
            );
        }
        for (auto& f : futures)
            f.get();
        futureModify.get();
    }

    void fMutex()
    {
        std::future<void> futureModify = std::async(std::launch::async, [&]()
            {
                for (int64_t k = 0; k < c_nrLoops; ++k)
                {
                    if (k % c_writeInterval == 0)
                    {
                        std::lock_guard<std::mutex> l {m};
                        auto pOld = pCurrent.load(std::memory_order_relaxed);
                        pCurrent = new int64_t(distrib(gen));
                        delete pOld;
                    }
                }

            });
        std::future<void> futures[c_nrThreads];
        for (int i = 0; i < c_nrThreads; ++i)
        {
            futures[i] = std::async(std::launch::async,
                [&]()
                {
                    for (int64_t k = 0; k < c_nrLoops; ++k)
                    {
                        std::lock_guard<std::mutex> l {m};
                        func(*(pCurrent.load(std::memory_order_relaxed)));
                    }
                }
            );
        }
        for (auto& f : futures)
            f.get();
        futureModify.get();
    }

    void fRCU()
    {
        std::future<void> futureModify = std::async(std::launch::async, [&]()
            {
                for (int64_t k = 0; k < c_nrLoops; ++k)
                {
                    if (k % c_writeInterval == 0)
                    {
                        auto pOld = pCurrent.load(std::memory_order_acquire);
                        pCurrent.store(new int64_t(distrib(gen)), std::memory_order_release);
                        rcuSynchronize(zone);
                        delete pOld;
                    }
                }

            });
        std::future<void> futures[c_nrThreads];
        for (int i = 0; i < c_nrThreads; ++i)
        {
            futures[i] = std::async(std::launch::async,
                [&]()
                {
                    for (int64_t k = 0; k < c_nrLoops; ++k)
                    {
                        auto epoch = rcuReadLock(zone);
                        func(*(pCurrent.load(std::memory_order_acquire)));
                        rcuReadUnlock(zone, epoch);
                    }
                }
            );
        }
        for (auto& f : futures)
            f.get();
        futureModify.get();
    }

    void fRCURegister()
    {
        std::future<void> futureModify = std::async(std::launch::async, [&]()
            {
                for (int64_t k = 0; k < c_nrLoops; ++k)
                {
                    if (k % c_writeInterval == 0)
                    {
                        auto pOld = pCurrent.load(std::memory_order_acquire);
                        pCurrent.store(new int64_t(distrib(gen)), std::memory_order_release);
                        rcuSynchronize(zone);
                        delete pOld;
                    }
                }

            });
        std::atomic<int> latch = 0;
        std::future<void> futures[c_nrThreads];
        for (int i = 0; i < c_nrThreads; ++i)
        {
            futures[i] = std::async(std::launch::async,
                [&]()
                {
                    rcuRegisterReaderThread();
                    latch++;
                    while (latch < 8)
                        continue;
                    for (int64_t k = 0; k < c_nrLoops; ++k)
                    {
                        auto epoch = rcuReadLock(zone);
                        func(*(pCurrent.load(std::memory_order_acquire)));
                        rcuReadUnlock(zone, epoch);
                    }
                }
            );
        }
        for (auto& f : futures)
            f.get();
        futureModify.get();
    }

    void fAtomicSharedPtr()
    {
        std::future<void> futureModify = std::async(std::launch::async, [&]()
            {
                for (int64_t k = 0; k < c_nrLoops; ++k)
                    if (k % c_writeInterval == 0)
                        spCurrent = std::make_shared<int64_t>(distrib(gen));

            });
        std::future<void> futures[c_nrThreads];
        for (int i = 0; i < c_nrThreads; ++i)
        {
            futures[i] = std::async(std::launch::async,
                [&]()
                {
                    for (int64_t k = 0; k < c_nrLoops; ++k)
                        func(*(spCurrent.load(std::memory_order_relaxed)));
                }
            );
        }
        for (auto& f : futures)
            f.get();
        futureModify.get();
    }

    void speedTest()
    {
        std::cout << "hardware concurrency: " << std::thread::hardware_concurrency() << std::endl;
        std::cout << "Nr threads: " << c_nrThreads << std::endl;
        pCurrent = new int64_t(distrib(gen));
        rcuInitZone(zone);

        if (true)
        {
            auto timeStart = std::chrono::system_clock::now();
            fRCURegister();
            auto timeFinish = std::chrono::system_clock::now();
            auto diff = timeFinish - timeStart;
            std::cout << "RCU_REGISTER: " << std::chrono::duration_cast<std::chrono::milliseconds>(diff).count() << "_ms\n";
        }

        if (true)
        {
            auto timeStart = std::chrono::system_clock::now();
            fRCU();
            auto timeFinish = std::chrono::system_clock::now();
            auto diff = timeFinish - timeStart;
            std::cout << "FRCU__RCU___: " << std::chrono::duration_cast<std::chrono::milliseconds>(diff).count() << "_ms\n";
        }

        if (true)
        {
            auto timeStart = std::chrono::system_clock::now();
            fReadOnly();
            auto timeFinish = std::chrono::system_clock::now();
            auto diff = timeFinish - timeStart;
            std::cout << "FReadOnly___: " << std::chrono::duration_cast<std::chrono::milliseconds>(diff).count() << "_ms\n";
        }

        if (true)
        {
            auto timeStart = std::chrono::system_clock::now();
            fAtomicSharedPtr();
            auto timeFinish = std::chrono::system_clock::now();
            auto diff = timeFinish - timeStart;
            std::cout << "FAtomicSRPTR: " << std::chrono::duration_cast<std::chrono::milliseconds>(diff).count() << "_ms\n";
        }


        if (true)
        {
            auto timeStart = std::chrono::system_clock::now();
            fSharedMutex();
            auto timeFinish = std::chrono::system_clock::now();
            auto diff = timeFinish - timeStart;
            std::cout << "FSharedMutex: " << std::chrono::duration_cast<std::chrono::milliseconds>(diff).count() << "_ms\n";
        }

        if (true)
        {
            auto timeStart = std::chrono::system_clock::now();
            fMutex();
            auto timeFinish = std::chrono::system_clock::now();
            auto diff = timeFinish - timeStart;
            std::cout << "FMutex______: " << std::chrono::duration_cast<std::chrono::milliseconds>(diff).count() << "_ms\n";
        }

        rcuReleaseZone(zone);
    }

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