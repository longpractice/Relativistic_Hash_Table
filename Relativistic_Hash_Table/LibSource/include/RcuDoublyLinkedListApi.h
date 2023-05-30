#pragma once

#include "RcuDoublyLinkedListTypes.h"

namespace yrcu
{
// similar to rcu single lists
// This rcu doubly linked list allows one concurrent writer with many concurrent readers
inline void rcuDlistInit(RcuDlist* list)
{
	RcuDlistHead* head = &list->head;
	head->prev.store(head, std::memory_order_relaxed);
	head->next.store(head, std::memory_order_relaxed);
}

inline void rcuDlistRest(RcuDlist* list)
{
	RcuDlistHead* head = &list->head;
	head->prev.store(head, std::memory_order_release);
	head->next.store(head, std::memory_order_release);
}

inline bool rcuDlistEmpty(RcuDlist* list)
{
	return list->head.next.load(std::memory_order_acquire) == &list->head;
}

namespace dlistDetail
{
	inline void rcuDlistAddBetween(RcuDlistHead* prev, RcuDlistHead* next, RcuDlistHead* newElem)
	{
		newElem->next.store(next, std::memory_order_release);
		newElem->prev.store(prev, std::memory_order_release);
		prev->next.store(newElem, std::memory_order_release);
		next->prev.store(newElem, std::memory_order_release);
	}

	inline void rcuDlistRemoveBetween(RcuDlistHead* prev, RcuDlistHead* next)
	{
		prev->next.store(next, std::memory_order_release);
		next->prev.store(prev, std::memory_order_release);
	}
}	 // namespace dlistDetail

// prepend to the front of the list
// write operation, must be serialized with other write operations
inline void rcuDlistInsertAfter(RcuDlistHead* pos, RcuDlistHead* newElem)
{
	dlistDetail::rcuDlistAddBetween(pos, pos->next.load(std::memory_order_relaxed), newElem);
}

inline void rcuDlistInsertBefore(RcuDlistHead* pos, RcuDlistHead* newElem)
{
	dlistDetail::rcuDlistAddBetween(pos->prev.load(std::memory_order_relaxed), pos, newElem);
}

inline void rcuDlistAppend(RcuDlist* list, RcuDlistHead* newElem)
{
	rcuDlistInsertBefore(&list->head, newElem);
}

inline void rcuDlistPrepend(RcuDlist* list, RcuDlistHead* newElem)
{
	rcuDlistInsertAfter(&list->head, newElem);
}

inline void rcuDlistRemove(RcuDlistHead* elem)
{
	dlistDetail::rcuDlistRemoveBetween(
			elem->prev.load(std::memory_order_relaxed), elem->next.load(std::memory_order_relaxed));
}

//////////Read-only Iterator that is not safe to erase in the middle, forward//////
struct RcuDlistIter
{
	RcuDlistHead* pos;
};

inline RcuDlistIter rcuDlistBegin(const RcuDlist* list)
{
	RcuDlistIter it;
	it.pos = list->head.next.load(std::memory_order_acquire);
	return it;
}

inline bool rcuDlistIsEnd(const RcuDlistIter& it, RcuDlist* list)
{
	return it.pos == &list->head;
}

inline void rcuDlistAdvance(RcuDlistIter& it)
{
	it.pos = it.pos->next.load(std::memory_order_acquire);
}

//////////Read-only Iterator that is not safe to erase in the middle, reverse//////
struct RcuDlistRIter
{
	RcuDlistHead* pos;
};

inline RcuDlistRIter rcuDlistRBegin(const RcuDlist* list)
{
	RcuDlistRIter it;
	it.pos = list->head.prev.load(std::memory_order_acquire);
	return it;
}

inline bool rcuDlistIsREnd(const RcuDlistRIter& it, RcuDlist* list)
{
	return it.pos == &list->head;
}

inline void rcuDlistRAdvance(RcuDlistRIter& it)
{
	it.pos = it.pos->prev.load(std::memory_order_acquire);
}

inline RcuDlistHead* rcuDlistRGetHead(const RcuDlistIter& it)
{
	return it.pos;
}

///////////////////////////////////////////////////////////////////////
////Safe iterator such that you can erase when you are iterating
////This is specific for writing operations and thus must be
////serialized with other writing operations (even if you are only using
////this iterator for read only operations
///////////////////////////////////////////////////////////////////////

struct RcuDlistSafeIter
{
	RcuDlistHead* pos;
	RcuDlistHead* next;
};

inline RcuDlistSafeIter rcuDlistSafeBegin(RcuDlist* list)
{
	RcuDlistSafeIter it;
	it.pos = list->head.next.load(std::memory_order_relaxed);
	it.next = it.pos->next.load(std::memory_order_relaxed);
	return it;
}

inline bool rcuDlistSafeIsEnd(const RcuDlistSafeIter& it, RcuDlist* list)
{
	return it.pos == &list->head;
}

inline void rcuDlistSafeAdvance(RcuDlistSafeIter& it)
{
	it.pos = it.next;
	it.next = it.pos->next.load(std::memory_order_relaxed);
}

inline RcuDlistHead* rcuDlistSafeGetHead(const RcuDlistSafeIter& it)
{
	return it.pos;
}

// after remove you can still advance
inline void rcuDlistSafeRemoveIt(RcuDlistSafeIter& it)
{
	rcuDlistRemove(it.pos);
}

////////////////////////////////////
// Reverse safe iterator
//////////////////////////////////
struct RcuDlistSafeRIter
{
	RcuDlistHead* pos;
	RcuDlistHead* prev;
};

inline RcuDlistSafeRIter rcuDlistSafeRBegin(RcuDlist* list)
{
	RcuDlistSafeRIter it;
	it.pos = list->head.prev.load(std::memory_order_relaxed);
	it.prev = it.pos->prev.load(std::memory_order_relaxed);
	return it;
}

inline bool rcuDlistSafeRIsEnd(const RcuDlistSafeRIter& it, RcuDlist* list)
{
	return it.pos == &list->head;
}

inline void rcuDlistSafeRAdvance(RcuDlistSafeRIter& it)
{
	it.pos = it.prev;
	it.prev = it.pos->prev.load(std::memory_order_relaxed);
}

inline RcuDlistHead* rcuDlistSafeRIterGetHead(const RcuDlistSafeRIter& it)
{
	return it.pos;
}

// after remove you can still advance
inline void rcuDlistSafeRRemoveIt(RcuDlistSafeRIter& it)
{
	rcuDlistRemove(it.pos);
}

}	 // namespace yrcu