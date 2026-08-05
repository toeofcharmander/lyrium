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
// Time spent inside the driver's own CreateTexture, split by the pool the
// texture ended up in. The two cost radically different things: MANAGED
// allocates system memory and returns, with the runtime uploading lazily and
// free to evict back out under pressure, while DEFAULT must allocate real video
// memory synchronously and can never be paged out. Every other timer in this
// path came back too small to explain the cutscene stall, and this call -- the
// one thing that changes when relocation is switched off -- was never measured.
inline std::atomic<std::uint64_t> create_default_calls{};
inline std::atomic<std::uint64_t> create_default_us{};
inline std::atomic<std::uint64_t> create_other_calls{};
inline std::atomic<std::uint64_t> create_other_us{};
inline std::atomic<std::uint64_t> create_slowest_us{};

// The lock path around the upload. Every LockRect maps the whole mip-chain
// section and every UnlockRect tears it down again, so this is per-lock kernel
// page-table work that upload_us never covered. Measured because the upload
// figures came back far too small to explain the stall they were meant to.
inline std::atomic<std::uint64_t> locks{};
inline std::atomic<std::uint64_t> map_us{};
inline std::atomic<std::uint64_t> unmap_us{};
inline std::atomic<std::uint64_t> mapping_creates{};
inline std::atomic<std::uint64_t> mapping_create_us{};

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
