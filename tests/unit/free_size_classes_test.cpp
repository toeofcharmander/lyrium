#include <array>
#include <cstdint>
#include <numeric>

#include <gtest/gtest.h>

#include "lyrium/diag/free_size_classes.h"

using lyrium::diag::accumulate_free_buckets;
using lyrium::diag::free_bucket_thresholds;
using lyrium::diag::free_size_classes;
using lyrium::diag::FreeBuckets;
using lyrium::diag::sliver_count;
using lyrium::diag::sliver_threshold;

namespace
{

constexpr auto kb = std::uint64_t{1024};
constexpr auto mb = kb * 1024;

// Accumulate a list of free-block sizes exactly as the address-space walk does.
auto buckets_for(std::initializer_list<std::uint64_t> sizes) -> FreeBuckets
{
    auto buckets = FreeBuckets{};
    for (const auto size : sizes)
    {
        accumulate_free_buckets(buckets, size);
    }
    return buckets;
}

auto total_of(const auto &counts) -> std::uint32_t
{
    return std::accumulate(counts.begin(), counts.end(), std::uint32_t{0});
}

}

// The thresholds are cumulative, so one block counts in every bucket it clears.
TEST(FreeSizeClasses, ABlockCountsInEveryBucketItClears)
{
    const auto buckets = buckets_for({300 * mb});

    // 300 MB clears 256, 128, 64, 32, 16, 4 and 1 MB, but not 512 MB or 1 GB.
    EXPECT_EQ(buckets[0], 0u); // 1 GB
    EXPECT_EQ(buckets[1], 0u); // 512 MB
    EXPECT_EQ(buckets[2], 1u); // 256 MB
    EXPECT_EQ(buckets[8], 1u); // 1 MB
}

// The invariant the shipped overlay violated: every free block belongs to
// exactly one class, so the classes must account for all of them and no more.
TEST(FreeSizeClasses, ClassesAreDisjointAndSumToTheBlockCount)
{
    const auto sizes = {2 * mb, 700 * kb, 40 * mb, 300 * mb, 12 * kb, 5 * mb, 64 * kb};
    const auto buckets = buckets_for(sizes);

    const auto classes = free_size_classes(buckets, static_cast<std::uint32_t>(sizes.size()));

    EXPECT_EQ(total_of(classes), sizes.size());
}

// Each block lands in the class named by the largest threshold it clears.
TEST(FreeSizeClasses, EachBlockLandsInExactlyOneClass)
{
    const auto classes = free_size_classes(buckets_for({300 * mb, 40 * mb, 2 * mb, 700 * kb}), 4u);

    EXPECT_EQ(classes[2], 1u); // 256 MB and up
    EXPECT_EQ(classes[5], 1u); // 32 MB and up
    EXPECT_EQ(classes[8], 1u); // 1 MB and up
    EXPECT_EQ(classes[9], 1u); // below the smallest threshold
    EXPECT_EQ(total_of(classes), 4u);
}

// Blocks smaller than the smallest threshold are counted by nothing, so the
// final class has to be recovered from the total. These are the blocks that
// matter most -- they are the unusable ones.
TEST(FreeSizeClasses, BlocksBelowTheSmallestThresholdLandInTheFinalClass)
{
    const auto classes = free_size_classes(buckets_for({4 * kb, 64 * kb, 900 * kb}), 3u);

    EXPECT_EQ(classes[9], 3u);
    EXPECT_EQ(total_of(classes), 3u);
}

// Regression for the histogram that shipped: a healthy address space with a few
// huge blocks and a long tail of small ones drew as a single spike, because the
// walk filled the buckets ascending while the overlay differenced them
// descending. Every difference went negative, clamped to zero, and the whole
// population fell into the final class.
TEST(FreeSizeClasses, AHealthySpaceDrawsAsADistributionNotOneSpike)
{
    auto buckets = FreeBuckets{};
    accumulate_free_buckets(buckets, 2047ull * mb); // the untouched space above 2 GB
    accumulate_free_buckets(buckets, 289 * mb);     // the headroom the overlay reports
    for (auto i = 0; i < 6; ++i)
    {
        accumulate_free_buckets(buckets, 20 * mb);
    }
    for (auto i = 0; i < 30; ++i)
    {
        accumulate_free_buckets(buckets, 2 * mb);
    }
    for (auto i = 0; i < 201; ++i)
    {
        accumulate_free_buckets(buckets, 100 * kb);
    }

    const auto classes = free_size_classes(buckets, 239u);

    EXPECT_EQ(total_of(classes), 239u);
    EXPECT_EQ(classes[0], 1u);   // the 2 GB block
    EXPECT_EQ(classes[2], 1u);   // the 289 MB headroom block
    EXPECT_EQ(classes[6], 6u);   // the 20 MB blocks
    EXPECT_EQ(classes[8], 30u);  // the 2 MB blocks
    EXPECT_EQ(classes[9], 201u); // the sub-megabyte tail

    // The bug drew every block in the final class. Anything that puts the large
    // blocks anywhere else disproves it.
    EXPECT_LT(classes[9], 239u);
}

// The headline number under the histogram. Blocks under 4 MB cannot hold a
// relocated texture, so their count is the fragmentation signal.
TEST(FreeSizeClasses, SliverCountIsTheBlocksUnderFourMegabytes)
{
    EXPECT_EQ(sliver_threshold, 4 * mb);

    const auto classes = free_size_classes(buckets_for({300 * mb, 8 * mb, 3 * mb, 900 * kb, 4 * kb}), 5u);

    EXPECT_EQ(sliver_count(classes), 3u);
}

// A degenerate reading must not produce a nonsense count. The walk and the
// bucket counts are sampled together, but a caller passing a total smaller than
// the buckets already saw should get zero rather than an underflowed count.
TEST(FreeSizeClasses, AnUndercountedTotalDoesNotUnderflow)
{
    EXPECT_EQ(free_size_classes(buckets_for({2 * mb, 3 * mb}), 0u)[9], 0u);
}
