#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/diag/size_tally.h"

using lyrium::diag::SizeTally;

namespace
{

constexpr auto kb = std::uint64_t{1024};

}

TEST(SizeTally, StartsAtNothing)
{
    const auto tally = SizeTally{};

    EXPECT_EQ(tally.count(), 0u);
    EXPECT_EQ(tally.bytes(), 0u);
    EXPECT_EQ(tally.largest(), 0u);
}

TEST(SizeTally, AccumulatesCountAndBytes)
{
    auto tally = SizeTally{};

    tally.note(512 * kb);
    tally.note(1400 * kb);

    EXPECT_EQ(tally.count(), 2u);
    EXPECT_EQ(tally.bytes(), 1912 * kb);
}

// The largest single request is what identifies the traffic. A module making a
// thousand 4 KB calls and one making a single 16 MB call reach similar totals
// and are completely different findings.
TEST(SizeTally, RemembersTheLargestSingleRequest)
{
    auto tally = SizeTally{};

    tally.note(600 * kb);
    tally.note(16440 * kb);
    tally.note(700 * kb);

    EXPECT_EQ(tally.largest(), 16440 * kb);
}

TEST(SizeTally, ALaterSmallerRequestDoesNotLowerTheLargest)
{
    auto tally = SizeTally{};

    tally.note(16440 * kb);
    tally.note(1 * kb);

    EXPECT_EQ(tally.largest(), 16440 * kb);
}

// A zero-byte call is still a call: the count is how a shim proves it is
// installed and being reached at all, which is what separated "the hook is not
// firing" from "the traffic is not there" when the heap arena served nothing.
TEST(SizeTally, CountsAZeroByteCall)
{
    auto tally = SizeTally{};

    tally.note(0u);

    EXPECT_EQ(tally.count(), 1u);
    EXPECT_EQ(tally.bytes(), 0u);
    EXPECT_EQ(tally.largest(), 0u);
}

// These sit on an allocation path and must never stop counting.
TEST(SizeTally, NeverFills)
{
    auto tally = SizeTally{};

    for (auto i = 0; i < 100'000; ++i)
    {
        tally.note(kb);
    }

    EXPECT_EQ(tally.count(), 100'000u);
    EXPECT_EQ(tally.bytes(), 100'000u * kb);
}
