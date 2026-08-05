#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string_view>

#include <windows.h>

#include "lyrium/allocators/arena.h"
#include "lyrium/allocators/local_alloc_policy.h"
#include "lyrium/diag/import_probe.h"
#include "lyrium/diag/size_tally.h"
#include "lyrium/log.h"
#include "lyrium/utils.h"

namespace lyrium
{

// Serves d3d9.dll's large LocalAlloc requests from one contiguous reservation.
//
// This is the heap arena rebuilt against a measured call instead of a guessed
// one. The first attempt redirected HeapAlloc, took 1877 calls with not one of
// them 512 KB or larger, and served nothing across two sessions before being
// deleted. Counting shims then found the traffic: LocalAlloc, 180 calls of
// 512 KB or more carrying 280 MB with a largest of 16 MB, cross-checked against
// 184 allocations totalling 324 MB recorded inside IDirect3DDevice9::
// CreateTexture by an independent probe. Those are the MANAGED texture
// duplicates.
//
// The blast radius is two IAT slots in one module. The deleted version inline
// hooked RtlFreeHeap, RtlSizeHeap and RtlReAllocateHeap process-wide -- every
// allocation the engine, PhysX and CUDA made passed through it -- because it did
// not know which call it wanted. This touches nothing outside d3d9.dll's own
// import table, and nothing else in the process can reach it.
//
// The whole API path is owned, which is the condition that makes interposing
// safe. d3d9.dll's entire Local surface is LocalAlloc and LocalFree: no
// LocalSize, no LocalReAlloc, and no LocalLock. The absence of LocalLock is what
// establishes the memory is LMEM_FIXED, so the HLOCAL returned is a pointer and
// the arena's range test is a valid ownership answer.
//
// Ownership is decided by two comparisons against a range that never changes
// after install, so the fast path takes no lock. The arena mutex is taken only
// once a pointer is known to be ours, which is rare against the 104,598 calls a
// session makes.

namespace detail
{

using LocalAllocFn = ::HLOCAL(WINAPI *)(::UINT, ::SIZE_T);
using LocalFreeFn = ::HLOCAL(WINAPI *)(::HLOCAL);

inline std::atomic<LocalAllocFn> real_local_alloc{nullptr};
inline std::atomic<LocalFreeFn> real_local_free{nullptr};

inline std::mutex local_arena_mutex{};
inline Arena *local_arena{nullptr};

// The reservation's bounds, published once at install and never written again,
// so the ownership test on the free path needs no lock at all. This is the
// lesson from the deleted interposer, which took a mutex on every free in the
// process to answer a question two loads can answer.
inline std::atomic<std::uintptr_t> arena_low{0};
inline std::atomic<std::uintptr_t> arena_high{0};

inline std::atomic<std::uint64_t> local_arena_threshold{512u * 1024u};

inline diag::SizeTally arena_served{};
inline diag::SizeTally arena_passed{};
inline std::atomic<std::uint64_t> arena_full_fallbacks{};
inline std::atomic<std::uint64_t> arena_frees{};
inline std::atomic<std::uint64_t> arena_foreign_frees{};

[[nodiscard]] inline auto arena_owns(const void *pointer) -> bool
{
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return address >= arena_low.load(std::memory_order_relaxed) && address < arena_high.load(std::memory_order_relaxed);
}

inline auto WINAPI local_alloc_detour(::UINT flags, ::SIZE_T bytes) -> ::HLOCAL
{
    const auto original = real_local_alloc.load(std::memory_order_relaxed);
    if (original == nullptr)
    {
        return nullptr;
    }

    if (!allocators::serve_from_arena(flags, bytes, local_arena_threshold.load(std::memory_order_relaxed)))
    {
        arena_passed.note(bytes);
        return original(flags, bytes);
    }

    auto *served = static_cast<void *>(nullptr);
    {
        const auto lock = std::scoped_lock{local_arena_mutex};
        if (local_arena != nullptr)
        {
            served = local_arena->allocate(bytes);
        }
    }

    if (served == nullptr)
    {
        // A full arena degrades to exactly the behaviour that existed before it,
        // rather than failing an allocation the runtime cannot recover from.
        arena_full_fallbacks.fetch_add(1u, std::memory_order_relaxed);
        return original(flags, bytes);
    }

    // Arena memory is recycled and therefore dirty. LPTR asks for zeroes and the
    // runtime will rely on getting them; handing back the previous texture's
    // bytes instead surfaces as wrong pixels rather than as a crash.
    if (allocators::needs_zeroing(flags))
    {
        std::memset(served, 0, bytes);
    }

    arena_served.note(bytes);
    return static_cast<::HLOCAL>(served);
}

// The other two ways a block this arena served can legally come back.
//
// LocalAlloc(LMEM_FIXED) returns a process-heap pointer identical to what
// HeapAlloc(GetProcessHeap(), ...) returns, and the two are interchangeable on
// Win32. d3d9.dll imports GetProcessHeap, HeapFree and msvcrt's free alongside
// LocalAlloc and LocalFree, so covering only LocalFree covers one of three
// doors. The first build did exactly that: it served 40 allocations, saw 5 come
// back, and the game died with an access violation in nvd3dum.dll -- which is
// what a heap does when handed a pointer it never allocated, surfacing in
// whichever module touches it next rather than in the one that mixed them.
//
// Each counter is reported separately because which door the pointers arrive at
// is the finding. If none of them ever sees one, d3d9 is not freeing these
// itself and the pointer is leaving the module entirely, which is a different
// problem with a much worse answer.
using HeapFreeFn = ::BOOL(WINAPI *)(::HANDLE, ::DWORD, ::LPVOID);
using CrtFreeFn = void(__attribute__((cdecl)) *)(void *);

inline std::atomic<HeapFreeFn> real_heap_free{nullptr};
inline std::atomic<CrtFreeFn> real_crt_free{nullptr};

inline std::atomic<std::uint64_t> frees_via_local{};
inline std::atomic<std::uint64_t> frees_via_heap{};
inline std::atomic<std::uint64_t> frees_via_crt{};

// Returns true when the pointer was ours and has been released.
[[nodiscard]] inline auto release_if_ours(void *memory) -> bool
{
    if (!arena_owns(memory))
    {
        return false;
    }

    const auto lock = std::scoped_lock{local_arena_mutex};
    return local_arena != nullptr && local_arena->deallocate(memory);
}

inline auto WINAPI heap_free_detour(::HANDLE heap, ::DWORD flags, ::LPVOID memory) -> ::BOOL
{
    const auto original = real_heap_free.load(std::memory_order_relaxed);
    if (original == nullptr)
    {
        return FALSE;
    }

    if (release_if_ours(memory))
    {
        frees_via_heap.fetch_add(1u, std::memory_order_relaxed);
        arena_frees.fetch_add(1u, std::memory_order_relaxed);
        return TRUE;
    }

    return original(heap, flags, memory);
}

inline auto __attribute__((cdecl)) crt_free_detour(void *memory) -> void
{
    const auto original = real_crt_free.load(std::memory_order_relaxed);
    if (original == nullptr)
    {
        return;
    }

    if (release_if_ours(memory))
    {
        frees_via_crt.fetch_add(1u, std::memory_order_relaxed);
        arena_frees.fetch_add(1u, std::memory_order_relaxed);
        return;
    }

    original(memory);
}

inline auto WINAPI local_free_detour(::HLOCAL memory) -> ::HLOCAL
{
    const auto original = real_local_free.load(std::memory_order_relaxed);
    if (original == nullptr)
    {
        return memory;
    }

    // Two loads and two comparisons, no lock. Anything not ours is passed
    // through untouched, which is the property that makes interposing on another
    // module's frees safe: a pointer is inside our reservation or it is not, and
    // nothing else can live in a range we reserved.
    if (!arena_owns(memory))
    {
        arena_foreign_frees.fetch_add(1u, std::memory_order_relaxed);
        return original(memory);
    }

    if (release_if_ours(memory))
    {
        frees_via_local.fetch_add(1u, std::memory_order_relaxed);
        arena_frees.fetch_add(1u, std::memory_order_relaxed);
        return nullptr;
    }

    // In our range but not something we handed out -- an interior pointer, or a
    // double free. Refused rather than guessed at: taking the header from an
    // interior pointer reads payload bytes as metadata and then coalesces.
    arena_foreign_frees.fetch_add(1u, std::memory_order_relaxed);
    return original(memory);
}

}

struct LocalArenaResult
{
    bool installed;
    std::uint64_t reserved_bytes;
    const char *reason;
};

// Reserves the region and redirects both imports, or does neither.
//
// Everything is resolved and verified before a byte is written. An earlier
// version of this installed its free hook first and left it live process-wide
// when a later step failed, which is the worst failure this kind of code has.
inline auto install_local_arena(::HMODULE system_d3d9, std::uint64_t reserve_bytes, std::uint64_t threshold_bytes)
    -> LocalArenaResult
{
    if (system_d3d9 == nullptr || reserve_bytes == 0u)
    {
        return LocalArenaResult{.installed = false, .reserved_bytes = 0u, .reason = "disabled"};
    }

    // Every slot resolved before any is written. A served pointer that can come
    // back through a door we did not cover is heap corruption, so installing
    // partially is worse than not installing at all.
    auto **alloc_slot = diag::detail::find_import_slot(system_d3d9, "LocalAlloc");
    auto **local_free_slot = diag::detail::find_import_slot(system_d3d9, "LocalFree");
    auto **heap_free_slot = diag::detail::find_import_slot(system_d3d9, "HeapFree");
    auto **crt_free_slot = diag::detail::find_import_slot(system_d3d9, "free");
    if (alloc_slot == nullptr || local_free_slot == nullptr || heap_free_slot == nullptr || crt_free_slot == nullptr)
    {
        return LocalArenaResult{.installed = false, .reserved_bytes = 0u, .reason = "imports not found"};
    }

    // Reserved and then committed whole. The arena has no commit-on-demand path
    // and writes its first block header at base immediately, so a partial commit
    // would only move the decision somewhere harder to get right.
    //
    // Committing costs pagefile and working set, not address space -- and the
    // reservation costs the same address space either way. Address space is the
    // resource this project is short of; pages that are never touched are never
    // resident.
    auto *base = static_cast<std::byte *>(
        ::VirtualAlloc(nullptr, static_cast<::SIZE_T>(reserve_bytes), MEM_RESERVE, PAGE_READWRITE));
    if (base == nullptr)
    {
        return LocalArenaResult{.installed = false, .reserved_bytes = 0u, .reason = "reservation failed"};
    }

    if (::VirtualAlloc(base, static_cast<::SIZE_T>(reserve_bytes), MEM_COMMIT, PAGE_READWRITE) == nullptr)
    {
        ::VirtualFree(base, 0, MEM_RELEASE);
        return LocalArenaResult{.installed = false, .reserved_bytes = 0u, .reason = "commit failed"};
    }

    // Placed inside the reservation it manages, so the arena itself costs the
    // address space nothing extra and is never destroyed. See never_destroyed.h
    // for why nothing here has a destructor to run at exit.
    alignas(Arena) static std::byte storage[sizeof(Arena)]{};
    detail::local_arena = ::new (static_cast<void *>(storage)) Arena{base, static_cast<std::size_t>(reserve_bytes)};

    detail::local_arena_threshold.store(threshold_bytes, std::memory_order_relaxed);
    detail::arena_low.store(reinterpret_cast<std::uintptr_t>(base), std::memory_order_relaxed);
    detail::arena_high.store(reinterpret_cast<std::uintptr_t>(base) + reserve_bytes, std::memory_order_relaxed);

    const auto abandon = [&](const char *reason)
    {
        ::VirtualFree(base, 0, MEM_RELEASE);
        detail::local_arena = nullptr;
        detail::arena_low.store(0u, std::memory_order_relaxed);
        detail::arena_high.store(0u, std::memory_order_relaxed);
        return LocalArenaResult{.installed = false, .reserved_bytes = 0u, .reason = reason};
    };

    // Every freeing path goes in before the allocation does. The other order
    // leaves a window in which the arena has handed out a pointer that one of
    // these would pass to the real heap, which never allocated it.
    auto *previous_local_free =
        diag::detail::write_import_slot(local_free_slot, reinterpret_cast<void *>(&detail::local_free_detour));
    if (previous_local_free == nullptr)
    {
        return abandon("LocalFree redirect failed");
    }
    detail::real_local_free.store(
        reinterpret_cast<detail::LocalFreeFn>(previous_local_free), std::memory_order_relaxed);

    auto *previous_heap_free =
        diag::detail::write_import_slot(heap_free_slot, reinterpret_cast<void *>(&detail::heap_free_detour));
    if (previous_heap_free == nullptr)
    {
        static_cast<void>(diag::detail::write_import_slot(local_free_slot, previous_local_free));
        return abandon("HeapFree redirect failed");
    }
    detail::real_heap_free.store(reinterpret_cast<detail::HeapFreeFn>(previous_heap_free), std::memory_order_relaxed);

    auto *previous_crt_free =
        diag::detail::write_import_slot(crt_free_slot, reinterpret_cast<void *>(&detail::crt_free_detour));
    if (previous_crt_free == nullptr)
    {
        static_cast<void>(diag::detail::write_import_slot(heap_free_slot, previous_heap_free));
        static_cast<void>(diag::detail::write_import_slot(local_free_slot, previous_local_free));
        return abandon("free redirect failed");
    }
    detail::real_crt_free.store(reinterpret_cast<detail::CrtFreeFn>(previous_crt_free), std::memory_order_relaxed);

    auto *previous_alloc =
        diag::detail::write_import_slot(alloc_slot, reinterpret_cast<void *>(&detail::local_alloc_detour));
    if (previous_alloc == nullptr)
    {
        // Put every freeing slot back rather than leaving detours live with
        // nothing feeding them.
        static_cast<void>(diag::detail::write_import_slot(crt_free_slot, previous_crt_free));
        static_cast<void>(diag::detail::write_import_slot(heap_free_slot, previous_heap_free));
        static_cast<void>(diag::detail::write_import_slot(local_free_slot, previous_local_free));
        return abandon("LocalAlloc redirect failed");
    }
    detail::real_local_alloc.store(reinterpret_cast<detail::LocalAllocFn>(previous_alloc), std::memory_order_relaxed);

    return LocalArenaResult{.installed = true, .reserved_bytes = reserve_bytes, .reason = "installed"};
}

// served is the question. passed proves the detour is being reached at all, and
// the two read identically in a total -- which is what the deleted arena could
// not distinguish for two sessions. largest_free is the healing: it must stay
// high while served climbs, or the arena is fragmenting internally exactly the
// way the address space it replaces does.
inline auto log_local_arena(std::string_view reason) -> void
{
    auto largest = std::size_t{0};
    auto live = std::size_t{0};
    {
        const auto lock = std::scoped_lock{detail::local_arena_mutex};
        if (detail::local_arena != nullptr)
        {
            largest = detail::local_arena->largest_free();
            live = detail::local_arena->live_allocations();
        }
    }

    lyrium::log(
        "local_arena[{}]: served={}/{}kb largest_served_kb={} live={} freed={} via_local={} via_heap={} "
        "via_crt={} largest_free_kb={} full_fallbacks={} passed={} foreign_frees={}",
        reason,
        detail::arena_served.count(),
        detail::arena_served.bytes() / 1024u,
        detail::arena_served.largest() / 1024u,
        live,
        detail::arena_frees.load(std::memory_order_relaxed),
        detail::frees_via_local.load(std::memory_order_relaxed),
        detail::frees_via_heap.load(std::memory_order_relaxed),
        detail::frees_via_crt.load(std::memory_order_relaxed),
        largest / 1024u,
        detail::arena_full_fallbacks.load(std::memory_order_relaxed),
        detail::arena_passed.count(),
        detail::arena_foreign_frees.load(std::memory_order_relaxed));
}

}
