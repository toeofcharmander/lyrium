#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string_view>

#include <windows.h>

#include "lyrium/allocators/arena.h"
#include "lyrium/log.h"
#include "lyrium/utils.h"

namespace lyrium
{

// Serves d3d9.dll's large heap allocations from one contiguous reservation.
//
// The problem this exists for, measured: the D3D9 runtime allocates a full
// system-memory duplicate for every MANAGED texture through the process heap,
// and the NT heap sends anything past its 508 KB VirtualMemoryThreshold straight
// to its own dedicated NtAllocateVirtualMemory. So roughly 150 MB of duplicates
// churning per session becomes hundreds of separate reservations, appearing and
// disappearing at varying sizes, cutting the low 2 GB into pieces that never
// coalesce because live allocations sit between them. A live session ended with
// 101.6 MB free below the line in 339 blocks averaging 300 KB, largest 9.2 MB.
//
// Routing those through an Arena replaces hundreds of reservations with one.
// The same churn still fragments -- inside the arena, where it coalesces on
// every free and where nothing else in the process can see it.
//
// Three things make interposing on another module's allocator defensible here
// rather than reckless:
//
// 1. The arena is one contiguous range we reserved, so ownership is a two
//    comparison test with no false positives. A pointer is ours or it is not.
// 2. RtlFreeHeap is where every HeapFree from every module funnels, so it does
//    not matter who frees the pointer -- the driver, D3DX, the game. They all
//    arrive at one place that range-checks first.
// 3. Anything the arena cannot serve falls through to the real HeapAlloc, so a
//    full arena degrades to the behaviour that existed before this.
//
// The residual hazard, stated rather than hidden: an arena pointer has no heap
// header in front of it. Code that reads ((HEAP_ENTRY*)ptr)[-1] -- heap walking,
// some validation paths -- gets garbage, and that corrupts rather than crashes.
// Nothing in d3d9.dll's import table suggests it does this, but an import table
// cannot prove a negative. This is why the whole thing is off by default.

namespace detail
{

inline std::mutex arena_mutex{};
inline Arena *arena_instance{nullptr};

inline std::atomic<std::uint64_t> interposed_allocations{};
inline std::atomic<std::uint64_t> interposed_bytes{};
inline std::atomic<std::uint64_t> interposed_frees{};
inline std::atomic<std::uint64_t> passed_through{};
inline std::atomic<std::uint64_t> arena_full_fallbacks{};
inline std::atomic<std::uint64_t> foreign_frees_seen{};

inline std::atomic<std::size_t> interpose_threshold{512u * 1024u};

using HeapAllocFn = ::LPVOID(WINAPI *)(::HANDLE, ::DWORD, ::SIZE_T);
using RtlFreeHeapFn = ::BOOLEAN(NTAPI *)(::PVOID, ::ULONG, ::PVOID);

inline std::atomic<HeapAllocFn> heap_alloc_original{nullptr};
inline std::atomic<RtlFreeHeapFn> rtl_free_heap_original{nullptr};
inline std::atomic<::HANDLE> process_heap{nullptr};

// Never called with the arena lock held by this thread; the arena does no
// allocation of its own, so there is no re-entry through here.
[[nodiscard]] inline auto arena_allocate(std::size_t bytes) -> void *
{
    auto lock = std::scoped_lock{arena_mutex};
    return arena_instance != nullptr ? arena_instance->allocate(bytes) : nullptr;
}

[[nodiscard]] inline auto arena_owns(const void *allocation) -> bool
{
    auto lock = std::scoped_lock{arena_mutex};
    return arena_instance != nullptr && arena_instance->owns(allocation);
}

[[nodiscard]] inline auto arena_deallocate(void *allocation) -> bool
{
    auto lock = std::scoped_lock{arena_mutex};
    return arena_instance != nullptr && arena_instance->deallocate(allocation);
}

// Replaces d3d9.dll's HeapAlloc import. Only large requests on the process heap
// are diverted; everything else is the original call, unchanged.
inline auto WINAPI heap_alloc_detour(::HANDLE heap, ::DWORD flags, ::SIZE_T bytes) -> ::LPVOID
{
    const auto original = heap_alloc_original.load(std::memory_order_relaxed);
    const auto call_original = [&] { return original(heap, flags, bytes); };

    if (original == nullptr)
    {
        return nullptr;
    }
    if (heap != process_heap.load(std::memory_order_relaxed) ||
        bytes < interpose_threshold.load(std::memory_order_relaxed))
    {
        passed_through.fetch_add(1u, std::memory_order_relaxed);
        return call_original();
    }

    auto *served = arena_allocate(bytes);
    if (served == nullptr)
    {
        arena_full_fallbacks.fetch_add(1u, std::memory_order_relaxed);
        return call_original();
    }

    // HEAP_ZERO_MEMORY is the only flag whose contract we have to honour
    // ourselves; the arena hands back whatever the previous tenant left.
    if ((flags & HEAP_ZERO_MEMORY) != 0u)
    {
        std::memset(served, 0, bytes);
    }

    interposed_allocations.fetch_add(1u, std::memory_order_relaxed);
    interposed_bytes.fetch_add(bytes, std::memory_order_relaxed);
    return served;
}

// Hooked in ntdll, so it sees every free in the process from every module. The
// range check runs first and is the whole safety argument: a pointer outside the
// arena is passed to the real implementation untouched, exactly as if this hook
// did not exist.
inline auto NTAPI rtl_free_heap_detour(::PVOID heap, ::ULONG flags, ::PVOID allocation) -> ::BOOLEAN
{
    if (allocation != nullptr && arena_owns(allocation))
    {
        if (arena_deallocate(allocation))
        {
            interposed_frees.fetch_add(1u, std::memory_order_relaxed);
            return TRUE;
        }
    }

    foreign_frees_seen.fetch_add(1u, std::memory_order_relaxed);
    const auto original = rtl_free_heap_original.load(std::memory_order_relaxed);
    return original != nullptr ? original(heap, flags, allocation) : FALSE;
}

// Redirects one named import in one module. Deliberately narrow: only
// d3d9.dll's HeapAlloc is replaced, so no other module's allocations change
// hands.
inline auto redirect_import(::HMODULE module, const char *name, void *replacement, void **previous) -> bool
{
    auto *base = reinterpret_cast<std::uint8_t *>(module);
    const auto *dos = reinterpret_cast<const ::IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return false;
    }
    const auto *headers = reinterpret_cast<const ::IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE)
    {
        return false;
    }

