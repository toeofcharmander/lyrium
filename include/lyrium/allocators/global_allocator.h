#pragma once

#include <cstddef>
#include <new>

#include <windows.h>

namespace lyrium
{

class GlobalAllocator
{
  public:
    static auto allocate(std::size_t size) -> void *
    {
        const auto allocation = ::HeapAlloc(heap(), 0, size == 0u ? 1u : size);
        if (allocation == nullptr)
        {
            throw std::bad_alloc{};
        }
        return allocation;
    }

    static auto deallocate(void *allocation) -> void
    {
        if (allocation != nullptr)
        {
            ::HeapFree(heap(), 0, allocation);
        }
    }

  private:
    static auto heap() -> ::HANDLE
    {
        static const auto handle = ::HeapCreate(0, 0, 0);
        if (handle == nullptr)
        {
            throw std::bad_alloc{};
        }
        return handle;
    }
};

}
