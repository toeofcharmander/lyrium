#pragma once

#include <vector>

#include "lyrium/allocators/std_allocator.h"

namespace lyrium
{
template <class T>
using Vector = std::vector<T, STDAllocator<T>>;
}
