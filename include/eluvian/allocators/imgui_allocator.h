#pragma once

#include <cstddef>

#include "eluvian/allocators/global_allocator.h"

namespace eluvian
{

inline auto imgui_allocator(std::size_t size, void *)
{
    return GlobalAllocator::allocate(size);
}

inline auto imgui_deallocator(void *allocation, void *)
{
    GlobalAllocator::deallocate(allocation);
}

}
