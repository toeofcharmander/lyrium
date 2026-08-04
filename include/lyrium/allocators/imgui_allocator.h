#pragma once

#include <cstddef>

#include <windows.h>

namespace lyrium
{

namespace detail
{
inline auto imgui_heap() -> ::HANDLE
{
    static const auto handle = ::HeapCreate(0, 0, 0);
    return handle;
}
}

inline auto imgui_allocator(std::size_t size, void *)
{
    const auto heap = detail::imgui_heap();
    return heap != nullptr ? ::HeapAlloc(heap, 0, size == 0u ? 1u : size) : nullptr;
}

inline auto imgui_deallocator(void *allocation, void *)
{
    const auto heap = detail::imgui_heap();
    if (heap != nullptr && allocation != nullptr)
    {
        ::HeapFree(heap, 0, allocation);
    }
}

}
