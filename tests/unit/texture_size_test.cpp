#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/diag/texture_size.h"

using lyrium::diag::format_bits_per_pixel;
using lyrium::diag::is_block_compressed;
using lyrium::diag::level_bytes;
using lyrium::diag::pool_name;
using lyrium::diag::texture_bytes;

// This is the arithmetic that decides how many bytes every texture is recorded
// as costing, which in turn drives the pool-override threshold and the rescue
// trigger. It is pure, so it is testable against the shim in tests/shim, whose
// values the Windows build proves against the genuine <d3d9.h>.

TEST(TextureSize, ReportsBitsPerPixelForCommonFormats)
{
    EXPECT_EQ(format_bits_per_pixel(D3DFMT_A32B32G32R32F), 128u);
    EXPECT_EQ(format_bits_per_pixel(D3DFMT_A16B16G16R16), 64u);
    EXPECT_EQ(format_bits_per_pixel(D3DFMT_A8R8G8B8), 32u);
    EXPECT_EQ(format_bits_per_pixel(D3DFMT_X8R8G8B8), 32u);
    EXPECT_EQ(format_bits_per_pixel(D3DFMT_R8G8B8), 24u);
    EXPECT_EQ(format_bits_per_pixel(D3DFMT_R5G6B5), 16u);
    EXPECT_EQ(format_bits_per_pixel(D3DFMT_A8), 8u);
    EXPECT_EQ(format_bits_per_pixel(D3DFMT_DXT1), 4u);
    EXPECT_EQ(format_bits_per_pixel(D3DFMT_DXT5), 8u);
}

TEST(TextureSize, UnknownFormatsAreOverEstimatedAtThirtyTwoBits)
{
    // The fallback deliberately over-estimates rather than under-estimates, since
    // an under-estimate would let an oversized texture slip past the pool
    // threshold. Pinned because it is a policy choice, not an accident.
    EXPECT_EQ(format_bits_per_pixel(static_cast<D3DFORMAT>(0xDEADBEEFu)), 32u);
    EXPECT_EQ(format_bits_per_pixel(D3DFMT_UNKNOWN), 32u);
}

TEST(TextureSize, OnlyDxtFormatsAreBlockCompressed)
{
    EXPECT_TRUE(is_block_compressed(D3DFMT_DXT1));
    EXPECT_TRUE(is_block_compressed(D3DFMT_DXT2));
    EXPECT_TRUE(is_block_compressed(D3DFMT_DXT3));
    EXPECT_TRUE(is_block_compressed(D3DFMT_DXT4));
    EXPECT_TRUE(is_block_compressed(D3DFMT_DXT5));

    EXPECT_FALSE(is_block_compressed(D3DFMT_A8R8G8B8));
    EXPECT_FALSE(is_block_compressed(D3DFMT_UNKNOWN));
    EXPECT_FALSE(is_block_compressed(D3DFMT_A32B32G32R32F));
}

TEST(TextureSize, BlockFormatsCostOneBlockMinimum)
{
    // A 4x4 DXT1 block is 8 bytes and a DXT5 block is 16. Crucially a 1x1 level
    // still costs a whole block, which is the (w + 3) / 4 rounding and the reason
    // a mip chain does not shrink to nothing.
    EXPECT_EQ(level_bytes(4u, 4u, 1u, D3DFMT_DXT1), 8u);
    EXPECT_EQ(level_bytes(4u, 4u, 1u, D3DFMT_DXT5), 16u);
    EXPECT_EQ(level_bytes(1u, 1u, 1u, D3DFMT_DXT1), 8u);
    EXPECT_EQ(level_bytes(1u, 1u, 1u, D3DFMT_DXT5), 16u);

    // 5 pixels wide is two blocks, not one and a quarter.
    EXPECT_EQ(level_bytes(5u, 4u, 1u, D3DFMT_DXT1), 16u);
}

TEST(TextureSize, LinearFormatsAreWidthTimesHeightTimesDepth)
{
    EXPECT_EQ(level_bytes(256u, 256u, 1u, D3DFMT_A8R8G8B8), 256u * 256u * 4u);
    EXPECT_EQ(level_bytes(16u, 16u, 4u, D3DFMT_A8R8G8B8), 16u * 16u * 4u * 4u);
    EXPECT_EQ(level_bytes(8u, 8u, 1u, D3DFMT_A8), 64u);
}

