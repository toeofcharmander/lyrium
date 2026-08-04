#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/texture/block_rect.h"

using lyrium::texture::align_to_blocks;
using lyrium::texture::BlockRectKind;
using lyrium::texture::clamp_to_extent;
using lyrium::texture::classify_update_rect;
using lyrium::texture::level_extent;
using lyrium::texture::RectI32;

TEST(BlockRect, AlignmentWidensOutwardToEnclosingBlocks)
{
    // A rect starting mid-block must grow outward, never inward. Narrowing would
    // drop the partially covered blocks and leave stale pixels behind.
    EXPECT_EQ(align_to_blocks(RectI32{2, 2, 6, 6}), (RectI32{0, 0, 8, 8}));
    EXPECT_EQ(align_to_blocks(RectI32{1, 1, 2, 2}), (RectI32{0, 0, 4, 4}));
}

TEST(BlockRect, AlreadyAlignedRectsAreUnchanged)
{
    EXPECT_EQ(align_to_blocks(RectI32{0, 0, 4, 4}), (RectI32{0, 0, 4, 4}));
    EXPECT_EQ(align_to_blocks(RectI32{8, 12, 64, 128}), (RectI32{8, 12, 64, 128}));
}

TEST(BlockRect, ClampingBoundsTheRectToTheLevel)
{
    EXPECT_EQ(clamp_to_extent(RectI32{-8, -8, 999, 999}, 64, 32), (RectI32{0, 0, 64, 32}));
    EXPECT_EQ(clamp_to_extent(RectI32{4, 4, 8, 8}, 64, 64), (RectI32{4, 4, 8, 8}));
}

TEST(BlockRect, AFullSurfaceRectIsReportedAsCoveringTheLevel)
{
    const auto result = classify_update_rect(RectI32{0, 0, 64, 64}, 64, 64, true);

    EXPECT_EQ(result.kind, BlockRectKind::covers_level);
    EXPECT_EQ(result.rect, (RectI32{0, 0, 64, 64}));
}

TEST(BlockRect, AnOversizedRectStillCountsAsCoveringTheLevel)
{
    const auto result = classify_update_rect(RectI32{0, 0, 4096, 4096}, 64, 64, true);
    EXPECT_EQ(result.kind, BlockRectKind::covers_level);
}

TEST(BlockRect, AGenuineSubRectangleIsReportedAsPartial)
{
    const auto result = classify_update_rect(RectI32{8, 8, 16, 16}, 64, 64, true);

    EXPECT_EQ(result.kind, BlockRectKind::partial);
    EXPECT_EQ(result.rect, (RectI32{8, 8, 16, 16}));
}

TEST(BlockRect, AnUnalignedSubRectangleIsSnappedBeforeBeingClassified)
{
    // This is the case the alignment exists for. Without snapping, left = 2
    // truncates into block 0 and the copy lands two pixels off.
    const auto result = classify_update_rect(RectI32{2, 2, 10, 10}, 64, 64, true);

    EXPECT_EQ(result.kind, BlockRectKind::partial);
    EXPECT_EQ(result.rect, (RectI32{0, 0, 12, 12}));
}

TEST(BlockRect, UncompressedFormatsAreNotSnapped)
{
    const auto result = classify_update_rect(RectI32{2, 2, 10, 10}, 64, 64, false);

    EXPECT_EQ(result.kind, BlockRectKind::partial);
    EXPECT_EQ(result.rect, (RectI32{2, 2, 10, 10})) << "a linear format addresses individual pixels";
}

TEST(BlockRect, ACollapsedRectIsEmpty)
{
    EXPECT_EQ(classify_update_rect(RectI32{10, 10, 10, 20}, 64, 64, false).kind, BlockRectKind::empty);
    EXPECT_EQ(classify_update_rect(RectI32{10, 10, 20, 10}, 64, 64, false).kind, BlockRectKind::empty);
    EXPECT_EQ(classify_update_rect(RectI32{30, 30, 20, 20}, 64, 64, false).kind, BlockRectKind::empty);
}

TEST(BlockRect, ARectEntirelyOutsideTheLevelIsEmpty)
{
    EXPECT_EQ(classify_update_rect(RectI32{100, 100, 200, 200}, 64, 64, false).kind, BlockRectKind::empty);
    EXPECT_EQ(classify_update_rect(RectI32{-50, -50, -10, -10}, 64, 64, false).kind, BlockRectKind::empty);
}

TEST(BlockRect, SnappingASmallRectOnASmallLevelCanCoverIt)
{
    // A 4x4 level with a 1x1 request: snapping grows it to the whole block, which
    // is the entire level, so the copy can take the cheaper whole-surface path.
    const auto result = classify_update_rect(RectI32{1, 1, 2, 2}, 4, 4, true);

    EXPECT_EQ(result.kind, BlockRectKind::covers_level);
    EXPECT_EQ(result.rect, (RectI32{0, 0, 4, 4}));
}

TEST(BlockRect, MipExtentsHalveAndClampAtOnePixel)
{
    EXPECT_EQ(level_extent(256u, 0u), 256);
    EXPECT_EQ(level_extent(256u, 1u), 128);
    EXPECT_EQ(level_extent(256u, 8u), 1);
    EXPECT_EQ(level_extent(256u, 20u), 1) << "shifting past the end must clamp, not reach zero";
    EXPECT_EQ(level_extent(1u, 0u), 1);
}

// Everything above is constexpr-evaluable, so the same guarantees hold at compile
// time for any future caller that wants them.
static_assert(align_to_blocks(RectI32{2, 2, 6, 6}) == RectI32{0, 0, 8, 8});
static_assert(classify_update_rect(RectI32{0, 0, 64, 64}, 64, 64, true).kind == BlockRectKind::covers_level);
static_assert(level_extent(256u, 20u) == 1);
