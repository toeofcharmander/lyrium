#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace lyrium::diag
{

// Classification of one address-space region, lifted out of the VirtualQuery
// walk so it can be tested.
//
// The clamp below the 2 GB line is the single most load-bearing number in this
// project: on a large-address-aware process the overall largest free block sits
// above that line and never moves, so it is useless as a pressure signal, while
// the block below it is what actually shrinks as the heap fragments. The rescue
// policy reads the smaller of the two. Getting this wrong silently disables
// rescue, which is exactly the failure a live session already caught once.

inline constexpr auto two_gigabytes = std::uint64_t{0x80000000ull};

// Mirrors of the Windows MEM_* values. Kept here so this header needs no
// windows.h; the DLL build static_asserts them against the real ones.
enum class RegionState : std::uint32_t
{
    committed = 0x1000,
    reserved = 0x2000,
    free = 0x10000,
};

enum class RegionType : std::uint32_t
{
    private_memory = 0x20000,
    mapped = 0x40000,
    image = 0x1000000,
};

struct VaRegion
{
    std::uint64_t base{};
    std::uint64_t size{};
    RegionState state{};
    RegionType type{};
    std::uint32_t protect{};
    std::uint64_t allocation_base{};
};

// Cumulative rather than disjoint: a region counts in every bucket whose
// threshold it clears. Pinned by test because "at least this big" reads very
// differently from "between these sizes".
inline constexpr auto free_bucket_thresholds = std::array<std::uint64_t, 9u>{
    1ull << 30,
    512ull << 20,
    256ull << 20,
    128ull << 20,
    64ull << 20,
    32ull << 20,
    16ull << 20,
    4ull << 20,
    1ull << 20,
};

inline constexpr auto committed_bucket_thresholds = std::array<std::uint64_t, 5u>{
    256ull << 20,
    64ull << 20,
    16ull << 20,
    4ull << 20,
    1ull << 20,
};

inline constexpr auto big_block_threshold = std::uint64_t{16ull << 20};

// The largest free block below the 2 GB line, given one region. Returns 0 when
// the region starts at or above the line, since none of it counts.
[[nodiscard]] constexpr auto usable_below_2g(std::uint64_t base, std::uint64_t size) -> std::uint64_t
{
    if (base >= two_gigabytes)
    {
        return 0u;
    }
    return std::min<std::uint64_t>(size, two_gigabytes - base);
}

// True when the walk must stop. A region that does not advance the cursor would
// otherwise loop forever, which is the one way a diagnostic can hang the game.
[[nodiscard]] constexpr auto walk_stalled(std::uint64_t cursor, std::uint64_t base, std::uint64_t size) -> bool
{
    return base + size <= cursor;
}

}
