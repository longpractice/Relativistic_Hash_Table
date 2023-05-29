#pragma once

#include "AtomicDoublyLinkedListTypes.h"

namespace yrcu
{
// similar to atomic single lists
// This atomic doubly linked list allows one concurrent writer with many concurrent readers
inline void atomicDlistInit(AtomicDoubleHead* list)
{
	list->prev.store(list, std::memory_order_relaxed);
	list->next.store(list, std::memory_order_relaxed);
}

namespace dlistDetail
{
	inline void
	atomicDlistAddBetween(AtomicDoubleHead* prev, AtomicDoubleHead* next, AtomicDoubleHead* newElem)
	{
		newElem->next.store(next, std::memory_order_release);
		newElem->prev.store(prev, std::memory_order_release);
		prev->next.store(newElem, std::memory_order_release);
		next->prev.store(newElem, std::memory_order_release);
	}

	inline void atomicDlistRemoveBetween(AtomicDoubleHead* prev, AtomicDoubleHead* next)
	{
		prev->next.store(next, std::memory_order_release);
		next->prev.store(prev, std::memory_order_release);
	}
}	 // namespace dlistDetail

// prepend to the front of the list
// write operation, must be serialized with other write operations
inline void atomicDlistPrepend(AtomicDoubleHead* list, AtomicDoubleHead* newElem)
{
	dlistDetail::atomicDlistAddBetween(list, list->next.load(std::memory_order_relaxed), newElem);
}

inline void atomicDlistAppend(AtomicDoubleHead* list, AtomicDoubleHead* newElem)
{
	dlistDetail::atomicDlistAddBetween(list->prev.load(std::memory_order_relaxed), list, newElem);
}

inline void atomicDlistRemove(AtomicDoubleHead* elem)
{
	dlistDetail::atomicDlistRemoveBetween(
			elem->prev.load(std::memory_order_relaxed), elem->next.load(std::memory_order_relaxed));
}

}	 // namespace yrcu