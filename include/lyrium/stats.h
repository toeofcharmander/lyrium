#pragma once

#include <atomic>
#include <cstdint>

#include "lyrium/resource_tracker.h"

namespace lyrium::stats
{

inline std::atomic<std::uint64_t> texture_bytes_by_pool[4]{};
inline std::atomic<std::uint64_t> texture_bytes_live{};

inline std::atomic<std::uint64_t> d3d_creates{};
inline std::atomic<std::uint64_t> d3d_create_failures{};

inline std::atomic<std::uint64_t> pool_overrides{};
inline std::atomic<std::uint64_t> pool_override_bytes{};
inline std::atomic<std::uint64_t> pool_reverts{};

inline std::atomic<std::uint64_t> rescue_attempts{};
inline std::atomic<std::uint64_t> rescue_successes{};
inline std::atomic<std::uint64_t> rescue_preemptive{};

inline ResourceTracker<void *> live_textures{};
inline ResourceTracker<void *> live_vertex_buffers{};
inline ResourceTracker<void *> live_index_buffers{};
inline ResourceTracker<void *> live_state_blocks{};

}
