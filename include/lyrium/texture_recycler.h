#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include <d3d9.h>

#include "lyrium/containers/map.h"
#include "lyrium/containers/vector.h"

namespace lyrium
{

class TextureRecycler
{
  public:
    struct Key
    {
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t levels;
        std::uint32_t usage;
        std::uint32_t format;
        std::uint32_t pool;

        auto operator<=>(const Key &) const = default;
    };

    static auto instance() -> TextureRecycler &
    {
        static auto recycler = TextureRecycler{};
        return recycler;
    }

    auto configure(bool enabled, std::uint64_t budget_bytes, std::uint32_t max_per_key) -> void
    {
        auto lock = std::scoped_lock{mutex_};
        enabled_ = enabled;
        budget_bytes_ = budget_bytes;
        max_per_key_ = max_per_key;
    }

    auto enabled() const -> bool
    {
        return enabled_;
    }

    auto acquire(const Key &key) -> ::IDirect3DTexture9 *
    {
        if (!enabled_)
        {
            return nullptr;
        }

        auto lock = std::scoped_lock{mutex_};
        const auto found = idle_.find(key);
        if (found == idle_.end() || found->second.empty())
        {
            ++misses_;
            return nullptr;
        }

        auto entry = found->second.back();
        found->second.pop_back();
        if (found->second.empty())
        {
            idle_.erase(found);
        }

        idle_bytes_ -= entry.bytes;
        --idle_count_;
        ++hits_;
        reused_bytes_ += entry.bytes;

        live_[entry.texture] = LiveEntry{.key = key, .bytes = entry.bytes};
        return entry.texture;
    }

    auto note_created(::IDirect3DTexture9 *texture, const Key &key, std::uint64_t bytes) -> void
    {
        if (texture == nullptr)
        {
            return;
        }
        auto lock = std::scoped_lock{mutex_};
        live_[texture] = LiveEntry{.key = key, .bytes = bytes};
    }

    auto retain(::IDirect3DTexture9 *texture) -> bool
    {
        if (!enabled_ || texture == nullptr)
        {
            return false;
        }

        auto lock = std::scoped_lock{mutex_};
        const auto found = live_.find(texture);
        if (found == live_.end())
        {
            return false;
        }

        const auto key = found->second.key;
        const auto bytes = found->second.bytes;

        if (bytes == 0u || idle_bytes_ + bytes > budget_bytes_)
        {
            ++rejected_budget_;
            live_.erase(found);
            return false;
        }
        auto &bucket = idle_[key];
        if (bucket.size() >= max_per_key_)
        {
            ++rejected_per_key_;
            live_.erase(found);
            return false;
        }

        bucket.push_back(IdleEntry{.texture = texture, .bytes = bytes});
        idle_bytes_ += bytes;
        ++idle_count_;
        ++retained_;
        live_.erase(found);
        return true;
    }

    auto forget(::IDirect3DTexture9 *texture) -> void
    {
        auto lock = std::scoped_lock{mutex_};
        live_.erase(texture);
    }

    auto purge() -> std::uint32_t
    {
        auto victims = Vector<::IDirect3DTexture9 *>{};
        {
            auto lock = std::scoped_lock{mutex_};
            for (auto &[key, bucket] : idle_)
            {
                for (auto &entry : bucket)
                {
                    victims.push_back(entry.texture);
                }
            }
            idle_.clear();
            idle_bytes_ = 0u;
            idle_count_ = 0u;
        }

        for (auto *texture : victims)
        {
            texture->Release();
        }
        return static_cast<std::uint32_t>(victims.size());
    }

    struct Stats
    {
        std::uint64_t hits;
        std::uint64_t misses;
        std::uint64_t retained;
        std::uint64_t reused_bytes;
        std::uint64_t idle_bytes;
        std::uint64_t idle_count;
        std::uint64_t rejected_budget;
        std::uint64_t rejected_per_key;
    };

    auto stats() -> Stats
    {
        auto lock = std::scoped_lock{mutex_};
        return Stats{
            .hits = hits_,
            .misses = misses_,
            .retained = retained_,
            .reused_bytes = reused_bytes_,
            .idle_bytes = idle_bytes_,
            .idle_count = idle_count_,
            .rejected_budget = rejected_budget_,
            .rejected_per_key = rejected_per_key_,
        };
    }

  private:
    struct IdleEntry
    {
        ::IDirect3DTexture9 *texture;
        std::uint64_t bytes;
    };

    struct LiveEntry
    {
        Key key;
        std::uint64_t bytes;
    };

    TextureRecycler() = default;

    std::mutex mutex_;
    Map<Key, Vector<IdleEntry>> idle_;
    Map<::IDirect3DTexture9 *, LiveEntry> live_;

    std::uint64_t idle_bytes_{0};
    std::uint64_t idle_count_{0};
    std::uint64_t hits_{0};
    std::uint64_t misses_{0};
    std::uint64_t retained_{0};
    std::uint64_t reused_bytes_{0};
    std::uint64_t rejected_budget_{0};
    std::uint64_t rejected_per_key_{0};

    std::atomic<bool> enabled_{false};
    std::uint64_t budget_bytes_{96ull * 1024ull * 1024ull};
    std::uint32_t max_per_key_{8};
};

}
