#pragma once

#include <cassert>
#include <cstddef>
#include <libloaderapi.h>
#include <mutex>

#include <windows.h>

#include "eluvian/allocators/free_list_allocator.h"
#include "eluvian/allocators/page_allocator.h"

namespace eluvian
{

class GlobalAllocator
{
  public:
    static auto instance() -> GlobalAllocator &
    {
        static auto inst = GlobalAllocator{};
        return inst;
    }

    static auto allocate(std::size_t size) -> void *
    {
        const auto lock = instance().create_lock();
        return instance().allocator_.allocate(size);
    }

    static auto deallocate(void *allocation) -> void
    {
        const auto lock = instance().create_lock();
        instance().allocator_.deallocate(allocation);
    }

  private:
    struct VirtualAllocHelper
    {
        auto operator()(std::size_t size) const -> void *
        {
            return VirtualAlloc_orig(nullptr, size, MEM_COMMIT, PAGE_READWRITE);
        }
    };

    struct VirtualDeallocHelper
    {
        auto operator()(void *allocation) const -> void
        {
            VirtualFree_orig(allocation, 0, MEM_RELEASE);
        }
    };

    auto create_allocator()
    {
        const auto kernel32 = ::LoadLibraryA("kernel32.dll");
        assert(kernel32 != nullptr);

        if (kernel32 != nullptr)
        {
            VirtualAlloc_orig =
                reinterpret_cast<decltype(VirtualAlloc_orig)>(::GetProcAddress(kernel32, "VirtualAlloc"));
            VirtualFree_orig = reinterpret_cast<decltype(VirtualFree_orig)>(::GetProcAddress(kernel32, "VirtualFree"));
        }

        if (VirtualAlloc_orig == nullptr)
        {
            VirtualAlloc_orig = &::VirtualAlloc;
        }
        if (VirtualFree_orig == nullptr)
        {
            VirtualFree_orig = &::VirtualFree;
        }

        static constexpr auto allocation_size = 1024zu * 1000zu * 1zu;

        auto page_allocator = PageAllocator{VirtualAllocHelper{}, VirtualDeallocHelper{}};
        return FreeListAllocator{std::move(page_allocator), allocation_size};
    }

    GlobalAllocator()
        : allocator_{create_allocator()}
        , mutex_{}
    {
    }

    auto create_lock() -> std::scoped_lock<std::mutex>
    {
        return std::scoped_lock(mutex_);
    }

    static inline ::LPVOID(WINAPI *VirtualAlloc_orig)(
        ::LPVOID lpAddress, ::SIZE_T dwSize, ::DWORD flAllocationType, ::DWORD flProtect);
    static inline ::BOOL(WINAPI *VirtualFree_orig)(::LPVOID lpAddress, ::SIZE_T dwSize, ::DWORD dwFreeType);
    FreeListAllocator<PageAllocator<VirtualAllocHelper, VirtualDeallocHelper>> allocator_;
    std::mutex mutex_;
};

}
