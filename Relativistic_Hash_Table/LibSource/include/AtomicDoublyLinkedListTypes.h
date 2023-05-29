#pragma once

#include <atomic>

namespace yrcu
{
struct AtomicDoubleHead
{
	std::atomic<AtomicDoubleHead*> next;
	std::atomic<AtomicDoubleHead*> prev;
};
}	 // namespace yrcu