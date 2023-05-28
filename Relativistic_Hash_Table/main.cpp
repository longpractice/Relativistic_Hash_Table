#include <array>
#include <cassert>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <random>
#include <shared_mutex>

#include "RCUApi.h"
#include "RCUHashTableApi.h"
#include "RCUHashTableTypes.h"
#include "TestsAndExamples/RCUTableTests.h"
#include "TestsAndExamples/RCUTests.h"

namespace yrcu
{
void rcuHashTableQuickExample()
{
	RTable rTable;
	rTableInit(rTable);

	// user data to be tracked by the hash table
	struct MyElement
	{
		int value;
		// to be linked by the hash table
		RNode entry;
		bool valid;
	};

	// insert all array elements into the hash table
	std::vector<MyElement> myData{ 100000 };
	for (auto item = 0; item < myData.size(); ++item)
	{
		myData[item].value = static_cast<int>(item);
		myData[item].valid = true;
		auto hashVal = std::hash<int>{}(item);
		bool inserted = rTableTryInsert(
				rTable,
				&myData[item].entry,
				hashVal,
				[](RNode* p1, RNode* p2)
				{
					return YJ_CONTAINER_OF(p1, MyElement, entry)->value ==
								 YJ_CONTAINER_OF(p2, MyElement, entry)->value;
				});
		assert(inserted);
	}

	// reader thread looks up
	auto readerFunc = [&rTable, &myData]()
	{
		for (auto i = 0; i < myData.size(); ++i)
		{
			RTableReadLockGuard l(rTable);
			auto hashVal = std::hash<int>{}(myData[i].value);
			RNode* pEntry = rTableFind(
					rTable,
					hashVal,
					[i](const RNode* p) { return YJ_CONTAINER_OF(p, MyElement, entry)->value == i; });
			// writer might erase elements that is not multiples of 8
			// but other elements should remain found
			if (i % 8 == 0)
			{
				assert(pEntry);
				assert(pEntry == &myData[i].entry);
				assert(YJ_CONTAINER_OF(pEntry, MyElement, entry)->valid);
			}
		}
	};

	// push reader threads
	const auto nrThreads = std::thread::hardware_concurrency();
	std::vector<std::future<void>> readerFutures{ nrThreads };
	for (auto& f : readerFutures)
		f = std::async(std::launch::async, readerFunc);

	// a single writer removes and add back elements in several rounds
	for (auto i = 0; i < myData.size(); ++i)
	{
		if (i % 8 == 0)
			continue;
		auto hash = std::hash<int>{}(myData[i].value);

		RNode* pEntry = rTableTryDetachAndSynchronize(
				rTable,
				hash,
				[val = myData[i].value](const RNode* p)
				{ return YJ_CONTAINER_OF(p, MyElement, entry)->value == val; });
		assert(pEntry);
		YJ_CONTAINER_OF(pEntry, MyElement, entry)->valid = false;
	}

	for (auto& f : readerFutures)
		f.get();
}

void printRCUTable(RTable& rTable)
{
	std::cout << "-------------------\n";
	auto pBucket = rTable.core.pBucketsInfo.load();
	for (auto iBucket = 0; iBucket < pBucket->nrBucketsPowerOf2; ++iBucket)
	{
		std::cout << "\nBucket " << iBucket << std::endl;
		auto pSrc = pBucket->pBuckets + iBucket;

		for (auto p = pSrc->list.next.load(std::memory_order_relaxed); p != nullptr;
				 p = p->next.load(std::memory_order_relaxed))
		{
			size_t hashV = YJ_CONTAINER_OF(p, RNode, head)->hash;
			std::cout << " hash: " << hashV;
		}
	}
	std::cout << "\n-------------------\n";
}
}	 // namespace yrcu

int main()
{
	// rcu zone tests
	yrcu::rcuTests();

	// a quick example of the rcu hash table (rTable)
	yrcu::rcuHashTableQuickExample();

	// stress tests
	yrcu::rTableTests();
}