#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/diag/alloc_attribution.h"

using lyrium::diag::AllocAttribution;
using lyrium::diag::attribute;
using lyrium::diag::attributed_modules;
using lyrium::diag::max_alloc_modules;

namespace
{

constexpr auto kb = std::uint64_t{1024};
constexpr auto mb = kb * 1024;

// Stand-ins for the module bases GetModuleHandleEx resolves a caller to.
constexpr auto game_exe = std::uint64_t{0x00400000};
constexpr auto system_d3d9 = std::uint64_t{0x6d7f0000};
constexpr auto driver_umd = std::uint64_t{0x71000000};

}

TEST(AllocAttribution, StartsEmpty)
{
    auto table = AllocAttribution{};

    EXPECT_EQ(attributed_modules(table), 0u);
}

TEST(AllocAttribution, RecordsTheFirstAllocationAgainstItsModule)
{
    auto table = AllocAttribution{};

    ASSERT_TRUE(attribute(table, system_d3d9, 1400 * kb));

    ASSERT_EQ(attributed_modules(table), 1u);
    EXPECT_EQ(table[0].module_base, system_d3d9);
    EXPECT_EQ(table[0].count, 1u);
    EXPECT_EQ(table[0].bytes, 1400 * kb);
    EXPECT_EQ(table[0].largest, 1400 * kb);
}

// The whole point of the table: many allocations, few modules. Answering "who"
// means one row per module, not one row per allocation.
TEST(AllocAttribution, MergesRepeatAllocationsFromTheSameModule)
{
    auto table = AllocAttribution{};

    ASSERT_TRUE(attribute(table, system_d3d9, 1400 * kb));
    ASSERT_TRUE(attribute(table, system_d3d9, 600 * kb));

    ASSERT_EQ(attributed_modules(table), 1u);
    EXPECT_EQ(table[0].count, 2u);
    EXPECT_EQ(table[0].bytes, 2000 * kb);
}

TEST(AllocAttribution, KeepsDifferentModulesApart)
{
    auto table = AllocAttribution{};

    ASSERT_TRUE(attribute(table, game_exe, 8 * mb));
    ASSERT_TRUE(attribute(table, system_d3d9, 1400 * kb));
    ASSERT_TRUE(attribute(table, driver_umd, 2 * mb));

    EXPECT_EQ(attributed_modules(table), 3u);
    EXPECT_EQ(table[0].module_base, game_exe);
    EXPECT_EQ(table[1].module_base, system_d3d9);
    EXPECT_EQ(table[2].module_base, driver_umd);
}

// The largest single request from a module is what decides whether it is the one
// shredding the space: a hundred 4 KB allocations and one 2.8 MB one are very
// different problems and sum to nearly the same bytes.
TEST(AllocAttribution, TracksTheLargestSingleRequestPerModule)
{
    auto table = AllocAttribution{};

    ASSERT_TRUE(attribute(table, system_d3d9, 600 * kb));
    ASSERT_TRUE(attribute(table, system_d3d9, 2800 * kb));
    ASSERT_TRUE(attribute(table, system_d3d9, 700 * kb));

    EXPECT_EQ(table[0].largest, 2800 * kb);
}

// An unresolved caller is its own row rather than being dropped. A large
// unattributed count is itself the finding -- it would mean the allocation comes
// from a thunk or from code with no module, and that is worth seeing rather than
// silently losing.
TEST(AllocAttribution, GivesUnresolvedCallersTheirOwnRow)
{
    auto table = AllocAttribution{};

    ASSERT_TRUE(attribute(table, 0u, 1400 * kb));

    ASSERT_EQ(attributed_modules(table), 1u);
    EXPECT_EQ(table[0].module_base, 0u);
    EXPECT_EQ(table[0].count, 1u);
}

TEST(AllocAttribution, RefusesRatherThanOverwritingWhenFull)
{
    auto table = AllocAttribution{};

    for (auto module = std::uint64_t{0}; module < max_alloc_modules; ++module)
    {
        ASSERT_TRUE(attribute(table, 0x1000u + module * 0x1000u, 1 * mb)) << "module " << module;
    }

    EXPECT_FALSE(attribute(table, 0xdead0000u, 1 * mb));
    EXPECT_EQ(attributed_modules(table), max_alloc_modules);
}

// A full table must still accumulate against modules it already knows, or the
// byte totals for the interesting modules stop moving exactly when the session
// gets interesting.
TEST(AllocAttribution, StillMergesIntoKnownModulesWhenFull)
{
    auto table = AllocAttribution{};

    for (auto module = std::uint64_t{0}; module < max_alloc_modules; ++module)
    {
        ASSERT_TRUE(attribute(table, 0x1000u + module * 0x1000u, 1 * mb));
    }

    EXPECT_TRUE(attribute(table, 0x1000u, 3 * mb));
    EXPECT_EQ(table[0].count, 2u);
    EXPECT_EQ(table[0].bytes, 4 * mb);
}

// Zero-byte allocations carry no information and would only dilute the counts.
TEST(AllocAttribution, IgnoresAnEmptyAllocation)
{
    auto table = AllocAttribution{};

    EXPECT_FALSE(attribute(table, system_d3d9, 0u));
    EXPECT_EQ(attributed_modules(table), 0u);
}
