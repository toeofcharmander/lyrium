#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lyrium::texture
{

// Reuse of SYSTEMMEM staging textures between uploads.
//
// Every write to a relocated texture uploads through a staging texture, and the
// first implementation created and destroyed one per mip level per unlock. A
// cutscene entry streams dozens of textures at once, each with a full mip chain,
// so hundreds of driver resource creations serialized at exactly the moment the
// scene needed the GPU -- which played as a black stall while textures trickled
// in. Disabling relocation removed the stall and pinned it to this path; the
// uploads themselves are unavoidable, the churn is not.
//
// The trade is explicit: a pooled staging texture holds the game's address
// space permanently, which is the resource this project exists to protect. The
// byte budget is the ceiling on that cost, and it is deliberately small. The
// churn it removes was itself allocating and freeing SYSTEMMEM through the
// driver on every upload, so the pool also removes a source of the very
// fragmentation it spends its budget on.
//
// Policy only: handles are opaque, nothing here names a D3D or Windows type,
// and the caller owns all locking. The DLL wraps this in a mutex and stores
// IDirect3DTexture9 pointers; the tests store ints.

struct StagingShape
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t format{};

    [[nodiscard]] constexpr auto operator==(const StagingShape &) const -> bool = default;
};

template <class Handle, template <class> class Alloc = std::allocator>
class BasicStagingPool
{
  public:
    BasicStagingPool(std::uint64_t budget_bytes, std::size_t max_entries)
        : budget_bytes_{budget_bytes}
        , max_entries_{max_entries}
    {
    }

    // A staging texture is only substitutable for an identical shape, so this
    // returns an exact match or nothing. Ownership transfers to the caller.
    [[nodiscard]] auto acquire(const StagingShape &shape) -> std::optional<Handle>
    {
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
        {
            if (it->shape == shape)
            {
                auto handle = it->handle;
                held_bytes_ -= it->bytes;
                *it = entries_.back();
                entries_.pop_back();
                return handle;
            }
        }
        return std::nullopt;
    }

    // Offers a no-longer-needed staging texture back. Returns false when the
    // budget or the entry cap would be exceeded, in which case the caller must
    // destroy the texture -- refusing is the mechanism that bounds the pool's
    // permanent address-space cost.
    [[nodiscard]] auto offer(const StagingShape &shape, std::uint64_t bytes, Handle handle) -> bool
    {
        if (entries_.size() >= max_entries_ || held_bytes_ + bytes > budget_bytes_)
        {
            return false;
        }
        entries_.push_back(Entry{.shape = shape, .bytes = bytes, .handle = handle});
        held_bytes_ += bytes;
        return true;
    }

    [[nodiscard]] auto held_bytes() const -> std::uint64_t
    {
        return held_bytes_;
    }

    [[nodiscard]] auto size() const -> std::size_t
    {
        return entries_.size();
    }

  private:
    struct Entry
    {
        StagingShape shape{};
        std::uint64_t bytes{};
        Handle handle{};
    };

    std::uint64_t budget_bytes_;
    std::size_t max_entries_;
    std::uint64_t held_bytes_{};
    std::vector<Entry, Alloc<Entry>> entries_{};
};

}
