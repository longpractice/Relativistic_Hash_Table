#pragma once
#include "AtomicSinglyLinkedListApi.h"
#include "RCUApi.h"
#include "RCUHashTableTypes.h"

namespace yrcu
{
namespace rTableCoreDetail
{
	void expandBucketsByFac2IfNecessary(
			size_t nrElements,
			size_t nrBuckets,
			RTableCore& table,
			RCUZone& zone);
	bool shrinkBucketsByFac2IfNecessary(
			size_t nrElements,
			size_t nrBuckets,
			RTableCore& table,
			RCUZone& zone);
}	 // namespace rTableCoreDetail

//////////////////////////////////////////////
//         Advanced API
/////////////////////////////////////////////

struct RTableCoreConfig
{
	int nrBuckets = 64;
	float expandFactor = 1.1f;
	float shrinkFactor = 0.25f;
};

void rTableCoreInitDetailed(RTableCore& table, const RTableCoreConfig& conf);

// Op is of function signature of bool(const RNode* p0), which returns if the entry is
// what you are looking try erase but no synchronize This enables the caller to
// do several rcuHashTableTryDetach operations, do one rcuHashTableSynchronize
// and then do all the garbage collections.
template<typename UnaryPredicate>
RNode* rTableCoreTryDetachNoShrink(RTableCore& table, size_t hashVal, UnaryPredicate predict)
{
	RTableCore::BucketsInfo* pBucketsInfo = table.pBucketsInfo.load(std::memory_order_acquire);
	size_t bucketHash = pBucketsInfo->nrBucketsPowerOf2 - 1;
	auto bucketId = hashVal & bucketHash;
	RTableCore::Bucket* pBucket = pBucketsInfo->pBuckets + bucketId;
	auto predictInner = [&predict](const AtomicSingleHead* p)
	{ return predict(YJ_CONTAINER_OF(p, RNode, head)); };
	AtomicSingleHead* pRemoved = atomicSlistRemoveIf(&pBucket->list, predictInner);
	if (!pRemoved)
		return nullptr;
	return YJ_CONTAINER_OF(pRemoved, RNode, head);
}

// might shrink automatically
// But will not do any synchronization
template<typename Op>
RNode* rTableCoreTryDetachAutoShrink(
		RTableCore& table,
		size_t hashVal,
		Op matchOp,
		RCUZone& zone,
		bool* outIfAlreadyRcuSynrhonized = nullptr)
{
	RNode* p = rTableCoreTryDetachNoShrink(table, hashVal, std::move(matchOp));
	if (!p)
	{
		if (outIfAlreadyRcuSynrhonized)
			*outIfAlreadyRcuSynrhonized = false;
		return p;
	}
	else
	{
		size_t currentSize = table.size.load(std::memory_order_relaxed);
		size_t nrBuckets = table.pBucketsInfo.load(std::memory_order_acquire)->nrBucketsPowerOf2;
		bool ifAlreadyRcuSynrhonized =
				rTableCoreDetail::shrinkBucketsByFac2IfNecessary(currentSize, nrBuckets, table, zone);
		if (outIfAlreadyRcuSynrhonized)
			*outIfAlreadyRcuSynrhonized = ifAlreadyRcuSynrhonized;
		return p;
	}
}

template<typename BinaryPredict>
bool rTableCoreTryInsertNoExpand(
		RTableCore& table,
		RNode* pEntry,
		size_t hashVal,
		BinaryPredict binaryPredict)
{
	pEntry->hash = hashVal;
	RTableCore::BucketsInfo* pBucketsInfo = table.pBucketsInfo.load(std::memory_order_relaxed);
	size_t bucketMask = pBucketsInfo->nrBucketsPowerOf2 - 1;
	auto bucketId = pEntry->hash & bucketMask;
	RTableCore::Bucket* pBucket = pBucketsInfo->pBuckets + bucketId;
	auto binaryPredictInner = [&binaryPredict](const AtomicSingleHead* p1, const AtomicSingleHead* p2)
	{ return binaryPredict(YJ_CONTAINER_OF(p1, RNode, head), YJ_CONTAINER_OF(p2, RNode, head)); };
	bool inserted = atomicSlistPrependIfNoMatch(&pBucket->list, &pEntry->head, binaryPredictInner);
	if (inserted)
		table.size.fetch_add(1, std::memory_order_relaxed);
	return inserted;
}

// can only be called if the user is sure that no dup exists
void rTableCoreInsertNoExpand(RTableCore& table, RNode* pEntry);

void rTableCoreExpandBuckets2x(RTableCore& table, RCUZone& zone);

bool rTableCoreShrinkBuckets2x(RTableCore& table, RCUZone& zone);

//-----------------------------------------------------------------------------------------------//

///////////////////////////////////////////////////////////////////////////////////////////////////
// Basic API
///////////////////////////////////////////////////////////////////////////////////////////////////
//
// hash table must be initialized, nrBuckets is advised to be bigger than the
// predicted element count
void rTableCoreInit(RTableCore& table);

// Read operation
//\parameter hashVal should be the hash value of the find target, only table
// entries with a hash value equals to `hashVal` is checked for
//  equality with matchOp.
// op should have function signature of bool(RNode*) to identify if the RNode
// equals to what is to be found
template<typename UnaryPrediction>
RNode* rTableCoreFind(const RTableCore& table, size_t hashVal, UnaryPrediction predict)
{
	RTableCore::BucketsInfo* pBucketsInfo = table.pBucketsInfo.load(std::memory_order_acquire);
	size_t bucketHash = pBucketsInfo->nrBucketsPowerOf2 - 1;
	auto bucketId = hashVal & bucketHash;
	RTableCore::Bucket* pBucket = pBucketsInfo->pBuckets + bucketId;
	auto predictInner = [&predict](const AtomicSingleHead* p)
	{ return predict(YJ_CONTAINER_OF(p, RNode, head)); };
	AtomicSingleHead* pFound = atomicSlistFindIf(&pBucket->list, predictInner);
	if (!pFound)
		return nullptr;
	return YJ_CONTAINER_OF(pFound, RNode, head);
}

// Write operation: all writers must be serialized
// Op is of function signature of bool(RNode* p0, RcuHashTaleEntry* p1), which
// returns if two hash table entries are equivalent. Expand if necessary
template<typename Op>
bool rTableCoreTryInsert(
		RTableCore& table,
		RCUZone& rcuZone,
		RNode* pEntry,
		size_t hashVal,
		Op matchOp)
{
	bool inserted = rTableCoreTryInsertNoExpand(table, pEntry, hashVal, matchOp);
	if (!inserted)
		return false;
	auto currentSize = table.size.load(std::memory_order_relaxed);
	size_t nrBuckets = table.pBucketsInfo.load(std::memory_order_acquire)->nrBucketsPowerOf2;
	rTableCoreDetail::expandBucketsByFac2IfNecessary(currentSize, nrBuckets, table, rcuZone);
	return true;
}

// Write operation: all writers must be serialized
// Op is of function signature of bool(RNode* p0), which returns if the entry is
// what you are looking for. rcuSynchronize is called internally and it is safe
// to delete resource of RNode.
template<typename Op>
RNode*
rTableCoreTryDetachAndSynchronize(RTableCore& table, RCUZone& rcuZone, size_t hashVal, Op matchOp)
{
	bool ifAlreadyRcuSynchronized = false;
	RNode* pEntry =
			rTableCoreTryDetachAutoShrink(table, hashVal, matchOp, rcuZone, &ifAlreadyRcuSynchronized);
	if (pEntry == nullptr)
		return pEntry;
	if (!ifAlreadyRcuSynchronized)
		rcuSynchronize(rcuZone);
	return pEntry;
}
}	 // namespace yrcu
