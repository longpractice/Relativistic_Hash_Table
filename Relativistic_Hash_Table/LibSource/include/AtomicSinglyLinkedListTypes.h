#pragma once

#include <atomic>

namespace yrcu
{
    struct AtomicSingleHead
    {
        std::atomic<AtomicSingleHead*> next;
    };
}