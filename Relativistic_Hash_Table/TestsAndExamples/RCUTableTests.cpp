#include "RCUHashTableApi.h"
#include "RCUHashTableTypes.h"
#include <vector>
#include <future>
#include <iostream>

namespace yrcu
{
    namespace
    {
        struct RCUTableTest0
        {
        public:
            void run()
            {
                RcuHashTable rTable;
                rcuHashTableInit(rTable, 4);

                struct Val
                {
                    size_t v;
                    RcuHashTableEntry entry;
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

                    bool inserted = rcuHashTableTryInsert(rTable, &arr[i].entry, hash,
                        [](RcuHashTableEntry* p1, RcuHashTableEntry* p2)
                        {
                            return YJ_CONTAINER_OF(p1, Val, entry)->v == YJ_CONTAINER_OF(p2, Val, entry)->v;
                        }
                    );
                    if (!inserted)
                        throw std::exception("Broken");

                    //rcuHashTableInsert(rTable, &arr[i].entry);
                    //printRCUTable(rTable);
                }

                std::atomic<bool> finished = false;

                auto f = [&]()
                {
                    while (!finished.load(std::memory_order_relaxed))
                    {
                        for (size_t i = 0; i < sizeTotal; ++i)
                        {
                            size_t hashVal = std::hash<size_t>{}(i);
                            auto epoch = rcuHashTableReadLock(rTable);
                            bool found = rcuHashTableFind(rTable, hashVal, [&](RcuHashTableEntry* p)
                                {
                                    return YJ_CONTAINER_OF(p, Val, entry)->v == i;
                                }
                            );
                            if (found && !arr[i].valid)
                                throw std::exception("ElementNotValid in reader critical session.");
                            rcuHashTableReadUnlock(rTable, epoch);
                            if (i < sizePersist && !found)
                                throw std::exception("Broken");
                        }
                    }
                };

                std::future<void> futures[7];
                for (auto& future : futures)
                    future = std::async(std::launch::async, f);


                rcuHashTableSynchronize(rTable);
                for (auto i = sizePersist; i < sizeTotal; ++i)
                {
                    arr[i].v = i;
                    arr[i].valid = true;
                    auto hash = std::hash<size_t>{}(i);
                    bool inserted = rcuHashTableTryInsert(rTable, &arr[i].entry, hash,
                        [](RcuHashTableEntry* p1, RcuHashTableEntry* p2)
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

                        RcuHashTableEntry* pEntry = rcuHashTableTryDetachAndSynchronize(rTable, hash, [i](RcuHashTableEntry* p)
                            {return YJ_CONTAINER_OF(p, Val, entry)->v == i; }
                        );

                        rcuHashTableSynchronize(rTable);
                        YJ_CONTAINER_OF(pEntry, Val, entry)->valid = false;
                        if (!pEntry)
                            throw std::exception("Broken");
                    }

                    for (auto i = sizePersist; i < sizeTotal; ++i)
                    {
                        arr[i].v = i;
                        arr[i].valid = true;
                        auto hash = std::hash<size_t>{}(i);

                        bool inserted = rcuHashTableTryInsert(rTable, &arr[i].entry, hash,
                            [](RcuHashTableEntry* p1, RcuHashTableEntry* p2)
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
                    bool found = rcuHashTableFind(rTable, hashVal, [&](RcuHashTableEntry* p)
                        {
                            return YJ_CONTAINER_OF(p, Val, entry)->v == i;
                        }
                    );
                    if (!found)
                        throw std::exception("Broken");
                }

            }
        };
    }

    void rcuHashTableTests()
    {
        RCUTableTest0 test0;
        test0.run();
    }
}