    const auto directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0)
    {
        return false;
    }

    const auto *import = reinterpret_cast<const ::IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
    for (; import->Name != 0; ++import)
    {
        const auto *names = reinterpret_cast<const ::IMAGE_THUNK_DATA32 *>(
            base + (import->OriginalFirstThunk != 0 ? import->OriginalFirstThunk : import->FirstThunk));
        auto *addresses = reinterpret_cast<::IMAGE_THUNK_DATA32 *>(base + import->FirstThunk);

        for (; names->u1.AddressOfData != 0; ++names, ++addresses)
        {
            if ((names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) != 0)
            {
                continue;
            }
            const auto *by_name = reinterpret_cast<const ::IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(by_name->Name), name) != 0)
            {
                continue;
            }

            auto protection = ::DWORD{};
            if (::VirtualProtect(addresses, sizeof(*addresses), PAGE_READWRITE, &protection) == 0)
            {
                return false;
            }
            *previous = reinterpret_cast<void *>(addresses->u1.Function);
            addresses->u1.Function = reinterpret_cast<::DWORD>(replacement);
            ::VirtualProtect(addresses, sizeof(*addresses), protection, &protection);
            return true;
        }
    }
    return false;
}

// Five-byte relative jump with a trampoline, the same shape alloc_watch already
// uses successfully against NtAllocateVirtualMemory in the live game.
inline auto install_inline_hook(void *target_address, void *detour, void **trampoline_out) -> bool
{
    auto *target = static_cast<std::uint8_t *>(target_address);
    if (target == nullptr)
    {
        return false;
    }

    static constexpr auto patch_len = std::size_t{5};
    auto *trampoline =
        static_cast<std::uint8_t *>(::VirtualAlloc(nullptr, 64u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr)
    {
        return false;
    }

    std::memcpy(trampoline, target, patch_len);
    const auto back = static_cast<std::int32_t>((target + patch_len) - (trampoline + patch_len + 5));
    trampoline[patch_len] = 0xE9;
    std::memcpy(trampoline + patch_len + 1, &back, sizeof(back));
    *trampoline_out = trampoline;

    auto protection = ::DWORD{};
    if (::VirtualProtect(target, patch_len, PAGE_EXECUTE_READWRITE, &protection) == 0)
    {
        return false;
    }
    const auto jump = static_cast<std::int32_t>(static_cast<std::uint8_t *>(detour) - (target + patch_len));
    target[0] = 0xE9;
    std::memcpy(target + 1, &jump, sizeof(jump));
    ::VirtualProtect(target, patch_len, protection, &protection);
    ::FlushInstructionCache(::GetCurrentProcess(), target, patch_len);
    return true;
}

}

