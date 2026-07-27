#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <psapi.h>
#include <windows.h>

#include "eluvian/log.h"

namespace eluvian::diag
{

struct VaStats
{
    std::uint64_t largest_free;
    std::uint64_t largest_free_below_2g;
    std::uint64_t total_free;
    std::uint64_t total_reserved;
    std::uint64_t committed_private;
    std::uint64_t committed_image;
    std::uint64_t committed_mapped;
    std::uint32_t free_regions;
    std::uint32_t total_regions;

    std::array<std::uint32_t, 9u> buckets;

    std::uint64_t working_set;
    std::uint64_t private_usage;
    std::uint64_t pagefile_usage;
    std::uint64_t available_virtual;
    std::uint64_t total_virtual;

    struct Region
    {
        std::uint64_t base;
        std::uint64_t size;
        std::uint32_t type;
        std::uint32_t protect;

        std::uint64_t allocation_base;
    };
    static constexpr auto tracked_regions = std::size_t{12};
    std::array<Region, tracked_regions> largest_committed;
    std::uint32_t committed_regions;

    std::array<std::uint32_t, 5u> committed_buckets;
    std::uint64_t committed_in_big_blocks;

    std::int64_t walk_us;
};

inline constexpr std::array<std::uint64_t, 5u> committed_thresholds = {
    1ull << 20, 4ull << 20, 16ull << 20, 64ull << 20, 256ull << 20};

inline constexpr std::array<std::uint64_t, 9u> bucket_thresholds = {
    1ull << 20, 2ull << 20, 4ull << 20, 8ull << 20, 16ull << 20,
    32ull << 20, 64ull << 20, 128ull << 20, 256ull << 20};

inline auto sample_va() -> VaStats
{
    const auto started = now_us();

    auto stats = VaStats{};
    stats.buckets.fill(0u);
    stats.committed_buckets.fill(0u);
    stats.largest_committed.fill(VaStats::Region{});

    auto system_info = ::SYSTEM_INFO{};
    ::GetSystemInfo(&system_info);

    const auto *cursor = static_cast<const std::byte *>(system_info.lpMinimumApplicationAddress);
    const auto *ceiling = static_cast<const std::byte *>(system_info.lpMaximumApplicationAddress);
    static constexpr auto two_gigabytes = std::uintptr_t{0x80000000u};

    auto info = ::MEMORY_BASIC_INFORMATION{};
    while (cursor < ceiling && ::VirtualQuery(cursor, &info, sizeof(info)) != 0)
    {
        const auto size = static_cast<std::uint64_t>(info.RegionSize);
        ++stats.total_regions;

        if (info.State == MEM_FREE)
        {
            ++stats.free_regions;
            stats.total_free += size;

            if (size > stats.largest_free)
            {
                stats.largest_free = size;
            }

            const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
            if (base < two_gigabytes)
            {
                const auto usable = std::min<std::uint64_t>(size, two_gigabytes - base);
                if (usable > stats.largest_free_below_2g)
                {
                    stats.largest_free_below_2g = usable;
                }
            }

            for (auto i = std::size_t{0}; i < bucket_thresholds.size(); ++i)
            {
                if (size >= bucket_thresholds[i])
                {
                    ++stats.buckets[i];
                }
            }
        }
        else if (info.State == MEM_RESERVE)
        {
            stats.total_reserved += size;
        }
        else if (info.State == MEM_COMMIT)
        {
            switch (info.Type)
            {
                case MEM_IMAGE: stats.committed_image += size; break;
                case MEM_MAPPED: stats.committed_mapped += size; break;
                default: stats.committed_private += size; break;
            }

            ++stats.committed_regions;
            for (auto i = std::size_t{0}; i < committed_thresholds.size(); ++i)
            {
                if (size >= committed_thresholds[i])
                {
                    ++stats.committed_buckets[i];
                }
            }
            if (size >= (16ull << 20))
            {
                stats.committed_in_big_blocks += size;
            }

            auto candidate = VaStats::Region{
                .base = reinterpret_cast<std::uintptr_t>(info.BaseAddress),
                .size = size,
                .type = info.Type,
                .protect = info.Protect,
                .allocation_base = reinterpret_cast<std::uintptr_t>(info.AllocationBase)};
            for (auto &slot : stats.largest_committed)
            {
                if (candidate.size > slot.size)
                {
                    std::swap(slot, candidate);
                }
            }
        }

        const auto *next = static_cast<const std::byte *>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor)
        {
            break;
        }
        cursor = next;
    }

    auto counters = ::PROCESS_MEMORY_COUNTERS_EX{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(
            ::GetCurrentProcess(), reinterpret_cast<::PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)) != 0)
    {
        stats.working_set = counters.WorkingSetSize;
        stats.private_usage = counters.PrivateUsage;
        stats.pagefile_usage = counters.PagefileUsage;
    }

    auto memory_status = ::MEMORYSTATUSEX{};
    memory_status.dwLength = sizeof(memory_status);
    if (::GlobalMemoryStatusEx(&memory_status) != 0)
    {
        stats.available_virtual = memory_status.ullAvailVirtual;
        stats.total_virtual = memory_status.ullTotalVirtual;
    }

    stats.walk_us = now_us() - started;
    return stats;
}

}
