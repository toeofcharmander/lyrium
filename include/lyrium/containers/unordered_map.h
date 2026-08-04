#pragma once

#include <unordered_map>

#include "lyrium/allocators/std_allocator.h"

namespace lyrium
{
template <class Key, class T, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>>
using UnorderedMap = std::unordered_map<Key, T, Hash, KeyEqual, STDAllocator<std::pair<const Key, T>>>;
}
