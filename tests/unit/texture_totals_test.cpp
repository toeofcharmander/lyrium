#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lyrium/diag/texture_totals.h"

using lyrium::diag::note_texture_created;
using lyrium::diag::note_texture_released;
using lyrium::diag::texture_totals;
using lyrium::diag::TextureTotals;

// These counters are process-global inline variables with no reset hook, so every
// assertion here is a delta against a snapshot taken in SetUp rather than an
// absolute value. Phase 3 replaces this header with an owned TextureLedger, and
// these tests are the specification that replacement has to satisfy.

namespace
{

class TextureTotalsCounters : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        before = texture_totals();
    }

    TextureTotals before{};
};

}

TEST_F(TextureTotalsCounters, CreationAddsToTheTotalThePoolAndTheLiveCount)
{
    note_texture_created(1u, 4096u);

    const auto after = texture_totals();
    EXPECT_EQ(after.total - before.total, 4096u);
    EXPECT_EQ(after.by_pool[1] - before.by_pool[1], 4096u);
    EXPECT_EQ(after.live_count - before.live_count, 1u);
}

TEST_F(TextureTotalsCounters, ReleaseReversesTheTotalAndAccumulatesReleasedBytes)
{
    note_texture_created(2u, 8192u);
    note_texture_released(2u, 8192u);

    const auto after = texture_totals();
    EXPECT_EQ(after.total, before.total) << "a matched create and release must leave the total unchanged";
    EXPECT_EQ(after.by_pool[2], before.by_pool[2]);
    EXPECT_EQ(after.live_count, before.live_count);
    EXPECT_EQ(after.released_bytes - before.released_bytes, 8192u) << "released bytes accumulate, they do not net off";
}

TEST_F(TextureTotalsCounters, PeakRatchetsUpwardAndNeverFalls)
{
    note_texture_created(0u, 1024u * 1024u);
    const auto high = texture_totals().peak;

    note_texture_released(0u, 1024u * 1024u);

    const auto after = texture_totals();
    EXPECT_EQ(after.peak, high) << "peak is a high-water mark and must survive the release";
    EXPECT_GE(after.peak, before.peak);
}

TEST_F(TextureTotalsCounters, AnOutOfRangePoolStillMovesTheGlobalTotal)
{
    // Characterisation of a real asymmetry. The per-pool array holds four
    // entries, so a pool index of 4 or more is skipped there, but the global
    // total is updated unconditionally. The books therefore disagree: the sum of
    // by_pool no longer equals total.
    //
    // TextureLedger must not reproduce this. Deriving every counter from one
    // record map makes the disagreement unrepresentable.
    note_texture_created(9u, 2048u);

    const auto after = texture_totals();
    EXPECT_EQ(after.total - before.total, 2048u) << "the global total is updated even for an unknown pool";

    std::uint64_t pool_delta = 0u;
    for (std::size_t i = 0u; i < 4u; ++i)
    {
        pool_delta += after.by_pool[i] - before.by_pool[i];
    }
    EXPECT_EQ(pool_delta, 0u) << "no per-pool bucket absorbed it, so the two views now disagree";

    note_texture_released(9u, 2048u);
}

TEST_F(TextureTotalsCounters, ConcurrentCreatesAndReleasesConverge)
{
    static constexpr auto threads = 8;
    static constexpr auto per_thread = 500;

    std::vector<std::thread> workers{};
    workers.reserve(threads);
    for (int t = 0; t < threads; ++t)
    {
        workers.emplace_back([] {
            for (int i = 0; i < per_thread; ++i)
            {
                note_texture_created(1u, 64u);
                note_texture_released(1u, 64u);
            }
        });
    }
    for (auto &worker : workers)
    {
        worker.join();
    }

    const auto after = texture_totals();
    EXPECT_EQ(after.total, before.total);
    EXPECT_EQ(after.live_count, before.live_count);
    EXPECT_EQ(after.released_bytes - before.released_bytes,
              static_cast<std::uint64_t>(threads) * per_thread * 64u);
}
