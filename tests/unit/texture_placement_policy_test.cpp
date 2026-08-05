#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/policy/texture_placement_policy.h"

using lyrium::policy::PlacementDecision;
using lyrium::policy::TexturePlacementConfig;
using lyrium::policy::TexturePlacementPolicy;
using lyrium::texture::PixelFormat;
using lyrium::texture::TextureDesc;
using lyrium::texture::TexturePool;
using lyrium::texture::TextureUsage;

namespace lyrium_usage = lyrium::texture::usage;

namespace
{

constexpr auto one_mb = std::uint64_t{1024u * 1024u};

// A request that satisfies every condition, so each test can spoil exactly one
// thing and see the decision flip. That is what makes this a truth table rather
// than a pile of examples.
constexpr auto relocatable_request() -> TextureDesc
{
    return TextureDesc{
        .width = 1024u,
        .height = 1024u,
        .depth = 1u,
        .levels = 0u,
        .usage = lyrium_usage::none,
        .format = PixelFormat::D3DFMT_DXT1,
        .pool = TexturePool::D3DPOOL_MANAGED,
    };
}

constexpr auto default_policy() -> TexturePlacementPolicy
{
    return TexturePlacementPolicy{TexturePlacementConfig{}};
}

}

TEST(TexturePlacementPolicy, RelocatesALargeBlockCompressedManagedTexture)
{
    const auto placement = default_policy().place(relocatable_request(), one_mb);

    EXPECT_EQ(placement.decision, PlacementDecision::relocate_to_default);
    EXPECT_EQ(placement.pool, TexturePool::D3DPOOL_DEFAULT);
    EXPECT_TRUE(placement.relocated());
}

TEST(TexturePlacementPolicy, KeepsEverythingWhenDisabled)
{
    const auto policy = TexturePlacementPolicy{TexturePlacementConfig{.prefer_default = false}};
    const auto placement = policy.place(relocatable_request(), one_mb);

    EXPECT_EQ(placement.decision, PlacementDecision::keep_requested);
    EXPECT_EQ(placement.pool, TexturePool::D3DPOOL_MANAGED);
}

TEST(TexturePlacementPolicy, OnlyManagedTexturesAreRelocated)
{
    for (const auto pool : {TexturePool::D3DPOOL_DEFAULT, TexturePool::D3DPOOL_SYSTEMMEM, TexturePool::D3DPOOL_SCRATCH})
    {
        auto desc = relocatable_request();
        desc.pool = pool;

        const auto placement = default_policy().place(desc, one_mb);
        EXPECT_EQ(placement.decision, PlacementDecision::keep_requested)
            << "pool " << lyrium::texture::pool_name(pool) << " should be left alone";
        EXPECT_EQ(placement.pool, pool) << "the requested pool must be preserved unchanged";
    }
}

TEST(TexturePlacementPolicy, EachDisqualifyingUsageBitBlocksRelocationOnItsOwn)
{
    // Tested individually rather than as a mask, because a mistake in any single
    // bit is what relocated video surfaces and broke playback upstream.
    const auto bits = {
        lyrium_usage::render_target,
        lyrium_usage::depth_stencil,
        lyrium_usage::dynamic,
        lyrium_usage::autogen_mipmap,
    };

    for (const auto bit : bits)
    {
        auto desc = relocatable_request();
        desc.usage = bit;

        const auto placement = default_policy().place(desc, one_mb);
        EXPECT_EQ(placement.decision, PlacementDecision::keep_requested)
            << "usage bit 0x" << std::hex << bit.bits << std::dec << " must block relocation on its own";
    }
}

TEST(TexturePlacementPolicy, AnUnrelatedUsageBitDoesNotBlockRelocation)
{
    auto desc = relocatable_request();
    desc.usage = TextureUsage{0x00000008u};

    EXPECT_EQ(default_policy().place(desc, one_mb).decision, PlacementDecision::relocate_to_default);
}

