#include "eluvian/dao/engine_hooks.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <windows.h>

#include "eluvian/dao/inline_hook.h"
#include "eluvian/dao/targets.h"
#include "eluvian/log.h"
#include "eluvian/utils.h"

namespace eluvian::dao
{

namespace
{

#define ELUVIAN_THISCALL __attribute__((fastcall))
#define ELUVIAN_STDCALL __attribute__((stdcall))
#define ELUVIAN_CDECL __attribute__((cdecl))

using LoadTextureFileFn = void *(ELUVIAN_THISCALL *)(void *, void *, void *, const void *, int);

using CreateTextureCachedFn = void *(ELUVIAN_THISCALL *)(void *, void *, void *, const void *, int, int, int, int, int);

using CreateTextureRegisteredFn =
    void *(ELUVIAN_THISCALL *)(void *, void *, void *, const void *, int, int, int, int, int, int, int);

using StreamLoadFn = void *(ELUVIAN_THISCALL *)(void *, void *, void *, void *, int, int, int);

using DecodeTextureMemoryFn = void *(ELUVIAN_STDCALL *)(void *, const void *, unsigned int, void *, int);

using CreateTexture2DFn = void *(ELUVIAN_THISCALL *)(void *, void *, unsigned, unsigned, unsigned, unsigned, int, int);

using CreateTextureFromMemoryFn = void *(ELUVIAN_THISCALL *)(void *, void *, const void *, unsigned int);

using CreateVolumeFromMemoryFn =
    void *(ELUVIAN_THISCALL *)(void *, void *, const void *, unsigned int, int, int, int, int, int, int);

using EvictFn = void(ELUVIAN_THISCALL *)(void *, void *, int);

using ClearFn = void(ELUVIAN_THISCALL *)(void *, void *);

using MallocFn = void *(ELUVIAN_CDECL *)(std::size_t);
using FreeFn = void(ELUVIAN_CDECL *)(void *);
using ReallocFn = void *(ELUVIAN_CDECL *)(void *, std::size_t);

auto config = EngineConfig{};

auto hook_load_texture_file = InlineHook{};
auto hook_create_texture_cached = InlineHook{};
auto hook_create_texture_registered = InlineHook{};
auto hook_stream_load = InlineHook{};
auto hook_decode_texture_memory = InlineHook{};
auto hook_create_texture_2d = InlineHook{};
auto hook_create_texture_from_memory = InlineHook{};
auto hook_create_volume_from_memory = InlineHook{};
auto hook_evict = InlineHook{};
auto hook_clear = InlineHook{};
auto hook_malloc = InlineHook{};
auto hook_free = InlineHook{};
auto hook_realloc = InlineHook{};

std::atomic<const void *> texture_cache_ptr{nullptr};

std::atomic<std::uint64_t> counter_texture_loads{};
std::atomic<std::uint64_t> counter_engine_creates{};
std::atomic<std::uint64_t> counter_engine_failures{};
std::atomic<std::uint64_t> counter_suspect{};
std::atomic<std::uint64_t> counter_evictions{};
std::atomic<std::uint64_t> counter_evicted{};

std::atomic<std::uint64_t> counter_large_allocs{};
std::atomic<std::uint64_t> counter_large_alloc_bytes{};

std::atomic<std::uint64_t> counter_malloc_calls{};
std::atomic<std::uint64_t> counter_free_calls{};
std::atomic<std::uint64_t> counter_realloc_calls{};
std::atomic<std::uint64_t> counter_malloc_total_bytes{};
std::atomic<std::uint64_t> counter_malloc_largest{};

struct D3DResultSlot
{
    std::int32_t hr;
    std::uint64_t bytes;
    std::int64_t stamp_us;
    bool valid;
};
thread_local auto last_d3d_result = D3DResultSlot{};

thread_local auto in_hook = false;

struct ReentryGuard
{
    bool engaged;

