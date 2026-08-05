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

// ---------------------------------------------------------------------------
// Finding the module that asked, rather than the one that allocated.
//
// A live session resolved every one of 256 records to ntdll.dll or KERNEL32.DLL,
// which answers nothing: those are the allocators. RtlAllocateHeap and
// VirtualAlloc are what call NtAllocateVirtualMemory, so there is always at
// least one allocator frame between the client and the syscall and the immediate
// return address can never name the client. The requester is the first frame
// outward that belongs to something else.
// ---------------------------------------------------------------------------

using lyrium::diag::AllocatorModules;
using lyrium::diag::FrameModules;
using lyrium::diag::requesting_module;

namespace
{

constexpr auto ntdll = std::uint64_t{0x76ea0000};
constexpr auto kernel32 = std::uint64_t{0x75550000};
constexpr auto kernelbase = std::uint64_t{0x75000000};

constexpr auto allocators() -> AllocatorModules
{
    return AllocatorModules{ntdll, kernel32, kernelbase, 0u};
}

}

TEST(RequestingModule, SkipsTheAllocatorAndNamesTheCaller)
{
    const auto frames = FrameModules{ntdll, system_d3d9, game_exe, 0u, 0u, 0u, 0u, 0u};

    EXPECT_EQ(requesting_module(frames, 3u, allocators()), system_d3d9);
}

// The observed shape: VirtualAlloc in kernel32 forwarding into ntdll's syscall
// stub, two allocator frames deep before anything real.
TEST(RequestingModule, SkipsSeveralAllocatorFrames)
{
    const auto frames = FrameModules{ntdll, kernelbase, kernel32, driver_umd, 0u, 0u, 0u, 0u};

    EXPECT_EQ(requesting_module(frames, 4u, allocators()), driver_umd);
}

// An unresolvable frame is not a module and must not be reported as one, or a
// gap in the walk would masquerade as the answer.
TEST(RequestingModule, StepsOverAFrameThatResolvedToNothing)
{
    const auto frames = FrameModules{ntdll, 0u, system_d3d9, 0u, 0u, 0u, 0u, 0u};

    EXPECT_EQ(requesting_module(frames, 3u, allocators()), system_d3d9);
}

TEST(RequestingModule, ReportsNothingWhenEveryFrameIsAnAllocator)
{
    const auto frames = FrameModules{ntdll, kernel32, 0u, 0u, 0u, 0u, 0u, 0u};

    EXPECT_EQ(requesting_module(frames, 2u, allocators()), 0u);
}

TEST(RequestingModule, ReportsNothingWithoutFrames)
{
    EXPECT_EQ(requesting_module(FrameModules{}, 0u, allocators()), 0u);
}

// The walk returns fewer frames than the array holds more often than not, and
// the slots past frame_count are stale rather than empty.
TEST(RequestingModule, ReadsNoFurtherThanTheFrameCount)
{
    const auto frames = FrameModules{ntdll, kernel32, system_d3d9, 0u, 0u, 0u, 0u, 0u};

    EXPECT_EQ(requesting_module(frames, 2u, allocators()), 0u) << "read past the frames the walk actually returned";
}

// lyrium is deliberately not an allocator module. If our own staging sections
// are what cut the space, that is a finding we must be able to see.
TEST(RequestingModule, NamesOurselvesRatherThanHidingIt)
{
    constexpr auto lyrium_dll = std::uint64_t{0x10000000};
    const auto frames = FrameModules{ntdll, lyrium_dll, 0u, 0u, 0u, 0u, 0u, 0u};

    EXPECT_EQ(requesting_module(frames, 2u, allocators()), lyrium_dll);
}
