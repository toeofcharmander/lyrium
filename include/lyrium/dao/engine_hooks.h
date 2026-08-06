#pragma once

#include <cstddef>
#include <cstdint>

namespace lyrium::dao
{

struct EngineConfig
{
    bool hook_texture_paths{true};
    bool hook_cache{true};

    bool hook_allocator{false};
    std::size_t allocation_log_threshold{256u * 1024u};

    // The engine's own pool allocator. Off by default because pool_alloc sits on
    // the entry point every engine allocation goes through, and this project has
    // already crashed the game twice by interposing an allocator on first contact.
    bool hook_pool{false};
};

// What the install gate decided. aborted means verification failed and the
// process was left completely unmodified.
struct InstallState
{
    std::size_t planned{};
    std::size_t installed{};
    std::size_t failed_verification{};
    std::size_t failed_commit{};
    std::intptr_t base_delta{};
    bool aborted{};
};

auto install_engine_hooks(const EngineConfig &config) -> void;
auto engine_install_state() -> InstallState;

// Read-only verification of every non-optional target. Safe from DllMain.
auto targets_verify_clean() -> bool;
auto remove_engine_hooks() -> void;

auto emergency_evict(int max_count) -> int;
auto emergency_clear_texture_cache() -> bool;

auto texture_cache_known() -> bool;

// Just the pending-release count, without assembling an EngineState. This runs
// on every texture creation through the rescue probe, and engine_state() builds
// a twenty-field struct and reads several counters to answer one question.
// Returns 0 when the cache is not readable, which reads as "nothing to evict".
auto cache_pending_releases() -> std::int32_t;

struct EngineState
{
    const void *texture_cache;
    std::int32_t texture_memory;
    std::int32_t pending_releases;
    bool cache_readable;

    std::uint64_t texture_loads;
    std::uint64_t engine_texture_creates;
    std::uint64_t engine_texture_failures;
    std::uint64_t suspect_textures;
    std::uint64_t evictions;
    std::uint64_t evicted_textures;

    std::uint64_t large_allocations;
    std::uint64_t large_allocation_bytes;
    std::uint64_t large_allocations_live;
    std::uint64_t large_allocation_bytes_live;

    std::uint64_t malloc_calls;
    std::uint64_t free_calls;
    std::uint64_t realloc_calls;
    std::uint64_t malloc_total_bytes;
    std::uint64_t malloc_largest;

    // The engine's own pool. pool_alloc_failures is the figure the log has never
    // had: the engine allocates decoded asset data from its pool long before it
    // reaches D3D, so a pool set too small starves it without ever producing an
    // E_OUTOFMEMORY or a failed texture create.
    std::uint64_t pool_allocs;
    std::uint64_t pool_alloc_failures;
    std::uint64_t pool_alloc_largest_bytes;
    std::uint64_t pool_registrations;

    // Size the engine registered for "Main Pool", which is what survived its
    // back-off loop rather than what it asked for. Zero until it registers.
    const void *main_pool_base;
    std::uint64_t main_pool_bytes;
};

auto engine_state() -> EngineState;

auto note_d3d_create_result(std::int32_t hr, std::uint64_t bytes) -> void;

}
