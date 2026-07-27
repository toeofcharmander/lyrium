#pragma once

#include <algorithm>
#include <cstdint>

#include <d3d9.h>

namespace eluvian::diag
{

inline auto format_bits_per_pixel(::D3DFORMAT format) -> std::uint32_t
{
    switch (static_cast<std::uint32_t>(format))
    {
        case D3DFMT_A32B32G32R32F: return 128u;
        case D3DFMT_A16B16G16R16:
        case D3DFMT_A16B16G16R16F:
        case D3DFMT_Q16W16V16U16:
        case D3DFMT_G32R32F: return 64u;
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8:
        case D3DFMT_A2B10G10R10:
        case D3DFMT_A2R10G10B10:
        case D3DFMT_G16R16:
        case D3DFMT_Q8W8V8U8:
        case D3DFMT_V16U16:
        case D3DFMT_D32:
        case D3DFMT_D24S8:
        case D3DFMT_D24X8:
        case D3DFMT_G16R16F:
        case D3DFMT_R32F: return 32u;
        case D3DFMT_R8G8B8: return 24u;
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
        case D3DFMT_A8L8:
        case D3DFMT_V8U8:
        case D3DFMT_L16:
        case D3DFMT_D16:
        case D3DFMT_D15S1:
        case D3DFMT_R16F: return 16u;
        case D3DFMT_A8:
        case D3DFMT_L8:
        case D3DFMT_P8:
        case D3DFMT_A4L4: return 8u;
        case D3DFMT_DXT1: return 4u;
        case D3DFMT_DXT2:
        case D3DFMT_DXT3:
        case D3DFMT_DXT4:
        case D3DFMT_DXT5: return 8u;
        default: return 32u;
    }
}

inline auto is_block_compressed(::D3DFORMAT format) -> bool
{
    switch (static_cast<std::uint32_t>(format))
    {
        case D3DFMT_DXT1:
        case D3DFMT_DXT2:
        case D3DFMT_DXT3:
        case D3DFMT_DXT4:
        case D3DFMT_DXT5: return true;
        default: return false;
    }
}

inline auto level_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t depth, ::D3DFORMAT format)
    -> std::uint64_t
{
    const auto bits = format_bits_per_pixel(format);

    if (is_block_compressed(format))
    {
        const auto blocks_x = (std::max<std::uint32_t>(width, 1u) + 3u) / 4u;
        const auto blocks_y = (std::max<std::uint32_t>(height, 1u) + 3u) / 4u;
        const auto block_bytes = bits == 4u ? 8u : 16u;
        return static_cast<std::uint64_t>(blocks_x) * blocks_y * std::max<std::uint32_t>(depth, 1u) * block_bytes;
    }

    return (static_cast<std::uint64_t>(width) * height * std::max<std::uint32_t>(depth, 1u) * bits + 7u) / 8u;
}

inline auto texture_bytes(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t depth,
    std::uint32_t levels,
    ::D3DFORMAT format,
    std::uint32_t faces = 1u) -> std::uint64_t
{
    auto total = std::uint64_t{};
    auto w = std::max<std::uint32_t>(width, 1u);
    auto h = std::max<std::uint32_t>(height, 1u);
    auto d = std::max<std::uint32_t>(depth, 1u);

    auto remaining = levels;
    if (remaining == 0u)
    {
        remaining = 1u;
        auto largest = std::max({w, h, d});
        while (largest > 1u)
        {
            largest /= 2u;
            ++remaining;
        }
    }

    for (auto level = std::uint32_t{0}; level < remaining; ++level)
    {
        total += level_bytes(w, h, d, format);
        w = std::max<std::uint32_t>(w / 2u, 1u);
        h = std::max<std::uint32_t>(h / 2u, 1u);
        d = std::max<std::uint32_t>(d / 2u, 1u);
    }

    return total * std::max<std::uint32_t>(faces, 1u);
}

inline auto pool_name(::D3DPOOL pool) -> const char *
{
    switch (static_cast<std::uint32_t>(pool))
    {
        case D3DPOOL_DEFAULT: return "DEFAULT";
        case D3DPOOL_MANAGED: return "MANAGED";
        case D3DPOOL_SYSTEMMEM: return "SYSTEMMEM";
        case D3DPOOL_SCRATCH: return "SCRATCH";
        default: return "UNKNOWN";
    }
}

}
