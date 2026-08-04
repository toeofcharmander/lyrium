#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace lyrium::diag
{

namespace detail
{
inline std::array<std::atomic<std::uint64_t>, 4u> texture_bytes_pool{};
inline std::atomic<std::uint64_t> texture_bytes_total{};
inline std::atomic<std::uint64_t> texture_bytes_peak{};
inline std::atomic<std::uint64_t> texture_count_live{};
inline std::atomic<std::uint64_t> texture_bytes_released{};
}

inline auto note_texture_created(std::uint32_t pool, std::uint64_t bytes) -> void
{
    if (pool < detail::texture_bytes_pool.size())
    {
        detail::texture_bytes_pool[pool].fetch_add(bytes, std::memory_order_relaxed);
    }
    const auto total = detail::texture_bytes_total.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    detail::texture_count_live.fetch_add(1u, std::memory_order_relaxed);

    auto peak = detail::texture_bytes_peak.load(std::memory_order_relaxed);
    while (total > peak &&
           !detail::texture_bytes_peak.compare_exchange_weak(peak, total, std::memory_order_relaxed))
    {
    }
}

inline auto note_texture_released(std::uint32_t pool, std::uint64_t bytes) -> void
{
    if (pool < detail::texture_bytes_pool.size())
    {
        detail::texture_bytes_pool[pool].fetch_sub(bytes, std::memory_order_relaxed);
    }
    detail::texture_bytes_total.fetch_sub(bytes, std::memory_order_relaxed);
    detail::texture_bytes_released.fetch_add(bytes, std::memory_order_relaxed);
    detail::texture_count_live.fetch_sub(1u, std::memory_order_relaxed);
}

auto texture_pool_overrides() -> std::uint64_t;
auto texture_pool_override_bytes() -> std::uint64_t;
auto texture_pool_reverts() -> std::uint64_t;

struct TextureTotals
{
    std::uint64_t by_pool[4];
    std::uint64_t total;
    std::uint64_t peak;
    std::uint64_t live_count;
    std::uint64_t released_bytes;
};

inline auto texture_totals() -> TextureTotals
{
    auto out = TextureTotals{};
    for (auto i = std::size_t{0}; i < detail::texture_bytes_pool.size(); ++i)
    {
        out.by_pool[i] = detail::texture_bytes_pool[i].load(std::memory_order_relaxed);
    }
    out.total = detail::texture_bytes_total.load(std::memory_order_relaxed);
    out.peak = detail::texture_bytes_peak.load(std::memory_order_relaxed);
    out.live_count = detail::texture_count_live.load(std::memory_order_relaxed);
    out.released_bytes = detail::texture_bytes_released.load(std::memory_order_relaxed);
    return out;
}

}
