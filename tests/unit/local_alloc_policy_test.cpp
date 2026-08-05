#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/allocators/local_alloc_policy.h"

using lyrium::allocators::lmem_fixed;
using lyrium::allocators::lmem_moveable;
using lyrium::allocators::lmem_zeroinit;
using lyrium::allocators::needs_zeroing;
using lyrium::allocators::serve_from_arena;

namespace
{

constexpr auto kb = std::uint64_t{1024};
constexpr auto threshold = 512 * kb;

}

// The measured traffic: 180 calls of 512 KB or more, 280 MB in total, largest
// 16 MB, every one of them a MANAGED texture duplicate. Those are what become
// their own reservation and cut the address space.
TEST(LocalAllocPolicy, ServesTheLargeFixedRequestsThatFragment)
{
    EXPECT_TRUE(serve_from_arena(lmem_fixed, 1400 * kb, threshold));
    EXPECT_TRUE(serve_from_arena(lmem_fixed, 16384 * kb, threshold));
}

// Below the NT heap's own threshold a request is sub-allocated inside an
// existing segment and costs no new region, so taking it would be work for
// nothing -- and 104,598 of the 104,598 calls measured were that size.
TEST(LocalAllocPolicy, LeavesSmallRequestsAlone)
{
    EXPECT_FALSE(serve_from_arena(lmem_fixed, 4 * kb, threshold));
    EXPECT_FALSE(serve_from_arena(lmem_fixed, threshold - 1u, threshold));
}

TEST(LocalAllocPolicy, TheThresholdItselfIsServed)
{
    EXPECT_TRUE(serve_from_arena(lmem_fixed, threshold, threshold));
}

// LMEM_MOVEABLE makes LocalAlloc return a handle rather than a pointer, which
// the caller must LocalLock before touching. An arena hands back a pointer, so
// serving a moveable request would return something the caller would then
// dereference as a handle.
//
// d3d9.dll imports no LocalLock at all, so it cannot be using moveable memory
// today. This refuses anyway: the cost is passing through a call that never
// happens, and the cost of being wrong is silent corruption.
TEST(LocalAllocPolicy, RefusesMoveableRequestsHoweverLarge)
{
    EXPECT_FALSE(serve_from_arena(lmem_moveable, 16384 * kb, threshold));
    EXPECT_FALSE(serve_from_arena(lmem_moveable | lmem_zeroinit, 16384 * kb, threshold));
}

TEST(LocalAllocPolicy, RefusesAnEmptyRequest)
{
    EXPECT_FALSE(serve_from_arena(lmem_fixed, 0u, threshold));
}

// LPTR is LMEM_FIXED | LMEM_ZEROINIT and is the common way to ask for zeroed
// memory. Arena memory is recycled and therefore dirty, so missing this hands
// the runtime garbage where it expects zeroes -- a correctness bug that would
// show up as wrong pixels rather than as a crash, which is the worst way for one
// to show up.
TEST(LocalAllocPolicy, ZeroInitIsHonoured)
{
    EXPECT_TRUE(needs_zeroing(lmem_fixed | lmem_zeroinit));
    EXPECT_TRUE(needs_zeroing(lmem_zeroinit));
}

TEST(LocalAllocPolicy, MemoryIsNotZeroedWhenItWasNotAskedFor)
{
    EXPECT_FALSE(needs_zeroing(lmem_fixed));
}

// The real constants, so a future edit cannot quietly redefine them into
// something that still passes every test above.
TEST(LocalAllocPolicy, MatchesTheWindowsFlagValues)
{
    EXPECT_EQ(lmem_fixed, 0x0000u);
    EXPECT_EQ(lmem_moveable, 0x0002u);
    EXPECT_EQ(lmem_zeroinit, 0x0040u);
}
