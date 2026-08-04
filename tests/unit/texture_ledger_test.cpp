#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lyrium/texture/texture_ledger.h"

using lyrium::texture::TextureHandle;
using lyrium::texture::TextureLedger;
using lyrium::texture::TexturePool;

namespace
{

// Distinct non-null handles. Their values are never dereferenced; the ledger
// only ever uses them as identity.
auto handle(std::uintptr_t n) -> TextureHandle
{
    return reinterpret_cast<TextureHandle>(n * 16u + 16u);
}

constexpr auto one_mb = std::uint64_t{1024u * 1024u};

}

TEST(TextureLedger, StartsEmpty)
{
    const TextureLedger ledger{};
    const auto totals = ledger.totals();

    EXPECT_EQ(totals.total, 0u);
    EXPECT_EQ(totals.live_count, 0u);
    EXPECT_EQ(totals.peak, 0u);
    EXPECT_EQ(totals.released_bytes, 0u);
}

TEST(TextureLedger, RecordsACreationAgainstItsPool)
{
    TextureLedger ledger{};
    ledger.note_created(handle(1), TexturePool::D3DPOOL_MANAGED, one_mb);

    const auto totals = ledger.totals();
    EXPECT_EQ(totals.total, one_mb);
    EXPECT_EQ(totals.live_count, 1u);
    EXPECT_EQ(totals.by_pool[static_cast<std::size_t>(TexturePool::D3DPOOL_MANAGED)], one_mb);
    EXPECT_TRUE(ledger.is_tracked(handle(1)));
}

TEST(TextureLedger, ACreateAndDestroyCycleLeavesNothingBehind)
{
    // The defect this type replaces: a texture retained by the recycler was
    // removed from one accounting system and left in the others, so repeated
    // cycles ratcheted the byte total upward for the life of the process.
    TextureLedger ledger{};

    for (int cycle = 0; cycle < 100; ++cycle)
    {
        ledger.note_created(handle(1), TexturePool::D3DPOOL_DEFAULT, one_mb);
        ASSERT_TRUE(ledger.note_destroyed(handle(1)));
    }

    const auto totals = ledger.totals();
    EXPECT_EQ(totals.total, 0u) << "the total must return to zero, not ratchet";
    EXPECT_EQ(totals.live_count, 0u);
    EXPECT_EQ(totals.pool_sum(), 0u);
    EXPECT_EQ(totals.released_bytes, 100u * one_mb) << "released bytes accumulate, they do not net off";
}

TEST(TextureLedger, DestroyingAnUnknownHandleIsANoOp)
{
    // The old counters had no floor here and wrapped the unsigned total to near
    // its maximum. Being derived from the record map makes that unrepresentable.
    TextureLedger ledger{};

    EXPECT_FALSE(ledger.note_destroyed(handle(99)));

    const auto totals = ledger.totals();
    EXPECT_EQ(totals.total, 0u);
    EXPECT_EQ(totals.live_count, 0u);
    EXPECT_EQ(totals.released_bytes, 0u);
}

TEST(TextureLedger, DoubleDestroyOnlyCountsOnce)
{
    TextureLedger ledger{};
    ledger.note_created(handle(1), TexturePool::D3DPOOL_DEFAULT, one_mb);

    EXPECT_TRUE(ledger.note_destroyed(handle(1)));
    EXPECT_FALSE(ledger.note_destroyed(handle(1))) << "the second release has nothing to release";

    const auto totals = ledger.totals();
    EXPECT_EQ(totals.total, 0u);
    EXPECT_EQ(totals.released_bytes, one_mb) << "released bytes must not be counted twice";
}

