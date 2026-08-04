#pragma once

#include <functional>
#include <map>

#include "lyrium/allocators/std_allocator.h"

namespace lyrium
{

template <class Key, class T, class Compare = std::less<Key>>
using Map = std::map<Key, T, Compare, STDAllocator<std::pair<const Key, T>>>;

}
