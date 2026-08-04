#pragma once

#include <algorithm>
#include <cstdint>

namespace lyrium::texture
{

// Rectangle arithmetic for partial texture updates.
//
// Block-compressed formats store 4x4 pixel blocks, so a sub-rectangle can only
// be copied on block boundaries. A rect that starts mid-block has to be widened
// outward to the enclosing blocks, or the copy silently lands at the wrong offset
// and the update is placed a few pixels away from where the caller meant.
//
// Deliberately free of D3D and Windows types so the decision can be tested
// directly. The caller converts to and from RECT at the boundary.

struct RectI32
{
    std::int32_t left;
    std::int32_t top;
    std::int32_t right;
    std::int32_t bottom;

    auto operator==(const RectI32 &) const -> bool = default;
};

enum class BlockRectKind : std::uint8_t
{
    // Nothing to copy: the rect collapsed once clamped to the level.
    empty,
    // The rect spans the whole level, so the copy can skip sub-rect handling.
    covers_level,
    // A genuine sub-rectangle.
    partial,
};

struct BlockRect
{
    RectI32 rect;
    BlockRectKind kind;
};

// Widens a rect outward to enclosing 4x4 block boundaries. Widening rather than
// narrowing is what guarantees no touched block is missed.
[[nodiscard]] constexpr auto align_to_blocks(const RectI32 &rect) -> RectI32
{
    return RectI32{
        .left = rect.left & ~3,
        .top = rect.top & ~3,
        .right = (rect.right + 3) & ~3,
        .bottom = (rect.bottom + 3) & ~3,
    };
}

[[nodiscard]] constexpr auto clamp_to_extent(const RectI32 &rect, std::int32_t width, std::int32_t height) -> RectI32
{
    return RectI32{
        .left = std::max<std::int32_t>(rect.left, 0),
        .top = std::max<std::int32_t>(rect.top, 0),
        .right = std::min<std::int32_t>(rect.right, width),
        .bottom = std::min<std::int32_t>(rect.bottom, height),
    };
}

// Block-aligns when the format requires it, clamps to the level, and reports
// whether the result is empty, covers the level, or is a genuine sub-rectangle.
[[nodiscard]] constexpr auto classify_update_rect(
    const RectI32 &requested,
    std::int32_t level_width,
    std::int32_t level_height,
    bool block_compressed) -> BlockRect
{
    auto rect = block_compressed ? align_to_blocks(requested) : requested;
    rect = clamp_to_extent(rect, level_width, level_height);

    if (rect.right <= rect.left || rect.bottom <= rect.top)
    {
        return BlockRect{.rect = rect, .kind = BlockRectKind::empty};
    }

    const auto covers =
        rect.left == 0 && rect.top == 0 && rect.right >= level_width && rect.bottom >= level_height;

    return BlockRect{.rect = rect, .kind = covers ? BlockRectKind::covers_level : BlockRectKind::partial};
}

// The extent of mip level n, which never drops below one pixel.
[[nodiscard]] constexpr auto level_extent(std::uint32_t base, std::uint32_t level) -> std::int32_t
{
    const auto shifted = base >> level;
    return static_cast<std::int32_t>(shifted > 1u ? shifted : 1u);
}

}
