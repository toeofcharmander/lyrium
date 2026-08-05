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
