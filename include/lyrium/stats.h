#pragma once

#include <atomic>
#include <cstdint>

#include "lyrium/resource_tracker.h"

namespace lyrium::stats
{

inline std::atomic<std::uint64_t> d3d_creates{};
inline std::atomic<std::uint64_t> d3d_create_failures{};

// The staged upload path: how often a relocated texture's write reached the
// GPU, how long that took in total, and whether the staging texture was reused
// or created through the driver. reused vs created is the cutscene-stall
// metric: every created is a synchronous driver allocation on the load path.
inline std::atomic<std::uint64_t> uploads{};
inline std::atomic<std::uint64_t> upload_us{};
inline std::atomic<std::uint64_t> staging_created{};
inline std::atomic<std::uint64_t> staging_reused{};

inline std::atomic<std::uint64_t> pool_overrides{};
inline std::atomic<std::uint64_t> pool_override_bytes{};
inline std::atomic<std::uint64_t> pool_reverts{};

inline std::atomic<std::uint64_t> rescue_attempts{};
inline std::atomic<std::uint64_t> rescue_successes{};
inline std::atomic<std::uint64_t> rescue_preemptive{};

inline ResourceTracker<void *> live_vertex_buffers{};
inline ResourceTracker<void *> live_index_buffers{};
inline ResourceTracker<void *> live_state_blocks{};

}
