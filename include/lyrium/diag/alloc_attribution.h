#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace lyrium::diag
{

// Which module made the large allocations, folded one row per module.
//
// The allocation watch has recorded a caller address since the sixteen-frame
// stack walk was removed, and never resolved it to anything. That is why two
// sessions of arena work could not say where the MANAGED texture duplicates come
// from, and why a 512 KB HeapAlloc redirect on d3d9.dll caught 1877 calls and
// served none of them. The answer is one module name, and this is the shape that
// produces it.
//
// Portable and Windows-free by construction: alloc_watch.h includes psapi.h and
// cannot be reached by the test suite, so anything decided in there is untestable
// -- the rule that free_size_classes.h exists to enforce after a duplicated
// threshold array shipped a broken histogram.
//
// Resolving a caller to a module takes the loader lock, so it must never happen
// on the allocation path. The Windows side resolves at report time, from records
// captured with nothing but a return address.

inline constexpr auto max_alloc_modules = std::size_t{8};

// How many return addresses are captured per record. Enough to step over the
// allocator layers and reach the client; more would cost stack-walk time for
// frames nothing reads, which is how the first version of this went wrong.
inline constexpr auto max_alloc_frames = std::size_t{8};

using FrameModules = std::array<std::uint64_t, max_alloc_frames>;

// The modules that are allocators rather than clients: ntdll, kernel32,
// kernelbase, with a spare slot. 0 means the slot is unused and matches nothing.
using AllocatorModules = std::array<std::uint64_t, 4>;

// The module that asked for the allocation, as opposed to the one that made it.
//
// A live session resolved all 256 records to ntdll.dll and KERNEL32.DLL, which
// answers nothing. RtlAllocateHeap and VirtualAlloc are what call
// NtAllocateVirtualMemory, so there is always at least one allocator frame
// between the client and the syscall, and the immediate return address can never
// name the client at that hook site.
//
// lyrium is deliberately absent from the allocator list. If our own staging
// sections turn out to be what cuts the space, that is a finding, not noise.
//
// Returns 0 when every frame is an allocator or unresolvable, which reads as
// "the walk did not reach the client" rather than as an answer.
[[nodiscard]] constexpr auto requesting_module(
    const FrameModules &frames,
    std::uint32_t frame_count,
    const AllocatorModules &allocators) -> std::uint64_t
{
    const auto usable = frame_count < frames.size() ? frame_count : static_cast<std::uint32_t>(frames.size());

    for (auto i = std::uint32_t{0}; i < usable; ++i)
    {
        const auto module = frames[i];
        if (module == 0u)
        {
            continue;
        }

        auto is_allocator = false;
        for (const auto allocator : allocators)
        {
            if (allocator != 0u && allocator == module)
            {
                is_allocator = true;
                break;
            }
        }

        if (!is_allocator)
        {
            return module;
        }
    }

    return 0u;
}

struct ModuleAllocations
{
    // 0 means the caller resolved to no module. Kept as a row rather than
    // dropped: a large unattributed count is itself a finding.
    std::uint64_t module_base;
    std::uint32_t count;
    std::uint64_t bytes;
    std::uint64_t largest;
};

using AllocAttribution = std::array<ModuleAllocations, max_alloc_modules>;

[[nodiscard]] constexpr auto attributed_modules(const AllocAttribution &table) -> std::size_t
{
    auto used = std::size_t{0};
    while (used < table.size() && table[used].count != 0u)
    {
        ++used;
    }
    return used;
}

// Folds one allocation into the table.
//
// Returns false when the allocation carried nothing to record, or when the
// module is new and there is no room for it. A full table keeps merging into
// modules it already knows, so the totals for the interesting ones do not
// freeze exactly when the session gets interesting.
constexpr auto attribute(AllocAttribution &table, std::uint64_t module_base, std::uint64_t bytes) -> bool
{
    if (bytes == 0u)
    {
        return false;
    }

    for (auto &row : table)
    {
        if (row.count != 0u && row.module_base != module_base)
        {
            continue;
        }

        if (row.count == 0u)
        {
            row.module_base = module_base;
        }

        ++row.count;
        row.bytes += bytes;
        if (bytes > row.largest)
        {
            row.largest = bytes;
        }
        return true;
    }

    return false;
}

}
