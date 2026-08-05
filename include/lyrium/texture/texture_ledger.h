#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "lyrium/texture/texture_desc.h"

namespace lyrium::texture
{

// An opaque stand-in for IDirect3DTexture9* so this header never names a D3D
// type and can be tested on Linux.
using TextureHandle = const void *;

struct TextureTotals
{
    // One bucket per pool, plus a final bucket for any value outside the four
    // known pools. Sizing it this way is what makes the sum of the buckets always
    // equal to total, which the counters this replaces could not promise.
    std::array<std::uint64_t, 5u> by_pool{};
    std::uint64_t total{};
    std::uint64_t peak{};
    std::uint64_t live_count{};
    std::uint64_t released_bytes{};

    // Currently-live bytes that the placement policy moved out of the managed
    // pool. This is the saving as it stands right now; the cumulative counter in
    // stats.h never decrements and so overstates it once textures retire.
    std::uint64_t relocated_bytes{};
    std::uint64_t relocated_count{};

    [[nodiscard]] auto pool_sum() const -> std::uint64_t
    {
        std::uint64_t sum = 0u;
        for (const auto bytes : by_pool)
        {
            sum += bytes;
        }
        return sum;
    }
};

// The single owner of texture byte and count accounting.
//
// Three parallel systems used to be updated from one place and disagreed,
// because they were decremented at different points: a texture retained by the
// recycler was removed from one and left in the others, so the byte totals
// ratcheted upward for the life of the process.
//
// The fix here is structural rather than another bounds check. Every counter is
// derived from an insert or an erase on one record map, so an unmatched release
// is a no-op and a double count is unrepresentable. Peak and released bytes are
// the only cumulative values, and they only ever move in one direction.
//
// Templated on the allocator so the DLL can bind it to the private heap while
// tests bind it to std::allocator. The rule that this code never allocates from
// the game's CRT heap is preserved by the instantiation, not by this header.
template <template <class> class Alloc = std::allocator>
class BasicTextureLedger
{
  public:
    BasicTextureLedger() = default;

    BasicTextureLedger(const BasicTextureLedger &) = delete;
    auto operator=(const BasicTextureLedger &) -> BasicTextureLedger & = delete;

    // Records a live texture. Re-registering a handle that is already live
    // replaces the previous record rather than adding to it, so a caller that
    // reports the same texture twice cannot inflate the totals.
    auto note_created(TextureHandle texture, TexturePool pool, std::uint64_t bytes, bool relocated = false) -> void
    {
        if (texture == nullptr)
        {
            return;
        }

        const auto lock = std::scoped_lock{mutex_};

        if (const auto existing = records_.find(texture); existing != records_.end())
        {
            withdraw(existing->second);
            records_.erase(existing);
        }

        const auto record = Record{.pool = pool, .bytes = bytes, .relocated = relocated};
        records_.emplace(texture, record);
        deposit(record);
    }

    // Retires a texture. An unknown handle is a no-op, which is the property the
    // old counters lacked: releasing something that was never created used to
    // wrap the unsigned total to near its maximum.
    auto note_destroyed(TextureHandle texture) -> bool
    {
        if (texture == nullptr)
        {
            return false;
        }

        const auto lock = std::scoped_lock{mutex_};

        const auto found = records_.find(texture);
        if (found == records_.end())
        {
            return false;
        }

        withdraw(found->second);
        released_bytes_ += found->second.bytes;
        records_.erase(found);
        return true;
    }

    [[nodiscard]] auto is_tracked(TextureHandle texture) const -> bool
    {
        const auto lock = std::scoped_lock{mutex_};
        return records_.contains(texture);
    }

    // Non-blocking read. Used on the exit path, where a lock held by a thread
    // ExitProcess has already terminated would never be released. Also better on
    // the normal path: the sampler thread no longer stalls behind a render-thread
    // texture create.
    [[nodiscard]] auto try_totals(TextureTotals &out) const -> bool
    {
        const auto lock = std::unique_lock{mutex_, std::try_to_lock};
        if (!lock.owns_lock())
        {
            return false;
        }
        out = snapshot_locked();
        return true;
    }

    [[nodiscard]] auto totals() const -> TextureTotals
    {
        const auto lock = std::scoped_lock{mutex_};
        return snapshot_locked();
    }

  private:
    [[nodiscard]] auto snapshot_locked() const -> TextureTotals
    {
        auto out = TextureTotals{};
        out.by_pool = by_pool_;
        out.total = total_;
        out.peak = peak_;
        out.live_count = records_.size();
        out.released_bytes = released_bytes_;
        out.relocated_bytes = relocated_bytes_;
        out.relocated_count = relocated_count_;
        return out;
    }

    struct Record
    {
        TexturePool pool{};
        std::uint64_t bytes{};
        bool relocated{};
    };

    using MapAllocator = Alloc<std::pair<const TextureHandle, Record>>;
    using Map =
        std::unordered_map<TextureHandle, Record, std::hash<TextureHandle>, std::equal_to<TextureHandle>, MapAllocator>;

    static constexpr auto unknown_bucket = std::size_t{4};

    static constexpr auto bucket_for(TexturePool pool) -> std::size_t
    {
        const auto index = static_cast<std::size_t>(pool);
        return index < unknown_bucket ? index : unknown_bucket;
    }

    // Both of these run under the lock. They are the only places any counter
    // moves, which is what keeps the buckets and the total in agreement.
    auto deposit(const Record &record) -> void
    {
        by_pool_[bucket_for(record.pool)] += record.bytes;
        total_ += record.bytes;
        if (record.relocated)
        {
            relocated_bytes_ += record.bytes;
            ++relocated_count_;
        }
        if (total_ > peak_)
        {
            peak_ = total_;
        }
    }

    auto withdraw(const Record &record) -> void
    {
        by_pool_[bucket_for(record.pool)] -= record.bytes;
        total_ -= record.bytes;
        if (record.relocated)
        {
            relocated_bytes_ -= record.bytes;
            --relocated_count_;
        }
    }

    mutable std::mutex mutex_{};
    Map records_{};
    std::array<std::uint64_t, 5u> by_pool_{};
    std::uint64_t total_{};
    std::uint64_t peak_{};
    std::uint64_t released_bytes_{};
    std::uint64_t relocated_bytes_{};
    std::uint64_t relocated_count_{};
};

using TextureLedger = BasicTextureLedger<std::allocator>;

}