TEST(TexturePlacementPolicy, EveryBlockCompressedFormatIsEligible)
{
    for (const auto format :
         {PixelFormat::D3DFMT_DXT1,
          PixelFormat::D3DFMT_DXT2,
          PixelFormat::D3DFMT_DXT3,
          PixelFormat::D3DFMT_DXT4,
          PixelFormat::D3DFMT_DXT5})
    {
        auto desc = relocatable_request();
        desc.format = format;

        EXPECT_EQ(default_policy().place(desc, one_mb).decision, PlacementDecision::relocate_to_default);
    }
}

TEST(TexturePlacementPolicy, UncompressedFormatsAreNeverRelocated)
{
    for (const auto format :
         {PixelFormat::D3DFMT_A8R8G8B8,
          PixelFormat::D3DFMT_X8R8G8B8,
          PixelFormat::D3DFMT_R5G6B5,
          PixelFormat::D3DFMT_A8,
          PixelFormat::D3DFMT_UNKNOWN})
    {
        auto desc = relocatable_request();
        desc.format = format;

        EXPECT_EQ(default_policy().place(desc, one_mb).decision, PlacementDecision::keep_requested);
    }
}

TEST(TexturePlacementPolicy, TheMinimumSizeIsInclusiveAtTheBoundary)
{
    const auto config = TexturePlacementConfig{};
    const auto policy = TexturePlacementPolicy{config};
    const auto desc = relocatable_request();

    EXPECT_EQ(policy.place(desc, config.minimum_bytes - 1u).decision, PlacementDecision::keep_requested);
    EXPECT_EQ(policy.place(desc, config.minimum_bytes).decision, PlacementDecision::relocate_to_default)
        << "a texture exactly at the threshold qualifies";
    EXPECT_EQ(policy.place(desc, config.minimum_bytes + 1u).decision, PlacementDecision::relocate_to_default);
}

TEST(TexturePlacementPolicy, ExplainsItselfForEveryOutcome)
{
    // The reason is what the overlay and the log show, so it must actually name
    // the condition that decided rather than being a generic string.
    const auto policy = default_policy();

    auto not_managed = relocatable_request();
    not_managed.pool = TexturePool::D3DPOOL_DEFAULT;

    auto dynamic_usage = relocatable_request();
    dynamic_usage.usage = lyrium_usage::dynamic;

    auto uncompressed = relocatable_request();
    uncompressed.format = PixelFormat::D3DFMT_A8R8G8B8;

    EXPECT_STREQ(policy.place(relocatable_request(), one_mb).reason, "relocated to the default pool");
    EXPECT_STREQ(policy.place(not_managed, one_mb).reason, "not a managed texture");
    EXPECT_STREQ(policy.place(dynamic_usage, one_mb).reason, "usage flags forbid relocation");
    EXPECT_STREQ(policy.place(uncompressed, one_mb).reason, "format is not block compressed");
    EXPECT_STREQ(policy.place(relocatable_request(), 1u).reason, "below the minimum size");

    const auto disabled = TexturePlacementPolicy{TexturePlacementConfig{.prefer_default = false}};
    EXPECT_STREQ(disabled.place(relocatable_request(), one_mb).reason, "disabled by configuration");
}

TEST(TexturePlacementPolicy, FallbackOnlyAppliesToARelocatedRequest)
{
    const auto policy = default_policy();

    const auto relocated = policy.place(relocatable_request(), one_mb);
    EXPECT_TRUE(policy.may_fall_back(relocated));

    auto uncompressed = relocatable_request();
    uncompressed.format = PixelFormat::D3DFMT_A8R8G8B8;
    const auto kept = policy.place(uncompressed, one_mb);
    EXPECT_FALSE(policy.may_fall_back(kept)) << "a request that kept its pool has nothing to fall back to";
}

TEST(TexturePlacementPolicy, FallbackCanBeDisabled)
{
    const auto policy = TexturePlacementPolicy{TexturePlacementConfig{.fall_back_to_managed = false}};
    const auto relocated = policy.place(relocatable_request(), one_mb);

    ASSERT_TRUE(relocated.relocated());
    EXPECT_FALSE(policy.may_fall_back(relocated));
}

// The decision is constexpr, so the hot path costs nothing it does not have to.
static_assert(default_policy().place(relocatable_request(), one_mb).relocated());
static_assert(!default_policy().place(relocatable_request(), 1u).relocated());
