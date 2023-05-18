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
    yj::rcuHashTableTests();

    //yj::printRCUTable(rTable);
}