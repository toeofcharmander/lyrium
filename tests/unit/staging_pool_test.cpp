#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/texture/staging_pool.h"

using lyrium::texture::BasicStagingPool;
using lyrium::texture::StagingShape;

namespace
{

constexpr auto mb = std::uint64_t{1024} * 1024u;

// The handle type is opaque to the pool; ints keep the tests honest about that.
using Pool = BasicStagingPool<int>;

constexpr auto shape_a = StagingShape{.width = 1024u, .height = 1024u, .format = 21u};
constexpr auto shape_b = StagingShape{.width = 512u, .height = 512u, .format = 21u};

}

// The pool exists to make the second upload of a shape free. The first acquire
// of a shape misses, the caller creates, offers it back, and the next acquire of
// the same shape returns it instead of forcing another driver creation.
TEST(StagingPool, ReturnsAnOfferedHandleForTheSameShape)
{
    auto pool = Pool{32u * mb, 8u};

    EXPECT_FALSE(pool.acquire(shape_a).has_value());
    EXPECT_TRUE(pool.offer(shape_a, 1u * mb, 7));

    const auto reused = pool.acquire(shape_a);
    ASSERT_TRUE(reused.has_value());
    EXPECT_EQ(*reused, 7);
}

// A staging texture is only substitutable for an identical shape. Handing back
// a mismatched one would corrupt the upload, so a different shape must miss.
TEST(StagingPool, ADifferentShapeMisses)
{
    auto pool = Pool{32u * mb, 8u};
    EXPECT_TRUE(pool.offer(shape_a, 1u * mb, 7));

    EXPECT_FALSE(pool.acquire(shape_b).has_value());
}

// Acquire transfers ownership out of the pool; the same handle must not be
// handed to two callers.
TEST(StagingPool, AcquireRemovesTheEntry)
{
    auto pool = Pool{32u * mb, 8u};
    EXPECT_TRUE(pool.offer(shape_a, 1u * mb, 7));

    ASSERT_TRUE(pool.acquire(shape_a).has_value());
    EXPECT_FALSE(pool.acquire(shape_a).has_value());
}

// Pooled SYSTEMMEM textures hold the game's address space permanently, which is
// the resource this project exists to protect. The byte budget is the ceiling on
// that cost: an offer that would exceed it is refused, and the caller destroys
// the texture instead.
TEST(StagingPool, RefusesAnOfferThatWouldExceedTheByteBudget)
{
    auto pool = Pool{4u * mb, 8u};
    EXPECT_TRUE(pool.offer(shape_a, 3u * mb, 1));

    EXPECT_FALSE(pool.offer(shape_b, 2u * mb, 2));
    EXPECT_EQ(pool.held_bytes(), 3u * mb);
}

// The entry cap bounds the linear scan as well as the object count.
TEST(StagingPool, RefusesAnOfferBeyondTheEntryCap)
{
    auto pool = Pool{32u * mb, 2u};
    EXPECT_TRUE(pool.offer(StagingShape{.width = 1u, .height = 1u, .format = 1u}, 16u, 1));
    EXPECT_TRUE(pool.offer(StagingShape{.width = 2u, .height = 2u, .format = 1u}, 16u, 2));

    EXPECT_FALSE(pool.offer(StagingShape{.width = 4u, .height = 4u, .format = 1u}, 16u, 3));
}

// Taking a handle out returns its bytes to the budget.
TEST(StagingPool, AcquireReleasesTheBudgetItHeld)
{
    auto pool = Pool{4u * mb, 8u};
    EXPECT_TRUE(pool.offer(shape_a, 3u * mb, 1));
    ASSERT_TRUE(pool.acquire(shape_a).has_value());

    EXPECT_EQ(pool.held_bytes(), 0u);
    EXPECT_TRUE(pool.offer(shape_b, 2u * mb, 2));
}

// Several released textures of one shape can wait at once; each acquire hands
// out a different one. Order among them is unspecified.
TEST(StagingPool, HoldsMultipleEntriesOfTheSameShape)
{
    auto pool = Pool{32u * mb, 8u};
    EXPECT_TRUE(pool.offer(shape_a, 1u * mb, 1));
    EXPECT_TRUE(pool.offer(shape_a, 1u * mb, 2));

    const auto first = pool.acquire(shape_a);
    const auto second = pool.acquire(shape_a);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(*first, *second);
}

// The pool's contents are address space the game may suddenly need more than we
// do. Under rescue pressure the whole pool is surrendered: take_all hands every
// handle back for the caller to destroy and returns the budget to zero.
TEST(StagingPool, TakeAllSurrendersEverything)
{
    auto pool = Pool{32u * mb, 8u};
    EXPECT_TRUE(pool.offer(shape_a, 1u * mb, 1));
    EXPECT_TRUE(pool.offer(shape_b, 2u * mb, 2));

    const auto taken = pool.take_all();

    EXPECT_EQ(taken.size(), 2u);
    EXPECT_EQ(pool.held_bytes(), 0u);
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_FALSE(pool.acquire(shape_a).has_value());
}
