#include "RcuSinglyLinkedListTypes.h"

#define YJ_OFFSET_OF(Type, Field)					 __builtin_offsetof(Type, Field)
#define YJ_CONTAINER_OF(ptr, type, member) ((type*)((char*)ptr - YJ_OFFSET_OF(type, member)))

namespace yrcu
{
//*************** Atomic singly list **************//
// Write operations (insert and erase) must be serialized, and the std::atomic singly
// linked list itself does not require any rcu synchronization.
//
// External RCU synchronization is only required if there are readers who wants
// to read elements and there are writers who wants to destruct the element
// after the unlinking of element. If the erased (here it means that it is only
// unlinked but not yet destroyed).

inline void rcuSlistInit(RcuSlist* list)
{
	// Initializing a never initialized list would only require relaxed,
   // since the previous value is random.
	list->head.next.store(nullptr, std::memory_order_relaxed);
}

//previous value and newly initialized value diff might carry a release-acquire 
//memory order, for example, one thread reset the list and another thread checks
//of the list is empty.
inline void rcuSlistReset(RcuSlist* list)
{
	list->head.next.store(nullptr, std::memory_order_release);
}

inline bool rcuSlistEmpty(RcuSlist* list)
{
	return list->head.next.load(std::memory_order_acquire) == nullptr;
}

// Write operation(must be serialized with other write operations)
// add the new element right after pos, which could be a list or an element in the list
inline void rcuSlistInsertAfter(RcuSlistHead* pos, RcuSlistHead* newElem)
{
	// Since writers are serialized, inside writing apis we load std::atomic with
	// mem-order-relaxed. But since there might be concurrent readers, inside
	// writing apis we store with mem-order-release.
	RcuSlistHead* pNext = pos->next.load(std::memory_order_relaxed);
	newElem->next.store(pNext, std::memory_order_release);
	pos->next.store(newElem, std::memory_order_release);
}

// Find the tail starting from pos and append the new element.
// (Pos could be the list itself, and then this function just append the element to the
// end of the list)
inline void rcuSlistAppendToTail(RcuSlistHead* pos, RcuSlistHead* newElem)
{
	RcuSlistHead* p = pos;
	RcuSlistHead* pNext = p->next.load(std::memory_order_relaxed);
	while (pNext)
	{
		p = pNext;
		pNext = p->next.load(std::memory_order_relaxed);
	}
	// put the new element at the end
	// order of the lines below matters since there might be concurrent readers
	newElem->next.store(nullptr, std::memory_order_release);
	p->next.store(newElem, std::memory_order_release);
}

// Write operation(must be serialized with other write operations)
// Insert the element to the front of the list, if the element is not found from list.
// Note that there is a slight different from combining find and insertFront
// since this function is serialized with other writers, we can relax some load operations
// inside.
template<typename BinaryPredict>
inline bool
rcuSlistPrependIfNoMatch(RcuSlist* list, RcuSlistHead* newElem, BinaryPredict binaryPredict)
{
	RcuSlistHead* pFirst = list->head.next.load(std::memory_order_relaxed);
	RcuSlistHead* p;
	for (p = pFirst; p != nullptr; p = p->next.load(std::memory_order_relaxed))
		if (binaryPredict(p, newElem))
			return false;
	// put the new element in the front
	// order of the lines below matters since there might be concurrent readers
	newElem->next.store(pFirst, std::memory_order_release);
	list->head.next.store(newElem, std::memory_order_release);
	return true;
}

// Write operation(must be serialized with other write operations)
// Append the element to the end of the list, if the element is not found from list.
// Note that there is a slight different from combining find and insertFront
// since this function is serialized with other writers, we can relax some load operations
// inside.
template<typename BinaryPredict>
inline bool
rcuSlistAppendIfNoMatch(RcuSlist* list, RcuSlistHead* newElem, BinaryPredict binaryPredict)
{
	RcuSlistHead* p = &list->head;
	RcuSlistHead* pNext = p->next.load(std::memory_order_relaxed);

	while (pNext)
	{
		if (binaryPredict(p, newElem))
			return false;
		p = pNext;
		pNext = p->next.load(std::memory_order_relaxed);
	}
	// order of lines below matters, due to concurrent readers
	newElem->next.store(nullptr, std::memory_order_release);
	p->next.store(newElem, std::memory_order_release);
	return true;
}

// Find first element that predict(RcuSlistHead*) returns true.
// UnaryPredict is of signature `bool(RcuSlistHead*)`.
// If none is find, nullptr is returned
template<typename UnaryPredict>
RcuSlistHead* rcuSlistFindIf(RcuSlist* list, UnaryPredict predict)
{
	for (RcuSlistHead* p = list->head.next.load(std::memory_order_acquire); p != nullptr;
			 p = p->next.load(std::memory_order_acquire))
		if (predict(p))
			return p;
	return nullptr;
}

// Try find the first element that matches the predict and unlink it from the list
// Note that since there might be concurrent readers going on,
// the caller needs to wait for all the readers to expire before touching returned head's next
// member or dispose it.
template<typename UnaryPredict>
RcuSlistHead* rcuSlistRemoveIf(RcuSlist* list, UnaryPredict predict)
{
	RcuSlistHead* pLast = &list->head;
	RcuSlistHead* p = pLast->next.load(std::memory_order_relaxed);
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
inline RcuSlistHead* rcuSlistRemove(RcuSlist* list, RcuSlistHead* head)
{
	RcuSlistHead* pLast = &list->head;
	RcuSlistHead* p = pLast->next.load(std::memory_order_relaxed);

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