TEST(TextureSize, ClampingOfZeroExtentsIsAsymmetricButUnreachable)
{
    // Characterisation, not endorsement. The block-compressed path clamps width
    // and height to 1, while the linear path clamps only depth, so a zero extent
    // yields one block from the former and zero bytes from the latter.
    EXPECT_EQ(level_bytes(0u, 0u, 1u, D3DFMT_DXT1), 8u) << "block path clamps width and height";
    EXPECT_EQ(level_bytes(0u, 0u, 1u, D3DFMT_A8), 0u) << "linear path does not clamp width or height";

    // Both paths do clamp depth.
    EXPECT_EQ(level_bytes(4u, 4u, 0u, D3DFMT_DXT1), 8u);
    EXPECT_EQ(level_bytes(4u, 4u, 0u, D3DFMT_A8), 16u);

    // The inconsistency is unreachable in practice: level_bytes has exactly one
    // caller, and texture_bytes clamps all three extents to at least 1 first. If
    // level_bytes ever gains a second caller, make the clamping symmetric before
    // that caller can pass a zero.
    EXPECT_EQ(texture_bytes(0u, 0u, 0u, 1u, D3DFMT_A8), 1u);
    EXPECT_EQ(texture_bytes(0u, 0u, 0u, 1u, D3DFMT_DXT1), 8u);
}

TEST(TextureSize, ExplicitLevelCountSumsOnlyThoseLevels)
{
    EXPECT_EQ(texture_bytes(256u, 256u, 1u, 1u, D3DFMT_A8R8G8B8), 256u * 256u * 4u);
    EXPECT_EQ(
        texture_bytes(256u, 256u, 1u, 2u, D3DFMT_A8R8G8B8),
        (256u * 256u * 4u) + (128u * 128u * 4u));
}

TEST(TextureSize, ZeroLevelsDerivesTheWholeMipChain)
{
    // 256 halves nine times before reaching 1, so the chain is 9 levels and the
    // total is 4 * (256^2 + 128^2 + ... + 1^2) == 4 * 87381.
    std::uint64_t expected = 0u;
    for (std::uint32_t extent = 256u; extent >= 1u; extent /= 2u)
    {
        expected += static_cast<std::uint64_t>(extent) * extent * 4u;
        if (extent == 1u)
        {
            break;
        }
    }

    EXPECT_EQ(expected, 349524u) << "the analytic sum should be 4 * 87381";
    EXPECT_EQ(texture_bytes(256u, 256u, 1u, 0u, D3DFMT_A8R8G8B8), expected);
}

TEST(TextureSize, MipChainOfANonSquareTextureRunsToTheLongerEdge)
{
    // The level count comes from max(width, height, depth), so a 256x64 texture
    // keeps producing levels after its height has already clamped at 1.
    const auto total = texture_bytes(256u, 64u, 1u, 0u, D3DFMT_A8R8G8B8);

    std::uint64_t expected = 0u;
    std::uint32_t w = 256u;
    std::uint32_t h = 64u;
    for (int level = 0; level < 9; ++level)
    {
        expected += static_cast<std::uint64_t>(w) * h * 4u;
        w = w / 2u > 1u ? w / 2u : 1u;
        h = h / 2u > 1u ? h / 2u : 1u;
    }

    EXPECT_EQ(total, expected);
}

TEST(TextureSize, CubeMapsMultiplyByFaceCount)
{
    const auto one_face = texture_bytes(64u, 64u, 1u, 0u, D3DFMT_A8R8G8B8, 1u);
    const auto six_faces = texture_bytes(64u, 64u, 1u, 0u, D3DFMT_A8R8G8B8, 6u);

    EXPECT_EQ(six_faces, one_face * 6u);
    EXPECT_EQ(texture_bytes(64u, 64u, 1u, 0u, D3DFMT_A8R8G8B8, 0u), one_face) << "zero faces is treated as one";
}

TEST(TextureSize, ADxtTextureIsFarSmallerThanTheSameSizeUncompressed)
{
    // The pool-override policy only relocates block-compressed textures, so the
    // gap between these two is what the 256 KB threshold is measured against.
    const auto compressed = texture_bytes(1024u, 1024u, 1u, 0u, D3DFMT_DXT1);
    const auto uncompressed = texture_bytes(1024u, 1024u, 1u, 0u, D3DFMT_A8R8G8B8);

    EXPECT_LT(compressed, uncompressed / 6u);
    EXPECT_GT(compressed, 512u * 1024u) << "a full 1024 DXT1 chain should still clear the 256 KB threshold";
}

TEST(TextureSize, NamesEveryPoolAndFallsBackForUnknown)
{
    EXPECT_STREQ(pool_name(D3DPOOL_DEFAULT), "DEFAULT");
    EXPECT_STREQ(pool_name(D3DPOOL_MANAGED), "MANAGED");
    EXPECT_STREQ(pool_name(D3DPOOL_SYSTEMMEM), "SYSTEMMEM");
    EXPECT_STREQ(pool_name(D3DPOOL_SCRATCH), "SCRATCH");
    EXPECT_STREQ(pool_name(static_cast<D3DPOOL>(99u)), "UNKNOWN");
}
