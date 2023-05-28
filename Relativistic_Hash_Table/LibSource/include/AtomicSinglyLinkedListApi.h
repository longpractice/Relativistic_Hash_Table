#include "AtomicSinglyLinkedListTypes.h"

#define YJ_OFFSET_OF(Type, Field)					 __builtin_offsetof(Type, Field)
#define YJ_CONTAINER_OF(ptr, type, member) ((type*)((char*)ptr - YJ_OFFSET_OF(type, member)))

namespace yrcu
{
//*************** Atomic singly list **************//
// Write operations (insert and erase) must be serialized, and the atomic singly
// linked list itself does not require any rcu synchronization.
//
// External RCU synchronization is only required if there are readers who wants
// to read elements and there are writers who wants to destruct the element
// after the unlinking of element. If the erased (here it means that it is only
// unlinked but not yet destroyed).

inline void atomicSlistInit(AtomicSingleHead* list)
{
	// no memory orders since initialization should never be executing in parallel
	// with other apis
	list->next.store(nullptr, std::memory_order_relaxed);
}

// Write operation(must be serialized with other write operations)
// link elem to the front of the list
inline void atomicSlistPrepend(AtomicSingleHead* list, AtomicSingleHead* newElem)
{
	// Since writers are serialized, inside writing apis we load atomic with
	// mem-order-relaxed. But since there might be concurrent readers, inside
	// writing apis we store with mem-order-release.
	AtomicSingleHead* pNext = list->next.load(std::memory_order_relaxed);
	newElem->next.store(pNext, std::memory_order_release);
	list->next.store(newElem, std::memory_order_release);
}

// Append the element to the end of the list
// Note that this will cause a traversal of the list
inline void atomicSlistAppend(AtomicSingleHead* list, AtomicSingleHead* newElem)
{
	AtomicSingleHead* p = list;
	AtomicSingleHead* pNext = p->next.load(std::memory_order_relaxed);
	while (pNext)
	{
		p = pNext;
		pNext = p->next.load(std::memory_order_relaxed);
	}

	newElem->next.store(nullptr, std::memory_order_release);
	p->next.store(newElem, std::memory_order_release);
}

// Write operation(must be serialized with other write operations)
// Insert the element to the front of the list, if the element is not found from list.
// Note that there is a slight different from combining find and insertFront
// since this function is serialized with other writers, we can relax some load operations
// inside.
template<typename BinaryPredict>
inline bool atomicSlistPrependIfNoMatch(
		AtomicSingleHead* list,
		AtomicSingleHead* newElem,
		BinaryPredict binaryPredict)
{
	AtomicSingleHead* pFirst = list->next.load(std::memory_order_relaxed);
	AtomicSingleHead* p;
	for (p = pFirst; p != nullptr; p = p->next.load(std::memory_order_relaxed))
		if (binaryPredict(p, newElem))
			return false;
	// put the new element in the front
	newElem->next.store(pFirst, std::memory_order_release);
	list->next.store(newElem, std::memory_order_release);
	return true;
}


// Write operation(must be serialized with other write operations)
// Append the element to the end of the list, if the element is not found from list.
// Note that there is a slight different from combining find and insertFront
// since this function is serialized with other writers, we can relax some load operations
// inside.
template<typename BinaryPredict>
inline bool atomicSlistAppendIfNoMatch(
		AtomicSingleHead* list,
		AtomicSingleHead* newElem,
		BinaryPredict binaryPredict)
{
	AtomicSingleHead* p = list;
	AtomicSingleHead* pNext = p->next.load(std::memory_order_relaxed);

	while (pNext)
	{
		if (binaryPredict(p, newElem))
			return false;
		p = pNext;
		pNext = p->next.load(std::memory_order_relaxed);
	}
	newElem->next.store(nullptr, std::memory_order_release);
	p->next.store(newElem, std::memory_order_release);
	return true;
}

// Find first element that predict(AtomicSingleHead*) returns true.
// UnaryPredict is of signature `bool(AtomicSingleHead*)`.
// If none is find, nullptr is returned
template<typename UnaryPredict>
AtomicSingleHead* atomicSlistFindIf(AtomicSingleHead* list, UnaryPredict predict)
{
	for (AtomicSingleHead* p = list->next.load(std::memory_order_acquire); p != nullptr;
			 p = p->next.load(std::memory_order_acquire))
		if (predict(p))
			return p;
	return nullptr;
}

// Try find the first element inside
template<typename UnaryPredict>
AtomicSingleHead* atomicSlistRemoveIf(AtomicSingleHead* list, UnaryPredict predict)
{
	AtomicSingleHead* pLast = list;
	AtomicSingleHead* p = pLast->next.load(std::memory_order_relaxed);
	while (p)
	{
		if (predict(p))
		{
			pLast->next.store(p->next.load(std::memory_order_relaxed), std::memory_order_release);
			break;
		}
		pLast = p;
		p = p->next.load(std::memory_order_relaxed);
	}
	return p;
}

// Head will be returned if it exists in the list
// Otherwise, nullptr is returned
inline AtomicSingleHead* atomicSlistRemove(AtomicSingleHead* list, AtomicSingleHead* head)
{
	AtomicSingleHead* pLast = list;
	AtomicSingleHead* p = pLast->next.load(std::memory_order_relaxed);

	while (p)
	{
		if (p == head)
		{
			pLast->next.store(p->next.load(std::memory_order_relaxed), std::memory_order_release);
			break;
		}
		pLast = p;
		p = p->next.load(std::memory_order_relaxed);
	}
	return p;
}

}	 // namespace yrcu
