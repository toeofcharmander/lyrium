#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/dao/pool_occupancy.h"

using lyrium::dao::BlockTally;
using lyrium::dao::tally_block;

TEST(PoolOccupancy, AnInUseBlockCountsAsUsed)
{
    auto tally = BlockTally{};

    EXPECT_TRUE(tally_block(tally, 4096u, true, 1u << 20u));

    EXPECT_EQ(tally.blocks, 1u);
    EXPECT_EQ(tally.used_bytes, 4096u);
    EXPECT_EQ(tally.free_bytes, 0u);
}

TEST(PoolOccupancy, AFreeBlockCountsAsFreeAndSetsTheLargest)
{
    auto tally = BlockTally{};

    EXPECT_TRUE(tally_block(tally, 8192u, false, 1u << 20u));

    EXPECT_EQ(tally.free_bytes, 8192u);
    EXPECT_EQ(tally.largest_free_bytes, 8192u);
}

TEST(PoolOccupancy, TheLargestFreeBlockIsTheMaximumNotTheLast)
{
    // The whole point of the figure: a pool with plenty free but no single block
    // big enough is the state that makes the engine skip an asset.
    auto tally = BlockTally{};

    EXPECT_TRUE(tally_block(tally, 8192u, false, 1u << 20u));
    EXPECT_TRUE(tally_block(tally, 65536u, false, 1u << 20u));
    EXPECT_TRUE(tally_block(tally, 1024u, false, 1u << 20u));

    EXPECT_EQ(tally.free_bytes, 8192u + 65536u + 1024u);
    EXPECT_EQ(tally.largest_free_bytes, 65536u);
}

TEST(PoolOccupancy, AZeroSizedBlockStopsTheWalk)
{
    // Advancing by the block size means a zero size would never terminate. This
    // runs against live engine memory, so it has to refuse rather than hang.
    auto tally = BlockTally{};

    EXPECT_FALSE(tally_block(tally, 0u, false, 1u << 20u));
    EXPECT_EQ(tally.blocks, 0u);
}

TEST(PoolOccupancy, ABlockRunningPastTheEndOfThePoolStopsTheWalk)
{
    // Read without taking the engine's lock, so a torn read is expected
    // occasionally. Abandoning the sample is correct; believing it is not.
    auto tally = BlockTally{};

    EXPECT_FALSE(tally_block(tally, 4097u, true, 4096u));
    EXPECT_EQ(tally.blocks, 0u);
}

TEST(PoolOccupancy, ABlockExactlyFillingTheRemainderIsAccepted)
{
    auto tally = BlockTally{};

    EXPECT_TRUE(tally_block(tally, 4096u, true, 4096u));
    EXPECT_EQ(tally.blocks, 1u);
}
