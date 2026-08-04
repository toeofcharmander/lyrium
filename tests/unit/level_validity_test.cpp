#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lyrium/texture/level_validity.h"

using lyrium::texture::LevelValidity;

TEST(LevelValidity, StartsWithNothingValid)
{
    const LevelValidity validity{};

    EXPECT_FALSE(validity.any_valid());
    for (std::uint32_t level = 0u; level < LevelValidity::capacity; ++level)
    {
        EXPECT_FALSE(validity.is_valid(level));
    }
}

TEST(LevelValidity, MarksLevelsIndependently)
{
    LevelValidity validity{};
    validity.mark_valid(3u);

    EXPECT_TRUE(validity.is_valid(3u));
    EXPECT_FALSE(validity.is_valid(2u));
    EXPECT_FALSE(validity.is_valid(4u));
    EXPECT_TRUE(validity.any_valid());
}

TEST(LevelValidity, ClearingOneLevelLeavesTheOthers)
{
    LevelValidity validity{};
    validity.mark_valid(0u);
    validity.mark_valid(1u);

    validity.mark_invalid(0u);

    EXPECT_FALSE(validity.is_valid(0u));
    EXPECT_TRUE(validity.is_valid(1u));
}

TEST(LevelValidity, ClearResetsEverything)
{
    LevelValidity validity{};
    validity.mark_valid(0u);
    validity.mark_valid(31u);

    validity.clear();

    EXPECT_FALSE(validity.any_valid());
    EXPECT_FALSE(validity.is_valid(31u));
}

TEST(LevelValidity, OutOfRangeLevelsAreIgnoredRatherThanCorruptingNeighbours)
{
    LevelValidity validity{};
    validity.mark_valid(0u);

    validity.mark_valid(LevelValidity::capacity);
    validity.mark_valid(9999u);
    validity.mark_invalid(9999u);

    EXPECT_TRUE(validity.is_valid(0u)) << "an out-of-range write must not disturb a real level";
    EXPECT_FALSE(validity.is_valid(LevelValidity::capacity));
}

TEST(LevelValidity, ConcurrentMarksOfDistinctLevelsAllSurvive)
{
    // This is the case Vector<bool> could not hold. Adjacent levels share a word
    // there, so two threads marking different levels were a read-modify-write
    // race and one update could be lost.
    for (int repeat = 0; repeat < 200; ++repeat)
    {
        LevelValidity validity{};
        std::vector<std::thread> workers{};
        workers.reserve(LevelValidity::capacity);

        for (std::uint32_t level = 0u; level < LevelValidity::capacity; ++level)
        {
            workers.emplace_back([&validity, level] { validity.mark_valid(level); });
        }
        for (auto &worker : workers)
        {
            worker.join();
        }

        for (std::uint32_t level = 0u; level < LevelValidity::capacity; ++level)
        {
            ASSERT_TRUE(validity.is_valid(level)) << "level " << level << " lost on repeat " << repeat;
        }
    }
}
