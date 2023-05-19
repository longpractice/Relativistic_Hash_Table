#include "RCUHashTableApi.h"
#include "RCUHashTableTypes.h"
#include "TestHelper.h"
#include <vector>
#include <future>
#include <iostream>
#include <memory>
#include <unordered_set>
#include <shared_mutex>

namespace yrcu
{
    namespace
    {
        struct PerfComparisonWithStdUnorderedSet
        {
            static constexpr size_t c_nrElementsToLookUp = 2ull << 18;
            static constexpr size_t c_nrRounds = 10;;

            template<typename TMutex, template<typename> typename TReadLock, bool readOnly>
            void runStdUnorderedMapMutex(const std::string& title)
            {
                std::unordered_set<size_t> mySet;
                for (size_t v = 0; v < c_nrElementsToLookUp; ++v)
                    mySet.emplace(v);

                TMutex m;
                {
                    Timer timer{ title };
                    std::vector<std::future<void>> futures{std::thread::hardware_concurrency()};

                    auto f = [&m, &mySet, this]()
                    {
                        for (size_t round = 0; round < c_nrRounds; ++round)
                            for (auto v = 0; v < c_nrElementsToLookUp; ++v)
                            {
                                if constexpr (!readOnly)
                                {
                                    TReadLock<TMutex> l {m};
                                    if (mySet.find(v) == mySet.end())
                                        throw std::exception("Wrong");
                                }
                                else
                                {
                                    if (mySet.find(v) == mySet.end())
                                        throw std::exception("Wrong");
                                }
                            }
                    };

                    for (auto& myFuture : futures)
                        myFuture = std::async(std::launch::async, f);


                    //some seldom writing operations
                    if (!readOnly)
                        for (auto i = 0; i < c_nrElementsToLookUp; ++i)
                        {
                            if (i % 1024 != 0)
                                continue;
                            std::lock_guard<TMutex> l {m};
                            auto v = i + c_nrElementsToLookUp;
                            mySet.emplace(v);
                        }
                    for (auto& myFuture : futures)
                        myFuture.get();
                }
            }

            void runRcuHashMap()
            {
                RcuHashTable rTable;
                rcuHashTableInit(rTable);
                //user data to be tracked by the hash table
                struct MyElement
                {
                    size_t value;
                    RNode entry;
                };

                //here we use the slow heap allocated elements to mimic the std::unordered_set behavior to rule out the cache perf influence
                std::vector<std::unique_ptr<MyElement>> myData {c_nrElementsToLookUp};
                for (size_t item = 0; item < myData.size(); ++item)
                {
                    myData[item] = std::make_unique<MyElement>();
                    myData[item]->value = item;
                    auto hashVal = std::hash<size_t>{}(item);
                    bool inserted = rcuHashTableTryInsert(rTable, &myData[item]->entry, hashVal, [](RNode* p1, RNode* p2)
                        {
                            return YJ_CONTAINER_OF(p1, MyElement, entry)->value == YJ_CONTAINER_OF(p2, MyElement, entry)->value;
                        });
                }

                {
                    Timer timer{ "RCUHashTable: " };
                    std::vector<std::future<void>> futures{std::thread::hardware_concurrency()};
                    auto f = [&rTable, &myData, this]()
                    {
                        for (size_t round = 0; round < c_nrRounds; ++round)
                            for (auto i = 0; i < myData.size(); ++i)
                            {

                                RcuHashTableReadLockGuard l(rTable);
                                auto hashVal = std::hash<size_t>{}(myData[i]->value);
                                RNode* pEntry = rcuHashTableFind(rTable, hashVal, [i](const RNode* p)
                                    {
                                        return YJ_CONTAINER_OF(p, MyElement, entry)->value == i;
                                    });
                                if (!pEntry)
                                    throw std::exception("Wrong");
                            }
                    };

                    for (auto& myFuture : futures)
                        myFuture = std::async(std::launch::async, f);

                    //some seldom writing operations
                    std::vector<std::unique_ptr<MyElement>> myDataAdditional {c_nrElementsToLookUp};
                    for (auto i = 0; i < myData.size(); ++i)
                    {
                        if (i % 1024 != 0)
                            continue;
                        auto v = i + myData.size();
                        myDataAdditional[i] = std::make_unique<MyElement>();
                        myDataAdditional[i]->value = v;
                        auto hash = std::hash<int>{}(myDataAdditional[i]->value);

                        bool inserted = rcuHashTableTryInsert(rTable, &myDataAdditional[i]->entry, hash,
                            [](const RNode* p0, const RNode* p1)
                            {return YJ_CONTAINER_OF(p0, MyElement, entry)->value == YJ_CONTAINER_OF(p0, MyElement, entry)->value;  }
                        );
                    }

                    for (auto& myFuture : futures)
                        myFuture.get();
                }
            };

