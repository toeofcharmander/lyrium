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
};

auto engine_state() -> EngineState;

auto note_d3d_create_result(std::int32_t hr, std::uint64_t bytes) -> void;

}
