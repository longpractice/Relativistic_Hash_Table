#pragma once

#include <atomic>

namespace yrcu
{
struct RcuDlistHead
{
	std::atomic<RcuDlistHead*> next;
	std::atomic<RcuDlistHead*> prev;
};

struct RcuDlist
{
	RcuDlistHead head;
};

}	 // namespace yrcu