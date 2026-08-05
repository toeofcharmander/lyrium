#pragma once

#include <atomic>
#include <cstdint>

namespace lyrium::texture
{

// Which mip levels have been written but not yet handed to the GPU.
//
// The upload used to happen inside UnlockRect, one UpdateSurface per level, so a
// ten-level texture cost ten separate GPU copies and a scene entry writing
// hundreds of textures queued thousands of them at exactly the moment the scene
// needed the GPU. Every CPU-side timer in that path measured under half a second
// across a whole session while the stall lasted seconds, which is the signature
// of work queued for the GPU and never waited for: UpdateSurface returns as soon
// as the copy is enqueued, so timing the call measures the enqueue and not the
// work.
//
// Marking a level owes it instead. The debt is settled in one batch, at the
// point the engine actually binds the texture, which both collapses the call
// count and moves the work off the load burst to where it is genuinely needed.
//
// Atomic for the same reason LevelValidity is: levels are written from whichever
// thread the engine happens to be on, and a bit-packed non-atomic mask makes two
// distinct levels a read-modify-write race on one word.
class DirtyLevels
{
  public:
    static constexpr auto capacity = std::uint32_t{32};

    auto mark(std::uint32_t level) -> void
    {
        if (level >= capacity)
        {
            return;
        }
        mask_.fetch_or(bit(level), std::memory_order_relaxed);
    }

    // Every level holding data is owed again: used after a device reset, where
    // the GPU-side copy of everything is gone regardless of what was flushed.
    auto mark_all(std::uint32_t levels) -> void
    {
        mask_.fetch_or(levels, std::memory_order_relaxed);
    }

    [[nodiscard]] auto is_dirty(std::uint32_t level) const -> bool
    {
        return level < capacity && (mask_.load(std::memory_order_relaxed) & bit(level)) != 0u;
    }

    [[nodiscard]] auto any() const -> bool
    {
        return mask_.load(std::memory_order_relaxed) != 0u;
    }

    // Hands out the debt and clears it in one step. A write landing after this
    // returns is owed again rather than lost, which is why it exchanges rather
    // than reading and then clearing.
    [[nodiscard]] auto take() -> std::uint32_t
    {
        return mask_.exchange(0u, std::memory_order_relaxed);
    }

  private:
    static constexpr auto bit(std::uint32_t level) -> std::uint32_t
    {
        return std::uint32_t{1} << level;
    }

    std::atomic<std::uint32_t> mask_{0u};
};

}
