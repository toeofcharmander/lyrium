#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/diag/va_region.h"

using lyrium::diag::big_block_threshold;
using lyrium::diag::committed_bucket_thresholds;
using lyrium::diag::free_bucket_thresholds;
using lyrium::diag::two_gigabytes;
using lyrium::diag::usable_below_2g;
using lyrium::diag::walk_stalled;

namespace
{
constexpr auto mb = std::uint64_t{1ull << 20};
}

TEST(VaRegion, ARegionEntirelyBelowTheLineCountsInFull)
{
    EXPECT_EQ(usable_below_2g(0u, 64u * mb), 64u * mb);
    EXPECT_EQ(usable_below_2g(1024u * mb, 512u * mb), 512u * mb);
}

TEST(VaRegion, ARegionStraddlingTheLineCountsOnlyThePartBelowIt)
{
    // 1.75 GB base with a 512 MB region: only the 256 MB under the line counts.
    const auto base = two_gigabytes - 256u * mb;
    EXPECT_EQ(usable_below_2g(base, 512u * mb), 256u * mb);
}

TEST(VaRegion, ARegionAtOrAboveTheLineCountsForNothing)
{
    EXPECT_EQ(usable_below_2g(two_gigabytes, 512u * mb), 0u);
    EXPECT_EQ(usable_below_2g(two_gigabytes + mb, 512u * mb), 0u);
}

TEST(VaRegion, TheLastByteBelowTheLineStillCounts)
{
    EXPECT_EQ(usable_below_2g(two_gigabytes - 1u, 4096u), 1u);
}

TEST(VaRegion, BucketsAreCumulativeNotDisjoint)
{
    // A 600 MB region clears the 512, 256, 128, 64, 32, 16, 4 and 1 MB
    // thresholds, so it counts in eight buckets, not one. This is why the bucket
    // figures in a log are read as "at least this big".
    const auto size = 600u * mb;
    auto matched = 0;
    for (const auto threshold : free_bucket_thresholds)
    {
        if (size >= threshold)
        {
            ++matched;
        }
    }
    EXPECT_EQ(matched, 8);
}

TEST(VaRegion, ThresholdsDescendSoBucketZeroIsTheLargest)
{
    for (std::size_t i = 1u; i < free_bucket_thresholds.size(); ++i)
    {
        EXPECT_LT(free_bucket_thresholds[i], free_bucket_thresholds[i - 1u]);
    }
    for (std::size_t i = 1u; i < committed_bucket_thresholds.size(); ++i)
    {
        EXPECT_LT(committed_bucket_thresholds[i], committed_bucket_thresholds[i - 1u]);
    }
}

TEST(VaRegion, TheBigBlockThresholdSitsInsideTheCommittedBuckets)
{
    EXPECT_EQ(big_block_threshold, 16u * mb);
    EXPECT_GE(committed_bucket_thresholds.front(), big_block_threshold);
    EXPECT_LE(committed_bucket_thresholds.back(), big_block_threshold);
}

TEST(VaRegion, TheWalkStopsWhenARegionDoesNotAdvanceTheCursor)
{
    // The guard that stops a degenerate VirtualQuery result spinning forever.
    // A hang here would freeze the game from a diagnostic, so it is worth
    // pinning rather than trusting.
    EXPECT_TRUE(walk_stalled(0x1000u, 0x0u, 0x0u)) << "a zero-size region never advances";
    EXPECT_TRUE(walk_stalled(0x2000u, 0x1000u, 0x1000u)) << "ending exactly at the cursor is no progress";
    EXPECT_TRUE(walk_stalled(0x2000u, 0x0u, 0x500u)) << "ending before the cursor is backwards";

    EXPECT_FALSE(walk_stalled(0x1000u, 0x1000u, 0x1000u)) << "advancing past the cursor is progress";
}

// Compile-time so the clamp cannot regress silently.
static_assert(usable_below_2g(0u, 4096u) == 4096u);
static_assert(usable_below_2g(two_gigabytes, 4096u) == 0u);
static_assert(usable_below_2g(two_gigabytes - 1024u, 4096u) == 1024u);

// Which free-block figure the rescue should be measured against.
//
// The probe took the smaller of the two on every install. On a
// large-address-aware process that is the wrong constraint: Windows allocates
// bottom-up and serves each reservation from the lowest hole that fits, so when
// no low hole fits it serves from the untouched region above the 2 GB line by
// itself. Across every captured session the low block fell as far as 12 MB with
// creates never once failing, while the rescue fired and evicted -- stutter
// spent on a problem the OS was already handling.
TEST(HeadroomChoice, ALargeAddressAwareProcessIsMeasuredByTheWholeSpace)
{
    // Low half nearly gone, high half untouched: not an emergency.
    EXPECT_EQ(lyrium::diag::headroom_bytes(2'146'115'584ull, 12u * 1024u * 1024u, true), 2'146'115'584ull);
}

TEST(HeadroomChoice, WithoutLargeAddressAwarenessOnlyTheLowSpaceExists)
{
    // No region above the line to spill into, so the low block is the constraint.
    EXPECT_EQ(lyrium::diag::headroom_bytes(600u * 1024u * 1024u, 12u * 1024u * 1024u, false), 12u * 1024u * 1024u);
}

// Unknown headroom must still read as pressure rather than as safety, which is
// what silently disabled the rescue before the sampler's first tick.
TEST(HeadroomChoice, AnUnknownReadingFallsBackToWhicheverIsKnown)
{
    EXPECT_EQ(lyrium::diag::headroom_bytes(0u, 12u * 1024u * 1024u, true), 12u * 1024u * 1024u);
    EXPECT_EQ(lyrium::diag::headroom_bytes(600u * 1024u * 1024u, 0u, false), 600u * 1024u * 1024u);
    EXPECT_EQ(lyrium::diag::headroom_bytes(0u, 0u, true), 0u);
}
