#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/texture/mip_layout.h"

using lyrium::texture::blocks_for;
using lyrium::texture::build_mip_layout;

namespace
{
constexpr auto dxt1 = std::uint32_t{8};
constexpr auto dxt5 = std::uint32_t{16};
}

TEST(MipLayout, RoundsPixelsUpToWholeBlocks)
{
    EXPECT_EQ(blocks_for(4u), 1u);
    EXPECT_EQ(blocks_for(5u), 2u) << "one pixel past a block boundary needs a second block";
    EXPECT_EQ(blocks_for(8u), 2u);
    EXPECT_EQ(blocks_for(1u), 1u) << "a 1 pixel level still costs a whole block";
    EXPECT_EQ(blocks_for(0u), 1u) << "zero is clamped, never producing a zero-byte level";
}

TEST(MipLayout, ASingleLevelIsOneBlockRowSet)
{
    const auto layout = build_mip_layout(4u, 4u, 1u, dxt1);

    ASSERT_EQ(layout.levels.size(), 1u);
    EXPECT_EQ(layout.levels[0].pitch, 8u);
    EXPECT_EQ(layout.levels[0].rows, 1u);
    EXPECT_EQ(layout.levels[0].offset, 0u);
    EXPECT_EQ(layout.total_bytes, 8u);
}

TEST(MipLayout, OffsetsAreCumulativeAndNonOverlapping)
{
    // The property that matters most: each level must begin exactly where the
    // previous one ended, or a lock hands the engine a pointer into a
    // neighbouring level.
    const auto layout = build_mip_layout(256u, 256u, 9u, dxt1);

    ASSERT_EQ(layout.levels.size(), 9u);
    std::size_t expected_offset = 0u;
    for (const auto &level : layout.levels)
    {
        EXPECT_EQ(level.offset, expected_offset);
        expected_offset += level.bytes();
    }
    EXPECT_EQ(layout.total_bytes, expected_offset) << "the total must be the sum of every level";
}

TEST(MipLayout, EveryLevelFitsInsideTheMapping)
{
    // What the total is actually used for: the size of the file mapping. If any
    // level's end exceeds it, LockRect writes outside the section.
    for (const auto block_bytes : {dxt1, dxt5})
    {
        for (const auto extent : {1u, 4u, 5u, 17u, 64u, 256u, 1024u})
        {
            const auto layout = build_mip_layout(extent, extent, 6u, block_bytes);
            for (const auto &level : layout.levels)
            {
                EXPECT_LE(level.offset + level.bytes(), layout.total_bytes)
                    << "extent " << extent << " block_bytes " << block_bytes;
            }
        }
    }
}

TEST(MipLayout, DxtFiveIsTwiceTheSizeOfDxtOne)
{
    const auto one = build_mip_layout(256u, 256u, 9u, dxt1);
    const auto five = build_mip_layout(256u, 256u, 9u, dxt5);

    EXPECT_EQ(five.total_bytes, one.total_bytes * 2u);
}

TEST(MipLayout, ExtentsHalveAndClampAtOnePixel)
{
    const auto layout = build_mip_layout(256u, 64u, 9u, dxt1);

    ASSERT_EQ(layout.levels.size(), 9u);
    EXPECT_EQ(layout.levels[0].width, 256u);
    EXPECT_EQ(layout.levels[0].height, 64u);

    // Height reaches 1 first and must stay there while width keeps halving.
    EXPECT_EQ(layout.levels[6].height, 1u);
    EXPECT_EQ(layout.levels[8].height, 1u);
    EXPECT_EQ(layout.levels[8].width, 1u);
}

TEST(MipLayout, NonPowerOfTwoExtentsStillTile)
{
    const auto layout = build_mip_layout(17u, 13u, 3u, dxt1);

    ASSERT_EQ(layout.levels.size(), 3u);
    // 17 pixels is 5 blocks (the 17th needs a fifth), 13 is 4.
    EXPECT_EQ(layout.levels[0].pitch, 5u * dxt1);
    EXPECT_EQ(layout.levels[0].rows, 4u);
    // Halving 17 gives 8, which is 2 blocks; 13 gives 6, which is 2.
    EXPECT_EQ(layout.levels[1].width, 8u);
    EXPECT_EQ(layout.levels[1].pitch, 2u * dxt1);
    EXPECT_EQ(layout.levels[1].rows, 2u);
}

TEST(MipLayout, ZeroLevelsProducesAnEmptyMapping)
{
    const auto layout = build_mip_layout(256u, 256u, 0u, dxt1);

    EXPECT_TRUE(layout.levels.empty());
    EXPECT_EQ(layout.total_bytes, 0u) << "the caller treats a zero size as a failure to map";
}

TEST(MipLayout, MatchesAHandComputedFullChain)
{
    // 256x256 DXT1: 64 blocks across at level 0, halving. Block counts are
    // 64,32,16,8,4,2,1,1,1 so the byte total is 8 * (64*64 + 32*32 + ... + 1).
    const auto layout = build_mip_layout(256u, 256u, 9u, dxt1);

    std::size_t expected = 0u;
    for (const auto blocks : {64u, 32u, 16u, 8u, 4u, 2u, 1u, 1u, 1u})
    {
        expected += static_cast<std::size_t>(blocks) * blocks * dxt1;
    }
    EXPECT_EQ(layout.total_bytes, expected);
}
