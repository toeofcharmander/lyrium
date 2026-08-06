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