            void run()
            {
                runStdUnorderedMapMutex<std::shared_mutex, std::shared_lock, true>("UnorderedMap read only");
                runStdUnorderedMapMutex<std::shared_mutex, std::shared_lock, false>("UnorderedMap with std::shared_mutex");
                runStdUnorderedMapMutex<std::mutex, std::lock_guard, false>("UnorderedMap with std::mutex");
                runRcuHashMap();
            }
        };



        struct RCUTableTest0
        {
        public:
            void run()
            {
                RcuHashTable rTable;
                RcuHashTableConfig conf{};
                conf.expandFactor = 256.f;
                conf.shrinkFactor = 8.f;
                conf.nrBuckets = 2;
                conf.nrRcuBucketsForUnregisteredThreads = 64 * std::thread::hardware_concurrency();
                rcuHashTableInitDetailed(rTable, conf);

                struct Val
                {
                    size_t v;
                    RNode entry;
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
                        [](RNode* p1, RNode* p2)
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
                            bool found = rcuHashTableFind(rTable, hashVal, [&](RNode* p)
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
                        [](RNode* p1, RNode* p2)
                        {
                            return YJ_CONTAINER_OF(p1, Val, entry)->v == YJ_CONTAINER_OF(p2, Val, entry)->v;
                        }
                    );
                    if (!inserted)
                        throw std::exception("Broken");
                }

                //periodically remove and insert
                for (int j = 0; j < 3; ++j)
                {
                    std::cout << j << " round of erasing and inserting." << std::endl;
                    for (auto i = sizePersist; i < sizeTotal; ++i)
                    {
                        auto hash = std::hash<size_t>{}(i);

                        RNode* pEntry = rcuHashTableTryDetachAndSynchronize(rTable, hash, [i](RNode* p)
                            {return YJ_CONTAINER_OF(p, Val, entry)->v == i; }
                        );

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
                            [](RNode* p1, RNode* p2)
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
                    bool found = rcuHashTableFind(rTable, hashVal, [&](RNode* p)
                        {
                            return YJ_CONTAINER_OF(p, Val, entry)->v == i;
                        }
                    );
                    if (!found)
                        throw std::exception("Broken");
                }

            }
        };

        void RCUTableTestSingleThreadTest()
        {
            RcuHashTable tbl;
            rcuHashTableInit(tbl);

            struct Element
            {
                size_t v;
                RNode entry;
                static Element* fromNode(RNode* pEntry)
                {
                    return YJ_CONTAINER_OF(pEntry, Element, entry);
                }
            };
            std::vector<Element> values{1000};
            for (size_t i = 0; i < values.size(); ++i)
                values[i].v = i;

            //when there is only one thread, read lock is not needed
            for (size_t i = 0; i < values.size(); ++i)
            {
                bool inserted = rcuHashTableTryInsert(tbl, &values[i].entry, std::hash<size_t>{}(i),
                    [](RNode* p0, RNode* p1) {
                        return Element::fromNode(p0)->v == Element::fromNode(p1)->v;
                    }
                );
                if (!inserted)
                    throw std::exception("Broken");
                bool insertedSecondTime = rcuHashTableTryInsert(tbl, &values[i].entry, std::hash<size_t>{}(i),
                    [](RNode* p0, RNode* p1) {
                        return Element::fromNode(p0)->v == Element::fromNode(p1)->v;
                    }
                );
                if (insertedSecondTime)
                    throw std::exception("Broken");
            }

            bool foundNotInElement = rcuHashTableFind(tbl, std::hash<size_t>{}(values.size()),
                [val = values.size()](RNode* p) { return Element::fromNode(p)->v == val; });
            if (foundNotInElement)
                throw std::exception("Broken");

            for (size_t i = 0; i < values.size(); ++i)
            {
                RNode* detached = rcuHashTableTryDetach(tbl, std::hash<size_t>{}(i),
                    [i](RNode* p) { return Element::fromNode(p)->v == i; });
                if (!detached)
                    throw std::exception("Broken");
            }

            for (size_t i = 0; i < values.size(); ++i)
            {
                bool foundNotInElement = rcuHashTableFind(tbl, std::hash<size_t>{}(values.size()),
                    [val = values.size()](RNode* p) { return Element::fromNode(p)->v == val; });
                if (foundNotInElement)
                    throw std::exception("Broken");
            }
        }
    }


    void rcuHashTableTests()
    {
        RCUTableTestSingleThreadTest();

        PerfComparisonWithStdUnorderedSet comp;
        comp.run();

        RCUTableTest0 test0;
        test0.run();
    }
}