struct HeapInterposerResult
{
    bool installed;
    std::uint64_t reserved_bytes;
    const char *reason;
};

// Order matters and is not arbitrary. The free hook goes in first, so there can
// be no window in which an arena allocation exists but a free of it would reach
// the real RtlFreeHeap -- which would be handed a pointer its heap has never
// seen. If the free hook fails, nothing is diverted at all.
inline auto install_heap_interposer(std::uint64_t reserve_bytes, std::size_t threshold_bytes) -> HeapInterposerResult
{
    auto result = HeapInterposerResult{.installed = false, .reserved_bytes = 0u, .reason = "not attempted"};

    if (reserve_bytes == 0u)
    {
        result.reason = "disabled";
        return result;
    }

    auto *region = static_cast<std::byte *>(
        ::VirtualAlloc(nullptr, static_cast<::SIZE_T>(reserve_bytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (region == nullptr)
    {
        result.reason = "reservation failed";
        return result;
    }

    {
        auto lock = std::scoped_lock{detail::arena_mutex};
        static auto arena = Arena{region, static_cast<std::size_t>(reserve_bytes)};
        detail::arena_instance = &arena;
    }
    detail::interpose_threshold.store(threshold_bytes, std::memory_order_relaxed);
    detail::process_heap.store(::GetProcessHeap(), std::memory_order_relaxed);

    auto *ntdll = ::GetModuleHandleA("ntdll.dll");
    auto *rtl_free = ntdll != nullptr ? ::GetProcAddress(ntdll, "RtlFreeHeap") : nullptr;
    if (rtl_free == nullptr)
    {
        result.reason = "RtlFreeHeap not found";
        return result;
    }

    auto *trampoline = static_cast<void *>(nullptr);
    if (!detail::install_inline_hook(
            reinterpret_cast<void *>(rtl_free), reinterpret_cast<void *>(&detail::rtl_free_heap_detour), &trampoline))
    {
        result.reason = "RtlFreeHeap hook failed";
        return result;
    }
    detail::rtl_free_heap_original.store(
        reinterpret_cast<detail::RtlFreeHeapFn>(trampoline), std::memory_order_relaxed);

    auto *d3d9 = ::GetModuleHandleA("d3d9.dll");
    if (d3d9 == nullptr)
    {
        result.reason = "system d3d9 not loaded yet";
        return result;
    }

    auto *previous = static_cast<void *>(nullptr);
    if (!detail::redirect_import(d3d9, "HeapAlloc", reinterpret_cast<void *>(&detail::heap_alloc_detour), &previous))
    {
        result.reason = "d3d9 HeapAlloc import not found";
        return result;
    }
    detail::heap_alloc_original.store(reinterpret_cast<detail::HeapAllocFn>(previous), std::memory_order_relaxed);

    result.installed = true;
    result.reserved_bytes = reserve_bytes;
    result.reason = "installed";
    return result;
}

inline auto log_heap_interposer(std::string_view reason) -> void
{
    if (detail::arena_instance == nullptr)
    {
        return;
    }

    auto largest = std::size_t{0};
    auto live = std::size_t{0};
    {
        auto lock = std::scoped_lock{detail::arena_mutex};
        largest = detail::arena_instance->largest_free();
        live = detail::arena_instance->live_allocations();
    }

    log("arena[{}]: served={}/{}kb freed={} live={} largest_free={}kb full_fallbacks={} passed={}",
        reason,
        detail::interposed_allocations.load(std::memory_order_relaxed),
        detail::interposed_bytes.load(std::memory_order_relaxed) / 1024u,
        detail::interposed_frees.load(std::memory_order_relaxed),
        live,
        largest / 1024u,
        detail::arena_full_fallbacks.load(std::memory_order_relaxed),
        detail::passed_through.load(std::memory_order_relaxed));
}

}
