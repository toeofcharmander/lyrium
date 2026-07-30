#pragma once

#include <cstddef>
#include <cstdint>

namespace eluvian::dao
{

struct EngineConfig
{
    bool hook_texture_paths{true};
    bool hook_cache{true};

    bool hook_allocator{false};
    std::size_t allocation_log_threshold{256u * 1024u};
};

auto install_engine_hooks(const EngineConfig &config) -> void;
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
