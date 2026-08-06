#pragma once

#include <cstdint>
#include <optional>

// What the immediate at 0x004B8F30 actually buys, separated from the mechanism so
// it can be tested without a Windows toolchain.
//
// Read out of FUN_004b8da0. The engine treats 0x35200000 as a budget covering two
// allocations, not as the size of one pool:
//
//   budget = 0x35200000
//   HeapAlloc a 0x3700000 "Strings" pool, budget -= it
//   two further records carry size 0 and are skipped
//   while (budget > 1 MB && HeapAlloc(budget) == null) budget -= 1 MB
//   register the result as "Main Pool"
//
// So the pool is the budget minus a fixed 55 MB, and the request backs off in 1 MB
// steps until the address space can satisfy it. That last part is why a pool can
// come up short with nothing in the log saying so, and comparing what was asked
// for against what was registered is the only evidence it happened.

namespace lyrium::dao
{

// The first record's size in FUN_004b8da0, taken before the main pool is asked for.
inline constexpr auto strings_pool_bytes = std::uint64_t{0x3700000};

// The immediate as it ships, which pool_patch.h refuses to write over unless it
// finds exactly this value already there.
inline constexpr auto stock_budget_bytes = std::uint64_t{0x35200000};

// The engine gives up rather than going below this, and steps by it on the way down.
inline constexpr auto main_pool_backoff_step_bytes = std::uint64_t{0x100000};

// What the main pool would be if the first request succeeded. Saturates rather than
// wrapping: a budget under 55 MB leaves the engine asking for nothing, which is a
// configuration to report, not an enormous pool.
[[nodiscard]] constexpr auto expected_main_pool_bytes(std::uint64_t budget_bytes) -> std::uint64_t
{
    return budget_bytes <= strings_pool_bytes ? 0u : budget_bytes - strings_pool_bytes;
}

// The smallest main pool worth running. With routing on, the largest single
// request it still has to satisfy is under the threshold, but it must still hold
// the working set -- measured at 306 MB peak with a side pool taking the rest.
inline constexpr auto minimum_main_pool_bytes = std::uint64_t{320} * 1024 * 1024;

struct PoolSplit
{
    std::uint32_t budget_bytes;
    std::uint64_t side_bytes;
    const char *reason;
};

// Decides how the pool budget and the side pool share the address space.
//
// This exists because the two are separate configuration keys and nothing stopped
// them being set to a combination that cannot fit. Leaving the budget at 768 MB
// and adding a 192 MB side pool put 905 MB of pools into a 2 GB address space,
// and the game froze during a level load with every counter reading healthy --
// the exact invisible failure this project exists to remove, caused by us.
//
// On a large-address-aware image there is a second 2 GB nobody has touched, so
// the side pool is free and the budget is left alone. Without it, the side pool
// has to come out of the budget, and if that would starve the main pool the side
// pool is dropped instead: degrading to today's behaviour is always safe.
[[nodiscard]] constexpr auto plan_pool_split(
    std::uint64_t configured_budget_bytes,
    std::uint64_t side_bytes,
    bool large_address_aware) -> PoolSplit
{
    if (side_bytes == 0u)
    {
        return PoolSplit{static_cast<std::uint32_t>(configured_budget_bytes), 0u, "no side pool"};
    }
    if (large_address_aware)
    {
        return PoolSplit{static_cast<std::uint32_t>(configured_budget_bytes), side_bytes, "additive, image is 4 GB"};
    }
    if (configured_budget_bytes < side_bytes ||
        expected_main_pool_bytes(configured_budget_bytes - side_bytes) < minimum_main_pool_bytes)
    {
        return PoolSplit{
            static_cast<std::uint32_t>(configured_budget_bytes), 0u, "side pool dropped, would starve the main pool"};
    }
    return PoolSplit{
        static_cast<std::uint32_t>(configured_budget_bytes - side_bytes), side_bytes, "split out of the budget"};
}

struct MainPoolOutcome
{
    // False when the pool has not been read yet. Everything below is then zero and
    // means nothing -- an unknown pool is not a pool that got nothing.
    bool observed;

    std::uint64_t expected_bytes;
    std::uint64_t actual_bytes;

    // How far the back-off loop had to walk down. Zero when the first request fit.
    std::uint64_t shortfall_bytes;

    // True when the engine settled for less than it asked for, including when it
    // got nothing at all.
    bool backed_off;
};

// actual_bytes is the size the engine recorded on the pool object. Empty when it
// has not been read, which is the ordinary case before the engine has built the
// pool -- reporting the whole request as a shortfall there would be a gauge that
// reads alarming while knowing nothing.
//
// A value larger than expected cannot come from the engine's own loop, but it
// arrives from engine memory, so it must not wrap the subtraction.
[[nodiscard]] constexpr auto evaluate_main_pool(
    std::uint64_t budget_bytes,
    std::optional<std::uint64_t> actual_bytes) -> MainPoolOutcome
{
    const auto expected = expected_main_pool_bytes(budget_bytes);
    if (!actual_bytes.has_value())
    {
        return MainPoolOutcome{
            .observed = false,
            .expected_bytes = expected,
            .actual_bytes = 0u,
            .shortfall_bytes = 0u,
            .backed_off = false,
        };
    }

    const auto actual = *actual_bytes;
    const auto shortfall = actual < expected ? expected - actual : std::uint64_t{0};
    return MainPoolOutcome{
        .observed = true,
        .expected_bytes = expected,
        .actual_bytes = actual,
        .shortfall_bytes = shortfall,
        .backed_off = shortfall != 0u,
    };
}

}
