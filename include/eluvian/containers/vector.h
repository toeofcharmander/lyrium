#pragma once

#include <vector>

#include "eluvian/allocators/std_allocator.h"

namespace eluvian
{
template <class T>
using Vector = std::vector<T, STDAllocator<T>>;
}
