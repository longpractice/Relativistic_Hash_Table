#include <iostream>
#include <future>
#include <cstdlib>
#include <random>
#include <shared_mutex>
#include <memory>
#include "../LibSource/include/RCUTypes.h"
#include "../LibSource/include/RCUApi.h"

namespace yj
{
    namespace
    {
        struct RCUTest0
        {
        private:
            std::random_device rd;  // a seed source for the random number engine
            std::mt19937 gen{rd()}; // mersenne_twister_engine seeded with rd()
            std::uniform_int_distribution<int64_t> distrib{1, 100000000ll};
            std::shared_mutex sm;
            std::mutex m;
            std::atomic<int64_t*> pCurrent;
            std::atomic<std::shared_ptr<int64_t>> spCurrent;

            RCUZone zone;

            static constexpr int c_nrLoops = (2ll << 21);
            static constexpr int c_nrThreads = 8;
            static constexpr int c_writeInterval = 10000;

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

        public:
            void run()
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
        };
    }

    void rcuTests()
    {
        RCUTest0 test0;
        test0.run();
    }
}