#pragma once

#include <atomic>

namespace yrcu
{
struct RcuSlistHead
{
	std::atomic<RcuSlistHead*> next;
};

struct RcuSlist
{
	RcuSlistHead head;
};
}	 // namespace yrcu