#pragma once

#include <unordered_set>

#include "eluvian/allocators/std_allocator.h"

namespace eluvian
{
template <class Key, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>>
using UnorderedSet = std::unordered_set<Key, Hash, KeyEqual, STDAllocator<Key>>;
}
