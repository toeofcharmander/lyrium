#pragma once

#include <array>
#include <cstdint>

#include "lyrium/diag/va_region.h"

namespace lyrium::diag
{

// Free-block size distribution: how the remaining address space is divided up.
//
// This is the only honest way to show fragmentation. The failure this project
// exists to prevent is not memory running out, it is what remains being in
// pieces too small to use, and no single number conveys that -- the same free
// bytes migrating from a few large blocks into hundreds of small ones is
// precisely the crash approaching.
//
// The accumulation and the differencing live here rather than beside the
// VirtualQuery walk because a Windows-only header cannot be tested, and the
// first version of this shipped with the walk filling the buckets in ascending
// order while the overlay differenced them in descending order. Every class came
// out empty and the whole population collapsed into the final one, which drew as
// a shattered address space when 289 MB of headroom said otherwise. Both sides
// now read the same thresholds through the same functions.

using FreeBuckets = std::array<std::uint32_t, free_bucket_thresholds.size()>;

// One count per threshold, plus a final class for everything below the smallest
// one. Unlike the buckets these are disjoint, so they sum to the block count.
using FreeSizeClasses = std::array<std::uint32_t, free_bucket_thresholds.size() + 1u>;

// Blocks smaller than this cannot hold a texture worth relocating, so their
// count is the fragmentation signal rather than the total free bytes.
inline constexpr auto sliver_threshold = free_bucket_thresholds[free_bucket_thresholds.size() - 2u];

// Counts one free block into every bucket whose threshold it clears.
constexpr auto accumulate_free_buckets(FreeBuckets &buckets, std::uint64_t size) -> void
{
    for (auto i = std::size_t{0}; i < free_bucket_thresholds.size(); ++i)
    {
        if (size >= free_bucket_thresholds[i])
        {
            ++buckets[i];
        }
    }
}

// Differences the cumulative buckets into disjoint per-class counts, largest
// class first. The final class holds the blocks below the smallest threshold,
// which nothing counted and which have to be recovered from the total.
[[nodiscard]] constexpr auto free_size_classes(const FreeBuckets &buckets, std::uint32_t free_regions)
    -> FreeSizeClasses
{
    auto classes = FreeSizeClasses{};
    for (auto i = std::size_t{0}; i < buckets.size(); ++i)
    {
        const auto larger = i == 0u ? std::uint32_t{0} : buckets[i - 1u];
        classes[i] = buckets[i] >= larger ? buckets[i] - larger : 0u;
    }

    const auto counted = buckets.back();
    classes.back() = free_regions > counted ? free_regions - counted : 0u;
    return classes;
}

// The blocks under the sliver threshold: the two smallest classes together.
[[nodiscard]] constexpr auto sliver_count(const FreeSizeClasses &classes) -> std::uint32_t
{
    return classes[classes.size() - 2u] + classes.back();
}

}
