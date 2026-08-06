#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/dao/size_histogram.h"

using lyrium::dao::bytes_at_or_above;
using lyrium::dao::note_size;
using lyrium::dao::size_bucket;
using lyrium::dao::SizeHistogram;

namespace
{

constexpr auto kb = std::uint64_t{1024};
constexpr auto mb = std::uint64_t{1024} * 1024;

}

TEST(SizeHistogram, BucketIsThePositionOfTheHighestSetBit)
{
    // Bucket i holds sizes in [2^i, 2^(i+1)), so the bucket index reads directly
    // as "this allocation is at least 2^i bytes".
    EXPECT_EQ(size_bucket(1u), 0u);
    EXPECT_EQ(size_bucket(2u), 1u);
    EXPECT_EQ(size_bucket(3u), 1u);
    EXPECT_EQ(size_bucket(4u), 2u);
    EXPECT_EQ(size_bucket(1023u), 9u);
    EXPECT_EQ(size_bucket(1024u), 10u);
}

TEST(SizeHistogram, ZeroLandsInBucketZeroRatherThanUndefined)
{
    // The engine's allocator returns null for a zero-size request without asking,
    // so these are excluded from the failure count -- but they still reach the
    // histogram, and a count-leading-zeros on zero is undefined.
    EXPECT_EQ(size_bucket(0u), 0u);
}

TEST(SizeHistogram, TheLargestObservedRequestFitsTheTopBuckets)
{
    // 71.6 MB, the largest single request measured in a session. It has to land
    // somewhere sensible or the histogram cannot describe the class that fails.
    EXPECT_EQ(size_bucket(75075791u), 26u);
    EXPECT_LT(size_bucket(0xFFFFFFFFu), SizeHistogram::bucket_count);
}

TEST(SizeHistogram, NotingASizeCountsItAndItsBytes)
{
    auto histogram = SizeHistogram{};

    note_size(histogram, 4096u);
    note_size(histogram, 5000u);

    EXPECT_EQ(histogram.counts[12], 2u);
    EXPECT_EQ(histogram.bytes[12], 4096u + 5000u);
}

TEST(SizeHistogram, BytesAtOrAboveIsCumulativeFromTheTop)
{
    // This is the figure that sizes the arena: given a candidate threshold, how
    // many bytes would have been diverted to it.
    auto histogram = SizeHistogram{};

    note_size(histogram, 2u * mb);
    note_size(histogram, 8u * mb);
    note_size(histogram, 32u * kb);

    EXPECT_EQ(bytes_at_or_above(histogram, 1u * mb), 10u * mb);
    EXPECT_EQ(bytes_at_or_above(histogram, 4u * mb), 8u * mb);
    EXPECT_EQ(bytes_at_or_above(histogram, 64u * mb), 0u);
}

TEST(SizeHistogram, BytesAtOrAboveCountsAThresholdOfZeroAsEverything)
{
    auto histogram = SizeHistogram{};

    note_size(histogram, 100u);
    note_size(histogram, 1u * mb);

    EXPECT_EQ(bytes_at_or_above(histogram, 0u), 100u + 1u * mb);
}

TEST(SizeHistogram, AThresholdBetweenPowersOfTwoRoundsToTheBucketBelow)
{
    // Deliberate and worth pinning: buckets are powers of two, so a threshold of
    // 1.5 MB can only be answered as "at least 1 MB". It over-reports rather than
    // under-reports, which is the safe direction for sizing an arena.
    auto histogram = SizeHistogram{};

    note_size(histogram, 1u * mb);

    EXPECT_EQ(bytes_at_or_above(histogram, mb + mb / 2u), 1u * mb);
}
