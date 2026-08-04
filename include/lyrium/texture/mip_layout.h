#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lyrium::texture
{

// Where each mip level sits inside the shadow copy of a relocated texture.
//
// This is the highest-consequence arithmetic in the project and had no coverage
// at all. The accumulated size becomes the length of a CreateFileMappingW
// section, and every offset is used to compute a pointer handed back to the
// engine from LockRect. An error here is not a wrong number on a diagnostic
// line, it is the engine writing texture data outside the mapping or into the
// wrong level.
//
// Deliberately free of D3D and Windows types so it can be tested directly; the
// caller keeps the CreateFileMappingW call and the SIZE_T overflow check.

struct MipLevel
{
    std::uint32_t width{};
    std::uint32_t height{};

    // Bytes per row of blocks.
    std::uint32_t pitch{};

    // Rows of blocks, not rows of pixels.
    std::uint32_t rows{};

    // Byte offset of this level from the start of the mapping.
    std::size_t offset{};

    auto operator==(const MipLevel &) const -> bool = default;

    [[nodiscard]] constexpr auto bytes() const -> std::size_t
    {
        return static_cast<std::size_t>(pitch) * rows;
    }
};

struct MipLayout
{
    std::vector<MipLevel> levels{};
    std::size_t total_bytes{};
};

// Block-compressed formats store 4x4 pixel blocks, so a level's storage is
// measured in blocks rounded up, never in pixels. That rounding is why a 1x1
// level still costs a whole block and why a mip chain does not shrink to
// nothing.
inline constexpr auto block_extent = std::uint32_t{4};

[[nodiscard]] constexpr auto blocks_for(std::uint32_t pixels) -> std::uint32_t
{
    return (std::max(pixels, 1u) + block_extent - 1u) / block_extent;
}

// Appends each level to a caller-supplied container and returns the total size.
//
// The container is the caller's because the DLL keeps these on its own private
// heap: returning a std::vector here would allocate from the game's CRT heap,
// which this project must never do. Tests pass a plain std::vector.
//
// block_bytes is 8 for DXT1 and 16 for the others; the caller derives it from
// the format because the format tables live elsewhere.
template <class Levels>
[[nodiscard]] auto append_mip_levels(
    Levels &out,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t levels,
    std::uint32_t block_bytes) -> std::size_t
{
    out.reserve(levels);

    auto level_width = std::max(width, 1u);
    auto level_height = std::max(height, 1u);
    auto offset = std::size_t{};

    for (auto level = std::uint32_t{}; level < levels; ++level)
    {
        const auto pitch = blocks_for(level_width) * block_bytes;
        const auto rows = blocks_for(level_height);

        out.push_back(MipLevel{
            .width = level_width,
            .height = level_height,
            .pitch = pitch,
            .rows = rows,
            .offset = offset,
        });

        offset += static_cast<std::size_t>(pitch) * rows;
        level_width = std::max(level_width / 2u, 1u);
        level_height = std::max(level_height / 2u, 1u);
    }

    return offset;
}

// Convenience wrapper for tests and any caller that does not care which heap the
// levels land on.
[[nodiscard]] inline auto build_mip_layout(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t levels,
    std::uint32_t block_bytes) -> MipLayout
{
    auto layout = MipLayout{};
    layout.total_bytes = append_mip_levels(layout.levels, width, height, levels, block_bytes);
    return layout;
}

}
