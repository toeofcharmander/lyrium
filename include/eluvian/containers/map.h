#pragma once

#include <functional>
#include <map>

#include "eluvian/allocators/std_allocator.h"

namespace eluvian
{

template <class Key, class T, class Compare = std::less<Key>>
using Map = std::map<Key, T, Compare, STDAllocator<std::pair<const Key, T>>>;

}