TEST(TextureLedger, ReRegisteringAHandleReplacesRatherThanAccumulates)
{
    TextureLedger ledger{};
    ledger.note_created(handle(1), TexturePool::D3DPOOL_MANAGED, one_mb);
    ledger.note_created(handle(1), TexturePool::D3DPOOL_DEFAULT, 2u * one_mb);

    const auto totals = ledger.totals();
    EXPECT_EQ(totals.total, 2u * one_mb) << "the second registration replaces the first";
    EXPECT_EQ(totals.live_count, 1u);
    EXPECT_EQ(totals.by_pool[static_cast<std::size_t>(TexturePool::D3DPOOL_MANAGED)], 0u)
        << "the old pool bucket must be credited back";
    EXPECT_EQ(totals.by_pool[static_cast<std::size_t>(TexturePool::D3DPOOL_DEFAULT)], 2u * one_mb);
}

TEST(TextureLedger, PoolBucketsAlwaysSumToTheTotal)
{
    // The invariant the old counters could not hold: an out-of-range pool was
    // skipped in the per-pool array while still moving the global total, after
    // which the two views disagreed permanently.
    TextureLedger ledger{};

    ledger.note_created(handle(1), TexturePool::D3DPOOL_DEFAULT, 100u);
    ledger.note_created(handle(2), TexturePool::D3DPOOL_MANAGED, 200u);
    ledger.note_created(handle(3), TexturePool::D3DPOOL_SYSTEMMEM, 400u);
    ledger.note_created(handle(4), TexturePool::D3DPOOL_SCRATCH, 800u);
    ledger.note_created(handle(5), static_cast<TexturePool>(99u), 1600u);

    auto totals = ledger.totals();
    EXPECT_EQ(totals.total, 3100u);
    EXPECT_EQ(totals.pool_sum(), totals.total) << "an unknown pool lands in its own bucket, not nowhere";
    EXPECT_EQ(totals.by_pool[4], 1600u);

    ledger.note_destroyed(handle(5));
    totals = ledger.totals();
    EXPECT_EQ(totals.pool_sum(), totals.total);
    EXPECT_EQ(totals.by_pool[4], 0u);
}

TEST(TextureLedger, PeakIsAHighWaterMarkThatSurvivesRelease)
{
    TextureLedger ledger{};
    ledger.note_created(handle(1), TexturePool::D3DPOOL_DEFAULT, 4u * one_mb);
    ledger.note_created(handle(2), TexturePool::D3DPOOL_DEFAULT, 4u * one_mb);
    ledger.note_destroyed(handle(1));
    ledger.note_destroyed(handle(2));

    const auto totals = ledger.totals();
    EXPECT_EQ(totals.total, 0u);
    EXPECT_EQ(totals.peak, 8u * one_mb) << "peak records the worst moment, not the current one";
}

TEST(TextureLedger, NullHandlesAreIgnored)
{
    TextureLedger ledger{};
    ledger.note_created(nullptr, TexturePool::D3DPOOL_DEFAULT, one_mb);

    EXPECT_EQ(ledger.totals().total, 0u);
    EXPECT_FALSE(ledger.note_destroyed(nullptr));
}

TEST(TextureLedger, ConcurrentTrafficKeepsTheBooksBalanced)
{
    static constexpr auto threads = 8;
    static constexpr auto per_thread = 400;

    TextureLedger ledger{};
    std::vector<std::thread> workers{};
    workers.reserve(threads);

    for (int t = 0; t < threads; ++t)
    {
        workers.emplace_back([&ledger, t] {
            for (int i = 0; i < per_thread; ++i)
            {
                const auto h = handle(static_cast<std::uintptr_t>(t * per_thread + i + 1));
                ledger.note_created(h, TexturePool::D3DPOOL_DEFAULT, 64u);
                ledger.note_destroyed(h);
            }
        });
    }
    for (auto &worker : workers)
    {
        worker.join();
    }

    const auto totals = ledger.totals();
    EXPECT_EQ(totals.total, 0u);
    EXPECT_EQ(totals.live_count, 0u);
    EXPECT_EQ(totals.pool_sum(), 0u);
    EXPECT_EQ(totals.released_bytes, static_cast<std::uint64_t>(threads) * per_thread * 64u);
}