    ReentryGuard()
        : engaged{!in_hook}
    {
        if (engaged)
        {
            in_hook = true;
        }
    }

    ~ReentryGuard()
    {
        if (engaged)
        {
            in_hook = false;
        }
    }
};

struct LiveAllocation
{
    std::size_t size;
    std::int64_t created_us;
};

std::mutex allocation_mutex;
std::unordered_map<const void *, LiveAllocation> live_allocations;
std::atomic<std::uint64_t> live_allocation_bytes{};

constexpr auto cache_texture_memory_offset = std::size_t{104};
constexpr auto cache_pending_offset = std::size_t{108};

auto read_cache_int(const void *cache, std::size_t offset, std::int32_t &out) -> bool
{
    if (cache == nullptr)
    {
        return false;
    }

    const auto *address = static_cast<const std::uint8_t *>(cache) + offset;
    auto info = ::MEMORY_BASIC_INFORMATION{};
    if (::VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
    {
        return false;
    }

    std::memcpy(&out, address, sizeof(out));
    return true;
}

auto cache_snapshot(const void *cache, std::int32_t &memory, std::int32_t &pending) -> bool
{
    return read_cache_int(cache, cache_texture_memory_offset, memory) &&
           read_cache_int(cache, cache_pending_offset, pending);
}

ELUVIAN_THISCALL auto load_texture_file_detour(
    void *self,
    void *edx,
    void *ret_buf,
    const void *path,
    int option) -> void *
{
    const auto guard = ReentryGuard{};

    const auto original = reinterpret_cast<LoadTextureFileFn>(hook_load_texture_file.trampoline());
    auto *result = original(self, edx, ret_buf, path, option);

    counter_texture_loads.fetch_add(1u, std::memory_order_relaxed);

    return result;
}

ELUVIAN_THISCALL auto create_texture_cached_detour(
    void *self,
    void *edx,
    void *ret_buf,
    const void *name,
    int width,
    int height,
    int levels,
    int usage,
    int format) -> void *
{
    const auto guard = ReentryGuard{};

    const auto original = reinterpret_cast<CreateTextureCachedFn>(hook_create_texture_cached.trampoline());
    auto *result = original(self, edx, ret_buf, name, width, height, levels, usage, format);

    return result;
}

ELUVIAN_THISCALL auto create_texture_registered_detour(
    void *self,
    void *edx,
    void *ret_buf,
    const void *name,
    int a,
    int b,
    int c,
    int d,
    int e,
    int f,
    int g) -> void *
{
    const auto guard = ReentryGuard{};

    const auto original = reinterpret_cast<CreateTextureRegisteredFn>(hook_create_texture_registered.trampoline());
    auto *result = original(self, edx, ret_buf, name, a, b, c, d, e, f, g);

    return result;
}

ELUVIAN_THISCALL auto stream_load_detour(
    void *self,
    void *edx,
    void *ret_buf,
    void *stream,
    int a,
    int b,
    int c) -> void *
{
    const auto guard = ReentryGuard{};

    const auto original = reinterpret_cast<StreamLoadFn>(hook_stream_load.trampoline());
    auto *result = original(self, edx, ret_buf, stream, a, b, c);

    return result;
}

ELUVIAN_STDCALL auto decode_texture_memory_detour(
    void *ret_buf,
    const void *source,
    unsigned int size,
    void *owner,
    int unused) -> void *
{
    const auto guard = ReentryGuard{};

    last_d3d_result.valid = false;

    const auto original = reinterpret_cast<DecodeTextureMemoryFn>(hook_decode_texture_memory.trampoline());
    auto *result = original(ret_buf, source, size, owner, unused);

    return result;
}

ELUVIAN_THISCALL auto create_texture_2d_detour(
    void *self,
    void *edx,
    unsigned width,
    unsigned height,
    unsigned levels,
    unsigned flags,
    int format_index,
    int pool_index) -> void *
{
    const auto guard = ReentryGuard{};

    last_d3d_result.valid = false;

    const auto original = reinterpret_cast<CreateTexture2DFn>(hook_create_texture_2d.trampoline());
    auto *result = original(self, edx, width, height, levels, flags, format_index, pool_index);

    counter_engine_creates.fetch_add(1u, std::memory_order_relaxed);
    if (result == nullptr)
    {
        counter_engine_failures.fetch_add(1u, std::memory_order_relaxed);
    }

    const auto d3d_failed = last_d3d_result.valid && last_d3d_result.hr < 0;
    const auto suspect = d3d_failed && result != nullptr;
    if (suspect)
    {
        counter_suspect.fetch_add(1u, std::memory_order_relaxed);
    }

    return result;
}

ELUVIAN_THISCALL auto create_texture_from_memory_detour(
    void *self,
    void *edx,
    const void *source,
    unsigned int size) -> void *
{
    const auto guard = ReentryGuard{};

    const auto original = reinterpret_cast<CreateTextureFromMemoryFn>(hook_create_texture_from_memory.trampoline());
    auto *result = original(self, edx, source, size);

    return result;
}

ELUVIAN_THISCALL auto create_volume_from_memory_detour(
    void *self,
    void *edx,
    const void *source,
    unsigned int size,
    int a,
    int b,
    int c,
    int d,
    int e,
    int f) -> void *
{
    const auto guard = ReentryGuard{};

    const auto original = reinterpret_cast<CreateVolumeFromMemoryFn>(hook_create_volume_from_memory.trampoline());
    auto *result = original(self, edx, source, size, a, b, c, d, e, f);

    return result;
}

ELUVIAN_THISCALL auto evict_detour(void *self, void *edx, int max_count) -> void
{
    const auto guard = ReentryGuard{};

    texture_cache_ptr.store(self, std::memory_order_relaxed);

    auto memory_before = std::int32_t{};
    auto pending_before = std::int32_t{};
    const auto readable = cache_snapshot(self, memory_before, pending_before);

    const auto original = reinterpret_cast<EvictFn>(hook_evict.trampoline());
    original(self, edx, max_count);

    auto memory_after = std::int32_t{};
    auto pending_after = std::int32_t{};
    cache_snapshot(self, memory_after, pending_after);

    counter_evictions.fetch_add(1u, std::memory_order_relaxed);
    if (readable && pending_before > pending_after)
    {
        counter_evicted.fetch_add(
            static_cast<std::uint64_t>(pending_before - pending_after), std::memory_order_relaxed);
    }

}

ELUVIAN_THISCALL auto clear_detour(void *self, void *edx) -> void
{
    const auto guard = ReentryGuard{};

    texture_cache_ptr.store(self, std::memory_order_relaxed);

    const auto original = reinterpret_cast<ClearFn>(hook_clear.trampoline());
    original(self, edx);

    auto memory_after = std::int32_t{};
    auto pending_after = std::int32_t{};
    cache_snapshot(self, memory_after, pending_after);

}

auto track_allocation(void *pointer, std::size_t size) -> void
{
    if (pointer == nullptr || size < config.allocation_log_threshold)
    {
        return;
    }

    {
        auto lock = std::scoped_lock{allocation_mutex};
        live_allocations[pointer] = LiveAllocation{.size = size, .created_us = now_us()};
    }

    counter_large_allocs.fetch_add(1u, std::memory_order_relaxed);
    counter_large_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
    live_allocation_bytes.fetch_add(size, std::memory_order_relaxed);

}

auto untrack_allocation(void *pointer) -> void
{
    if (pointer == nullptr)
    {
        return;
    }

    auto entry = LiveAllocation{};
    {
        auto lock = std::scoped_lock{allocation_mutex};
        const auto found = live_allocations.find(pointer);
        if (found == live_allocations.end())
        {
            return;
        }
        entry = found->second;
        live_allocations.erase(found);
    }

    live_allocation_bytes.fetch_sub(entry.size, std::memory_order_relaxed);

}

ELUVIAN_CDECL auto malloc_detour(std::size_t size) -> void *
{
    counter_malloc_calls.fetch_add(1u, std::memory_order_relaxed);
    counter_malloc_total_bytes.fetch_add(size, std::memory_order_relaxed);
    auto largest = counter_malloc_largest.load(std::memory_order_relaxed);
    while (size > largest &&
           !counter_malloc_largest.compare_exchange_weak(largest, size, std::memory_order_relaxed))
    {
    }

    const auto original = reinterpret_cast<MallocFn>(hook_malloc.trampoline());
    auto *result = original(size);

    if (!in_hook)
    {
        const auto guard = ReentryGuard{};
        track_allocation(result, size);
    }

    return result;
}

ELUVIAN_CDECL auto free_detour(void *pointer) -> void
{
    counter_free_calls.fetch_add(1u, std::memory_order_relaxed);

    if (!in_hook)
    {
        const auto guard = ReentryGuard{};
        untrack_allocation(pointer);
    }

    const auto original = reinterpret_cast<FreeFn>(hook_free.trampoline());
    original(pointer);
}

ELUVIAN_CDECL auto realloc_detour(void *pointer, std::size_t size) -> void *
{
    counter_realloc_calls.fetch_add(1u, std::memory_order_relaxed);

    const auto original = reinterpret_cast<ReallocFn>(hook_realloc.trampoline());

    if (in_hook)
    {
        return original(pointer, size);
    }

    {
        const auto guard = ReentryGuard{};
        untrack_allocation(pointer);
    }

    auto *result = original(pointer, size);

    {
        const auto guard = ReentryGuard{};
        track_allocation(result, size);
    }

    return result;
}

auto install(InlineHook &hook, TargetId id, void *detour) -> void
{
    const auto &entry = target(id);
    hook.install(entry, detour, image_base_delta());
}

}

auto install_engine_hooks(const EngineConfig &configuration) -> void
{
    config = configuration;

    if (config.hook_texture_paths)
    {
        install(hook_load_texture_file, TargetId::load_texture_file,
                reinterpret_cast<void *>(&load_texture_file_detour));
        install(hook_create_texture_cached, TargetId::create_texture_cached,
                reinterpret_cast<void *>(&create_texture_cached_detour));
        install(hook_create_texture_registered, TargetId::create_texture_registered,
                reinterpret_cast<void *>(&create_texture_registered_detour));
        install(hook_stream_load, TargetId::stream_load, reinterpret_cast<void *>(&stream_load_detour));
        install(hook_decode_texture_memory, TargetId::decode_texture_memory,
                reinterpret_cast<void *>(&decode_texture_memory_detour));
        install(hook_create_texture_2d, TargetId::create_texture_2d_factory,
                reinterpret_cast<void *>(&create_texture_2d_detour));
        install(hook_create_texture_from_memory, TargetId::create_texture_from_memory,
                reinterpret_cast<void *>(&create_texture_from_memory_detour));
        install(hook_create_volume_from_memory, TargetId::create_volume_from_memory,
                reinterpret_cast<void *>(&create_volume_from_memory_detour));
    }

    if (config.hook_cache)
    {
        install(hook_evict, TargetId::texture_cache_evict, reinterpret_cast<void *>(&evict_detour));
        install(hook_clear, TargetId::texture_cache_clear, reinterpret_cast<void *>(&clear_detour));
    }

    if (config.hook_allocator)
    {
        install(hook_malloc, TargetId::crt_malloc, reinterpret_cast<void *>(&malloc_detour));
        install(hook_free, TargetId::crt_free, reinterpret_cast<void *>(&free_detour));
        install(hook_realloc, TargetId::crt_realloc, reinterpret_cast<void *>(&realloc_detour));
    }

    const InlineHook *all[] = {
        &hook_load_texture_file,
        &hook_create_texture_cached,
        &hook_create_texture_registered,
        &hook_stream_load,
        &hook_decode_texture_memory,
        &hook_create_texture_2d,
        &hook_create_texture_from_memory,
        &hook_create_volume_from_memory,
        &hook_evict,
        &hook_clear,
        &hook_malloc,
        &hook_free,
        &hook_realloc,
    };

    (void)all;

}

auto remove_engine_hooks() -> void
{
    InlineHook *all[] = {
        &hook_malloc,
        &hook_free,
        &hook_realloc,
        &hook_load_texture_file,
        &hook_create_texture_cached,
        &hook_create_texture_registered,
        &hook_stream_load,
        &hook_decode_texture_memory,
        &hook_create_texture_2d,
        &hook_create_texture_from_memory,
        &hook_create_volume_from_memory,
        &hook_evict,
        &hook_clear,
    };

    for (auto *hook : all)
    {
        hook->remove();
    }
}

auto note_d3d_create_result(std::int32_t hr, std::uint64_t bytes) -> void
{
    last_d3d_result = D3DResultSlot{.hr = hr, .bytes = bytes, .stamp_us = now_us(), .valid = true};
}

auto texture_cache_known() -> bool
{
    return texture_cache_ptr.load(std::memory_order_relaxed) != nullptr;
}

auto emergency_evict(int max_count) -> int
{
    auto *cache = const_cast<void *>(texture_cache_ptr.load(std::memory_order_relaxed));
    if (cache == nullptr || !hook_evict.installed())
    {
        return 0;
    }

    if (in_hook)
    {
        return 0;
    }
    const auto guard = ReentryGuard{};

    auto memory_before = std::int32_t{};
    auto pending_before = std::int32_t{};
    const auto readable = cache_snapshot(cache, memory_before, pending_before);

    const auto original = reinterpret_cast<EvictFn>(hook_evict.trampoline());
    original(cache, nullptr, max_count);

    auto memory_after = std::int32_t{};
    auto pending_after = std::int32_t{};
    cache_snapshot(cache, memory_after, pending_after);

    const auto released = readable ? pending_before - pending_after : 0;

    return released;
}

auto engine_state() -> EngineState
{
    auto state = EngineState{};
    state.texture_cache = texture_cache_ptr.load(std::memory_order_relaxed);
    state.cache_readable = cache_snapshot(state.texture_cache, state.texture_memory, state.pending_releases);

    state.texture_loads = counter_texture_loads.load(std::memory_order_relaxed);
    state.engine_texture_creates = counter_engine_creates.load(std::memory_order_relaxed);
    state.engine_texture_failures = counter_engine_failures.load(std::memory_order_relaxed);
    state.suspect_textures = counter_suspect.load(std::memory_order_relaxed);
    state.evictions = counter_evictions.load(std::memory_order_relaxed);
    state.evicted_textures = counter_evicted.load(std::memory_order_relaxed);

    state.large_allocations = counter_large_allocs.load(std::memory_order_relaxed);
    state.large_allocation_bytes = counter_large_alloc_bytes.load(std::memory_order_relaxed);
    state.malloc_calls = counter_malloc_calls.load(std::memory_order_relaxed);
    state.free_calls = counter_free_calls.load(std::memory_order_relaxed);
    state.realloc_calls = counter_realloc_calls.load(std::memory_order_relaxed);
    state.malloc_total_bytes = counter_malloc_total_bytes.load(std::memory_order_relaxed);
    state.malloc_largest = counter_malloc_largest.load(std::memory_order_relaxed);
    state.large_allocation_bytes_live = live_allocation_bytes.load(std::memory_order_relaxed);
    {
        auto lock = std::scoped_lock{allocation_mutex};
        state.large_allocations_live = live_allocations.size();
    }

    return state;
}

}
