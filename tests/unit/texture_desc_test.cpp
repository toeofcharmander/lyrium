#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/texture/texture_desc.h"

using lyrium::texture::PixelFormat;
using lyrium::texture::TextureDesc;
using lyrium::texture::TexturePool;
using lyrium::texture::TextureUsage;

namespace lyrium_usage = lyrium::texture::usage;

TEST(TextureDesc, OrdersAndComparesSoItCanServeAsAShapeKey)
{
    // Defaulting <=> is what lets this replace the recycler's separate Key
    // struct, which duplicated the same six fields.
    const auto a = TextureDesc{.width = 256u, .height = 256u, .format = PixelFormat::D3DFMT_DXT1};
    auto b = a;

    EXPECT_EQ(a, b);
    b.width = 512u;
    EXPECT_NE(a, b);
    EXPECT_LT(a, b);
}

TEST(TextureDesc, RecognisesBlockCompressedFormats)
{
    EXPECT_TRUE(TextureDesc{.format = PixelFormat::D3DFMT_DXT1}.is_block_compressed());
    EXPECT_TRUE(TextureDesc{.format = PixelFormat::D3DFMT_DXT5}.is_block_compressed());
    EXPECT_FALSE(TextureDesc{.format = PixelFormat::D3DFMT_A8R8G8B8}.is_block_compressed());
    EXPECT_FALSE(TextureDesc{.format = PixelFormat::D3DFMT_UNKNOWN}.is_block_compressed());
}

TEST(TextureDesc, RelocatabilityFollowsTheDisqualifyingUsageSet)
{
    EXPECT_TRUE(TextureDesc{.usage = lyrium_usage::none}.is_relocatable());
    EXPECT_FALSE(TextureDesc{.usage = lyrium_usage::render_target}.is_relocatable());
    EXPECT_FALSE(TextureDesc{.usage = lyrium_usage::depth_stencil}.is_relocatable());
    EXPECT_FALSE(TextureDesc{.usage = lyrium_usage::dynamic}.is_relocatable());
    EXPECT_FALSE(TextureDesc{.usage = lyrium_usage::autogen_mipmap}.is_relocatable());

    // An unrelated bit must not disqualify.
    EXPECT_TRUE(TextureDesc{.usage = TextureUsage{0x00000008u}}.is_relocatable());

    // A combination that includes a disqualifying bit still fails.
    EXPECT_FALSE(TextureDesc{.usage = TextureUsage{0x00000008u | lyrium_usage::dynamic.bits}}.is_relocatable());
}

TEST(TextureDesc, NamesEveryPool)
{
    EXPECT_STREQ(lyrium::texture::pool_name(TexturePool::D3DPOOL_DEFAULT), "DEFAULT");
    EXPECT_STREQ(lyrium::texture::pool_name(TexturePool::D3DPOOL_MANAGED), "MANAGED");
    EXPECT_STREQ(lyrium::texture::pool_name(TexturePool::D3DPOOL_SYSTEMMEM), "SYSTEMMEM");
    EXPECT_STREQ(lyrium::texture::pool_name(TexturePool::D3DPOOL_SCRATCH), "SCRATCH");
    EXPECT_STREQ(lyrium::texture::pool_name(static_cast<TexturePool>(99u)), "UNKNOWN");
}
