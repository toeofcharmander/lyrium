#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lyrium/com/ref_count.h"

using lyrium::com::RefCount;

TEST(RefCount, StartsAtOneAndCountsUpAndDown)
{
    RefCount count{};
    EXPECT_EQ(count.current(), 1u);

    EXPECT_EQ(count.add_ref(), 2u);
    EXPECT_EQ(count.release(), 1u);
    EXPECT_EQ(count.release(), 0u);
}

TEST(RefCount, TryAddRefSucceedsWhileTheObjectIsAlive)
{
    RefCount count{};

    EXPECT_TRUE(count.try_add_ref());
    EXPECT_EQ(count.current(), 2u);
}

TEST(RefCount, TryAddRefRefusesToResurrectAtZero)
{
    // The whole point. A plain fetch_add would take this from 0 to 1 and hand
    // the caller a reference to an object that is already being destroyed.
    RefCount count{};
    ASSERT_EQ(count.release(), 0u);

    EXPECT_FALSE(count.try_add_ref());
    EXPECT_EQ(count.current(), 0u) << "a failed attempt must leave the count untouched";
}

TEST(RefCount, RepeatedFailedAttemptsDoNotDriftTheCount)
{
    RefCount count{};
    ASSERT_EQ(count.release(), 0u);

    for (int i = 0; i < 1000; ++i)
    {
        EXPECT_FALSE(count.try_add_ref());
    }
    EXPECT_EQ(count.current(), 0u);
}

TEST(RefCount, ConcurrentTryAddRefNeverResurrectsADyingObject)
{
    // Models the real race: one thread drops the last reference while many
    // others try to take one from a registry that has not yet been updated.
    // Exactly one outcome is legal -- either a racer got a reference before the
    // count hit zero, or none did and the releaser owns the destruction.
    for (int repeat = 0; repeat < 500; ++repeat)
    {
        RefCount count{};
        std::atomic<bool> go{false};
        std::atomic<int> acquired{0};
        std::atomic<std::uint32_t> remaining_after_release{1u};

        std::vector<std::thread> racers{};
        racers.reserve(8);
        for (int i = 0; i < 8; ++i)
        {
            racers.emplace_back([&] {
                while (!go.load(std::memory_order_acquire))
                {
                }
                if (count.try_add_ref())
                {
                    acquired.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        std::thread releaser{[&] {
            while (!go.load(std::memory_order_acquire))
            {
            }
            remaining_after_release.store(count.release(), std::memory_order_relaxed);
        }};

        go.store(true, std::memory_order_release);
        for (auto &racer : racers)
        {
            racer.join();
        }
        releaser.join();

        // If the releaser saw zero, nothing may have been acquired afterwards:
        // the count must still be zero rather than lifted back up.
        if (remaining_after_release.load() == 0u)
        {
            EXPECT_EQ(count.current(), 0u) << "an object that reached zero was resurrected on repeat " << repeat;
        }
        else
        {
            // Otherwise the count is one per surviving acquirer.
            EXPECT_EQ(count.current(), static_cast<std::uint32_t>(acquired.load()))
                << "count disagrees with the number of successful acquisitions on repeat " << repeat;
        }
    }
}
