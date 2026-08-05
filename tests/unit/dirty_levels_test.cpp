#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/texture/dirty_levels.h"

using lyrium::texture::DirtyLevels;

// Unlocking a level records that it needs uploading, rather than uploading it
// there and then. Nothing is owed until something is written.
TEST(DirtyLevels, StartsWithNothingOwed)
{
    auto dirty = DirtyLevels{};

    EXPECT_FALSE(dirty.any());
    EXPECT_EQ(dirty.take(), 0u);
}

TEST(DirtyLevels, MarkingALevelOwesIt)
{
    auto dirty = DirtyLevels{};
    dirty.mark(3u);

    EXPECT_TRUE(dirty.any());
    EXPECT_TRUE(dirty.is_dirty(3u));
    EXPECT_FALSE(dirty.is_dirty(2u));
}

// The whole point of batching: a texture written level by level owes one flush
// covering every level, not one flush per level.
TEST(DirtyLevels, AccumulatesSeveralLevelsIntoOneFlush)
{
    auto dirty = DirtyLevels{};
    for (auto level = std::uint32_t{0}; level < 10u; ++level)
    {
        dirty.mark(level);
    }

    const auto owed = dirty.take();

    EXPECT_EQ(owed, 0x3ffu);
    EXPECT_FALSE(dirty.any()) << "take must clear what it hands out, or the flush repeats forever";
}

// Marking the same level twice before a flush still costs one upload.
TEST(DirtyLevels, MarkingTwiceOwesOneUpload)
{
    auto dirty = DirtyLevels{};
    dirty.mark(4u);
    dirty.mark(4u);

    EXPECT_EQ(dirty.take(), 1u << 4u);
}

// A write that lands while a flush is in flight must not be lost: it is owed
// again immediately, so the next bind picks it up.
TEST(DirtyLevels, AWriteAfterTakeIsOwedAgain)
{
    auto dirty = DirtyLevels{};
    dirty.mark(1u);
    EXPECT_EQ(dirty.take(), 1u << 1u);

    dirty.mark(1u);

    EXPECT_TRUE(dirty.any());
    EXPECT_EQ(dirty.take(), 1u << 1u);
}

// Levels beyond the representable range are clamped rather than allowed to
// corrupt a neighbouring bit, matching LevelValidity's contract.
TEST(DirtyLevels, ALevelBeyondCapacityIsIgnored)
{
    auto dirty = DirtyLevels{};
    dirty.mark(DirtyLevels::capacity);
    dirty.mark(DirtyLevels::capacity + 5u);

    EXPECT_FALSE(dirty.any());
}

// A device reset invalidates the GPU-side copy of everything, so every level
// that holds data is owed again regardless of what was flushed before.
TEST(DirtyLevels, MarkAllOwesEveryLevelHoldingData)
{
    auto dirty = DirtyLevels{};
    dirty.mark_all(0x15u);

    EXPECT_EQ(dirty.take(), 0x15u);
}
