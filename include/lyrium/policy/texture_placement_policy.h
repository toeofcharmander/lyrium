#pragma once

#include <cstdint>

#include "lyrium/texture/texture_desc.h"

namespace lyrium::policy
{

// Decides which pool a texture creation request should actually use.
//
// This is the core of the mod. Dragon Age has a 2 GB address space, and a
// MANAGED texture keeps a full CPU-side duplicate inside it. Steering the large,
// block-compressed textures to DEFAULT removes that duplicate, which is what buys
// back the contiguous space the game runs out of.
//
// It is a pure function of the request and the configuration, with no D3D or
// Windows types anywhere, so the whole decision table can be tested directly.
// Previously this lived inline inside the CreateTexture hook, reachable only
// through four globals.

struct TexturePlacementConfig
{
    // Master switch. With this off every request keeps the pool it asked for.
    bool prefer_default{true};

    // Below this size the duplicate is not worth the staging machinery.
    std::uint64_t minimum_bytes{256ull * 1024ull};

    // Whether a failed DEFAULT creation may be retried in MANAGED.
    bool fall_back_to_managed{true};
};

enum class PlacementDecision : std::uint8_t
{
    keep_requested,
    relocate_to_default,
};

struct Placement
{
    texture::TexturePool pool{};
    PlacementDecision decision{PlacementDecision::keep_requested};

    // Why, in a form suitable for the overlay and the log. Always a literal.
    const char *reason{""};

    [[nodiscard]] constexpr auto relocated() const -> bool
    {
        return decision == PlacementDecision::relocate_to_default;
    }
};

class TexturePlacementPolicy
{
  public:
    explicit constexpr TexturePlacementPolicy(const TexturePlacementConfig &config)
        : config_{config}
    {
    }

    // bytes is passed rather than derived so the caller can use whatever sizing
    // it already computed, and so this stays independent of the format tables.
    [[nodiscard]] constexpr auto place(const texture::TextureDesc &desc, std::uint64_t bytes) const -> Placement
    {
        const auto keep = [&desc](const char *reason) {
            return Placement{.pool = desc.pool, .decision = PlacementDecision::keep_requested, .reason = reason};
        };

        if (!config_.prefer_default)
        {
            return keep("disabled by configuration");
        }

        // Only MANAGED carries the CPU-side duplicate this exists to remove.
        // DEFAULT is already where we want it, and SYSTEMMEM and SCRATCH are
        // staging pools the engine is using deliberately.
        if (desc.pool != texture::TexturePool::D3DPOOL_MANAGED)
        {
            return keep("not a managed texture");
        }

        // A render target, depth stencil, dynamic or auto-mipmapped texture must
        // stay put: the first two belong to the driver, the third is locked far
        // too often to pay for staging, and the fourth has its chain generated
        // rather than uploaded.
        if (!desc.is_relocatable())
        {
            return keep("usage flags forbid relocation");
        }

        // Only block-compressed formats are relocated. Uncompressed surfaces are
        // the ones the engine tends to lock and stream, and relocating them is
        // what broke video playback when this was first attempted.
        if (!desc.is_block_compressed())
        {
            return keep("format is not block compressed");
        }

        if (bytes < config_.minimum_bytes)
        {
            return keep("below the minimum size");
        }

        return Placement{
            .pool = texture::TexturePool::D3DPOOL_DEFAULT,
            .decision = PlacementDecision::relocate_to_default,
            .reason = "relocated to the default pool",
        };
    }

    // Whether a failed creation at the placed pool may be retried as MANAGED.
    // Only meaningful for a request that was actually relocated: one that kept
    // its pool has nothing to fall back to.
    [[nodiscard]] constexpr auto may_fall_back(const Placement &placement) const -> bool
    {
        return config_.fall_back_to_managed && placement.relocated();
    }

    [[nodiscard]] constexpr auto config() const -> const TexturePlacementConfig &
    {
        return config_;
    }

  private:
    TexturePlacementConfig config_;
};

}
