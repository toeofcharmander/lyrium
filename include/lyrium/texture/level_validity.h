#pragma once

#include <atomic>
#include <cstdint>

namespace lyrium::texture
{

// Tracks which mip levels hold data worth re-uploading after a device reset.
//
// This replaces a Vector<bool>, which is bit-packed: several levels share one
// underlying word, so marking level 3 and level 4 from different threads is a
// read-modify-write race on the same byte even though the levels are distinct.
// The write also happened outside the lock that guarded everything else about a
// level, so nothing serialised it.
//
// An atomic bitmask makes that unrepresentable rather than merely guarded. A
// texture cannot have more than 32 mip levels in practice -- a 32768 pixel
// square, larger than any hardware of the era supports, has 16 -- so one word is
// enough, and a level beyond that is clamped rather than allowed to corrupt a
// neighbouring bit.
class LevelValidity
{
  public:
    static constexpr auto capacity = std::uint32_t{32};

    auto mark_valid(std::uint32_t level) -> void
    {
        if (level >= capacity)
        {
            return;
        }
        bits_.fetch_or(bit(level), std::memory_order_release);
    }

    auto mark_invalid(std::uint32_t level) -> void
    {
        if (level >= capacity)
        {
            return;
        }
        bits_.fetch_and(static_cast<std::uint32_t>(~bit(level)), std::memory_order_release);
    }

    [[nodiscard]] auto is_valid(std::uint32_t level) const -> bool
    {
        if (level >= capacity)
        {
            return false;
        }
        return (bits_.load(std::memory_order_acquire) & bit(level)) != 0u;
    }

    auto clear() -> void
    {
        bits_.store(0u, std::memory_order_release);
    }

    [[nodiscard]] auto any_valid() const -> bool
    {
        return bits_.load(std::memory_order_acquire) != 0u;
    }

  private:
    static constexpr auto bit(std::uint32_t level) -> std::uint32_t
    {
        return std::uint32_t{1} << level;
    }

    std::atomic<std::uint32_t> bits_{};
};

}
