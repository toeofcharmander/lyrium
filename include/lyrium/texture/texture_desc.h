#pragma once

#include <compare>
#include <cstdint>

#include "lyrium/texture/pixel_format_values.inl"

namespace lyrium::texture
{

// The vocabulary the policy layer speaks.
//
// These are numerically the D3D9 values, but nothing here names a D3D or Windows
// type, which is the whole point: a policy expressed in these terms can be
// compiled and tested on Linux against fakes, while the D3D adapters convert at
// the boundary. The Windows build static_asserts every value against the genuine
// <d3d9.h>, so the correspondence is proved rather than assumed.

#define LYRIUM_PIXEL_FORMAT_ENUMERATOR(name, value) name = (value),

enum class PixelFormat : std::uint32_t
{
    LYRIUM_D3D9_FORMAT_LIST(LYRIUM_PIXEL_FORMAT_ENUMERATOR)
};

enum class TexturePool : std::uint32_t
{
    LYRIUM_D3D9_POOL_LIST(LYRIUM_PIXEL_FORMAT_ENUMERATOR)
};

#undef LYRIUM_PIXEL_FORMAT_ENUMERATOR

// Usage flags, as a type rather than a bare uint32 so a caller cannot pass a
// width where a usage was meant.
struct TextureUsage
{
    std::uint32_t bits{};

    // Ordered, not just equality: TextureDesc defaults its own <=> and that
    // requires every member to provide one.
    auto operator<=>(const TextureUsage &) const = default;

    [[nodiscard]] constexpr auto has_any(TextureUsage other) const -> bool
    {
        return (bits & other.bits) != 0u;
    }
};

namespace usage
{
// D3DUSAGE_* values. Only the ones the placement policy actually consults.
inline constexpr auto none = TextureUsage{0x00000000u};
inline constexpr auto render_target = TextureUsage{0x00000001u};
inline constexpr auto depth_stencil = TextureUsage{0x00000002u};
inline constexpr auto dynamic = TextureUsage{0x00000200u};
inline constexpr auto autogen_mipmap = TextureUsage{0x00000400u};

// The set that disqualifies a texture from being relocated: a render target or
// depth stencil must stay where the driver put it, a dynamic texture is locked
// too often to pay for staging, and an auto-generated mip chain is produced by
// the driver rather than uploaded.
inline constexpr auto not_relocatable =
    TextureUsage{render_target.bits | depth_stencil.bits | dynamic.bits | autogen_mipmap.bits};
}

[[nodiscard]] constexpr auto is_block_compressed(PixelFormat format) -> bool
{
    return format == PixelFormat::D3DFMT_DXT1 || format == PixelFormat::D3DFMT_DXT2 ||
           format == PixelFormat::D3DFMT_DXT3 || format == PixelFormat::D3DFMT_DXT4 ||
           format == PixelFormat::D3DFMT_DXT5;
}

[[nodiscard]] constexpr auto pool_name(TexturePool pool) -> const char *
{
    switch (pool)
    {
        case TexturePool::D3DPOOL_DEFAULT: return "DEFAULT";
        case TexturePool::D3DPOOL_MANAGED: return "MANAGED";
        case TexturePool::D3DPOOL_SYSTEMMEM: return "SYSTEMMEM";
        case TexturePool::D3DPOOL_SCRATCH: return "SCRATCH";
    }
    return "UNKNOWN";
}

// Everything needed to describe a texture creation request. Ordered comparison
// is defaulted so this doubles as the recycler's shape key, which retires the
// separate Key struct that duplicated these same fields.
struct TextureDesc
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{1u};
    std::uint32_t levels{};
    TextureUsage usage{};
    PixelFormat format{PixelFormat::D3DFMT_UNKNOWN};
    TexturePool pool{TexturePool::D3DPOOL_DEFAULT};

    auto operator<=>(const TextureDesc &) const = default;

    [[nodiscard]] constexpr auto is_block_compressed() const -> bool
    {
        return texture::is_block_compressed(format);
    }

    [[nodiscard]] constexpr auto is_relocatable() const -> bool
    {
        return !usage.has_any(usage::not_relocatable);
    }
};

}
