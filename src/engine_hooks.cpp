#include "lyrium/dao/engine_hooks.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <windows.h>

#include "lyrium/containers/string.h"
#include "lyrium/containers/unordered_map.h"
#include "lyrium/containers/vector.h"
#include "lyrium/dao/inline_hook.h"
#include "lyrium/dao/pool_layout.h"
#include "lyrium/dao/pool_occupancy.h"
#include "lyrium/dao/targets.h"
#include "lyrium/diag/alloc_context.h"
#include "lyrium/log.h"
#include "lyrium/utils.h"

namespace lyrium::dao
{

namespace
{

#define LYRIUM_THISCALL __attribute__((fastcall))
#define LYRIUM_STDCALL __attribute__((stdcall))
#define LYRIUM_CDECL __attribute__((cdecl))

using LoadTextureFileFn = void *(LYRIUM_THISCALL *)(void *, void *, void *, const void *, int);

using CreateTextureCachedFn = void *(LYRIUM_THISCALL *)(void *, void *, void *, const void *, int, int, int, int, int);

using CreateTextureRegisteredFn =
    void *(LYRIUM_THISCALL *)(void *, void *, void *, const void *, int, int, int, int, int, int, int);

using StreamLoadFn = void *(LYRIUM_THISCALL *)(void *, void *, void *, void *, int, int, int);

using DecodeTextureMemoryFn = void *(LYRIUM_STDCALL *)(void *, const void *, unsigned int, void *, int);

using CreateTexture2DFn = void *(LYRIUM_THISCALL *)(void *, void *, unsigned, unsigned, unsigned, unsigned, int, int);

using CreateTextureFromMemoryFn = void *(LYRIUM_THISCALL *)(void *, void *, const void *, unsigned int);

using CreateVolumeFromMemoryFn =
    void *(LYRIUM_THISCALL *)(void *, void *, const void *, unsigned int, int, int, int, int, int, int);

using EvictFn = void(LYRIUM_THISCALL *)(void *, void *, int);

using ClearFn = void(LYRIUM_THISCALL *)(void *, void *);

using MallocFn = void *(LYRIUM_CDECL *)(std::size_t);
using FreeFn = void(LYRIUM_CDECL *)(void *);
using ReallocFn = void *(LYRIUM_CDECL *)(void *, std::size_t);

// The engine's own pool allocator. __cdecl: all three of its return paths are a
// bare RET.
using PoolAllocFn = void *(LYRIUM_CDECL *)(std::uint32_t, std::int32_t);

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
auto hook_pool_alloc = InlineHook{};

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

std::atomic<std::uint64_t> counter_pool_allocs{};
std::atomic<std::uint64_t> counter_pool_alloc_failures{};
std::atomic<std::uint64_t> counter_pool_alloc_largest{};

std::atomic<std::uint32_t> traced_hook_calls{};

class FirstCallTrace
{
  public:
    FirstCallTrace(std::uint32_t bit, const char *enter, const char *leave)
        : enter_{enter}
        , leave_{leave}
        , first_{(traced_hook_calls.fetch_or(bit, std::memory_order_relaxed) & bit) == 0u}
    {
        if (first_)
        {
            breadcrumb(enter_);
        }
    }

    ~FirstCallTrace()
    {
        if (first_)
        {
            breadcrumb(leave_);
        }
    }

    FirstCallTrace(const FirstCallTrace &) = delete;
    auto operator=(const FirstCallTrace &) -> FirstCallTrace & = delete;

  private:
    const char *enter_;
    const char *leave_;
    bool first_;
};

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
UnorderedMap<const void *, LiveAllocation> live_allocations;
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

auto read_engine_u32(const void *base, std::size_t offset, std::uint32_t &out) -> bool
{
    if (base == nullptr)
    {
        return false;
    }

    const auto *address = static_cast<const std::uint8_t *>(base) + offset;
    auto info = ::MEMORY_BASIC_INFORMATION{};
    if (::VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
    {
        return false;
    }

    std::memcpy(&out, address, sizeof(out));
    return true;
}

// Locate the main pool object inside the engine's allocator manager, by the same
// walk the engine's own lookup does. Every hop is guarded: the manager pointer is a
// bare global with no prologue to hash, so nothing read through it can be verified
// by comparison and the plausibility screen is the only check this path can have.
//
// This exists because a detour on the registrar cannot work -- the pool is built at
// engine startup through a lazy getter, long before Direct3DCreate9 gives lyrium
// anywhere to install from. A session with that hook installed and verified
// recorded pools=0.
auto find_main_pool() -> const void *
{
    const auto delta = image_base_delta();
    const auto *site = reinterpret_cast<const void *>(manager_pointer_site + static_cast<std::uintptr_t>(delta));

    auto manager_value = std::uint32_t{};
    if (!read_engine_u32(site, 0u, manager_value) || manager_value == 0u)
    {
        return nullptr;
    }
    const auto *manager = reinterpret_cast<const void *>(static_cast<std::uintptr_t>(manager_value));

    for (auto slot = std::size_t{0}; slot < manager_pool_slots; ++slot)
    {
        auto candidate = std::uint32_t{};
        if (!read_engine_u32(manager, manager_pool_array_offset + slot * sizeof(std::uint32_t), candidate) ||
            candidate == 0u)
        {
            continue;
        }

        const auto *pool = reinterpret_cast<const void *>(static_cast<std::uintptr_t>(candidate));
        auto id = std::uint32_t{};
        if (read_engine_u32(pool, pool_id_offset, id) && static_cast<std::int32_t>(id) == main_pool_id)
        {
            return pool;
        }
    }

    return nullptr;
}

auto read_main_pool(std::uint32_t &base_out, std::uint64_t &size_out, std::uint64_t &usable_out) -> bool
{
    const auto *pool = find_main_pool();
    if (pool == nullptr)
    {
        return false;
    }

    auto base = std::uint32_t{};
    auto size = std::uint32_t{};
    auto usable = std::uint32_t{};
    if (!read_engine_u32(pool, pool_base_offset, base) || !read_engine_u32(pool, pool_size_offset, size) ||
        !read_engine_u32(pool, pool_usable_offset, usable))
    {
        return false;
    }

    if (!main_pool_reading_is_plausible(base, size))
    {
        return false;
    }

    base_out = base;
    size_out = size;
    usable_out = usable;
    return true;
}

// How full the pool is.
//
// One VirtualQuery for the whole region, then plain reads. The first version of
// this queried per block, which on a 713 MB pool is hundreds of thousands of
// syscalls per sample -- it stalled the sampler thread so badly the game appeared
// to freeze and never emitted a single sample. The pool is one committed HeapAlloc
// block, so one query covers every block inside it.
//
// Deliberately WITHOUT the engine's pool lock: taking an engine mutex from the
// sampler thread is what produced the one hang this project could only recover from
// by signing out. A walk can therefore race a live allocation, so it is bounded at
// both ends -- every block is screened, the count is capped, and anything
// inconsistent abandons the sample rather than reporting it.
auto walk_main_pool(BlockTally &tally, std::uint64_t &elapsed_us, bool &capped) -> bool
{
    const auto started = now_us();
    capped = false;

    const auto *pool = find_main_pool();
    if (pool == nullptr)
    {
        return false;
    }

    auto start = std::uint32_t{};
    auto usable = std::uint32_t{};
    if (!read_engine_u32(pool, pool_aligned_offset, start) || !read_engine_u32(pool, pool_usable_offset, usable) ||
        start == 0u || usable == 0u)
    {
        return false;
    }

    // The single query that makes this affordable. Everything below reads directly,
    // so the walk must never leave the range it just proved is committed.
    const auto *region_start = reinterpret_cast<const std::uint8_t *>(static_cast<std::uintptr_t>(start));
    auto info = ::MEMORY_BASIC_INFORMATION{};
    if (::VirtualQuery(region_start, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
    {
        return false;
    }

    const auto *committed_end = static_cast<const std::uint8_t *>(info.BaseAddress) + info.RegionSize;
    const auto *walk_end = region_start + usable;
    if (walk_end > committed_end)
    {
        // The pool spans more than the region we verified. One query is then not
        // enough, and guessing is exactly what this function must not do.
        return false;
    }

    const auto *cursor = region_start;
    while (cursor < walk_end)
    {
        if (tally.blocks >= max_walk_blocks)
        {
            capped = true;
            break;
        }

        std::uint32_t header[2]{};
        std::memcpy(header, cursor, sizeof(header));

        // The in-use byte sits at +7, the top byte of the dword at +4.
        const auto in_use = ((header[1] >> 24u) & 0xFFu) != 0u;
        if (!tally_block(tally, header[0], in_use, static_cast<std::uint64_t>(walk_end - cursor)))
        {
            return false;
        }
        cursor += header[0];
    }

    elapsed_us = static_cast<std::uint64_t>(now_us() - started);
    return true;
}

LYRIUM_THISCALL auto load_texture_file_detour(void *self, void *edx, void *ret_buf, const void *path, int option)
    -> void *
{
    const auto trace =
        FirstCallTrace{1u << 0u, "engine load_texture_file: enter", "engine load_texture_file: returned"};
    const auto guard = ReentryGuard{};

    const auto original = reinterpret_cast<LoadTextureFileFn>(hook_load_texture_file.trampoline());
    auto *result = original(self, edx, ret_buf, path, option);

    counter_texture_loads.fetch_add(1u, std::memory_order_relaxed);

    return result;
}

LYRIUM_THISCALL auto create_texture_cached_detour(
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
    const auto trace =
        FirstCallTrace{1u << 1u, "engine create_texture_cached: enter", "engine create_texture_cached: returned"};
    const auto guard = ReentryGuard{};
    const auto watched = lyrium::diag::AllocContextScope{
        lyrium::diag::current_alloc_context(), lyrium::diag::AllocContext::engine_texture};

    const auto original = reinterpret_cast<CreateTextureCachedFn>(hook_create_texture_cached.trampoline());
    auto *result = original(self, edx, ret_buf, name, width, height, levels, usage, format);

    return result;
}

LYRIUM_THISCALL auto create_texture_registered_detour(
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
    const auto trace = FirstCallTrace{
        1u << 2u, "engine create_texture_registered: enter", "engine create_texture_registered: returned"};
    const auto guard = ReentryGuard{};
    const auto watched = lyrium::diag::AllocContextScope{
        lyrium::diag::current_alloc_context(), lyrium::diag::AllocContext::engine_texture};

    const auto original = reinterpret_cast<CreateTextureRegisteredFn>(hook_create_texture_registered.trampoline());
    auto *result = original(self, edx, ret_buf, name, a, b, c, d, e, f, g);

    return result;
}

LYRIUM_THISCALL auto stream_load_detour(void *self, void *edx, void *ret_buf, void *stream, int a, int b, int c)
    -> void *
{
    const auto trace = FirstCallTrace{1u << 3u, "engine stream_load: enter", "engine stream_load: returned"};
    const auto guard = ReentryGuard{};
    const auto watched = lyrium::diag::AllocContextScope{
        lyrium::diag::current_alloc_context(), lyrium::diag::AllocContext::engine_texture};

    const auto original = reinterpret_cast<StreamLoadFn>(hook_stream_load.trampoline());
    auto *result = original(self, edx, ret_buf, stream, a, b, c);

    return result;
}

LYRIUM_STDCALL auto decode_texture_memory_detour(
    void *ret_buf,
    const void *source,
    unsigned int size,
    void *owner,
    int unused) -> void *
{
    const auto trace =
        FirstCallTrace{1u << 4u, "engine decode_texture_memory: enter", "engine decode_texture_memory: returned"};
    const auto guard = ReentryGuard{};

    last_d3d_result.valid = false;

    const auto original = reinterpret_cast<DecodeTextureMemoryFn>(hook_decode_texture_memory.trampoline());
    auto *result = original(ret_buf, source, size, owner, unused);

    return result;
}

LYRIUM_THISCALL auto create_texture_2d_detour(
    void *self,
    void *edx,
    unsigned width,
    unsigned height,
    unsigned levels,
    unsigned flags,
    int format_index,
    int pool_index) -> void *
{
    const auto trace =
        FirstCallTrace{1u << 5u, "engine create_texture_2d: enter", "engine create_texture_2d: returned"};
    const auto guard = ReentryGuard{};
    const auto watched = lyrium::diag::AllocContextScope{
        lyrium::diag::current_alloc_context(), lyrium::diag::AllocContext::engine_texture};

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

LYRIUM_THISCALL auto create_texture_from_memory_detour(void *self, void *edx, const void *source, unsigned int size)
    -> void *
{
    const auto trace = FirstCallTrace{
        1u << 6u, "engine create_texture_from_memory: enter", "engine create_texture_from_memory: returned"};
    const auto guard = ReentryGuard{};

    const auto original = reinterpret_cast<CreateTextureFromMemoryFn>(hook_create_texture_from_memory.trampoline());
    auto *result = original(self, edx, source, size);

    return result;
}

LYRIUM_THISCALL auto create_volume_from_memory_detour(
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
    const auto trace = FirstCallTrace{
        1u << 7u, "engine create_volume_from_memory: enter", "engine create_volume_from_memory: returned"};
    const auto guard = ReentryGuard{};

    const auto original = reinterpret_cast<CreateVolumeFromMemoryFn>(hook_create_volume_from_memory.trampoline());
    auto *result = original(self, edx, source, size, a, b, c, d, e, f);

    return result;
}

LYRIUM_THISCALL auto evict_detour(void *self, void *edx, int max_count) -> void
{
    const auto trace =
        FirstCallTrace{1u << 8u, "engine texture_cache_evict: enter", "engine texture_cache_evict: returned"};
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

LYRIUM_THISCALL auto clear_detour(void *self, void *edx) -> void
{
    const auto trace =
        FirstCallTrace{1u << 9u, "engine texture_cache_clear: enter", "engine texture_cache_clear: returned"};
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

LYRIUM_CDECL auto malloc_detour(std::size_t size) -> void *
{
    const auto trace = FirstCallTrace{1u << 10u, "engine malloc: enter", "engine malloc: returned"};
    counter_malloc_calls.fetch_add(1u, std::memory_order_relaxed);
    counter_malloc_total_bytes.fetch_add(size, std::memory_order_relaxed);
    auto largest = counter_malloc_largest.load(std::memory_order_relaxed);
    while (size > largest && !counter_malloc_largest.compare_exchange_weak(largest, size, std::memory_order_relaxed))
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

LYRIUM_CDECL auto free_detour(void *pointer) -> void
{
    const auto trace = FirstCallTrace{1u << 11u, "engine free: enter", "engine free: returned"};
    counter_free_calls.fetch_add(1u, std::memory_order_relaxed);

    if (!in_hook)
    {
        const auto guard = ReentryGuard{};
        untrack_allocation(pointer);
    }

    const auto original = reinterpret_cast<FreeFn>(hook_free.trampoline());
    original(pointer);
}

LYRIUM_CDECL auto realloc_detour(void *pointer, std::size_t size) -> void *
{
    const auto trace = FirstCallTrace{1u << 12u, "engine realloc: enter", "engine realloc: returned"};
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

// Deliberately without a ReentryGuard. This sits on the entry point every engine
// allocation goes through, the body below calls nothing that could re-enter a
// lyrium hook, and a thread_local read and write per allocation is a real cost on
// the hottest path in the process.
LYRIUM_CDECL auto pool_alloc_detour(std::uint32_t size, std::int32_t tag) -> void *
{
    const auto trace = FirstCallTrace{1u << 13u, "engine pool_alloc: enter", "engine pool_alloc: returned"};

    const auto original = reinterpret_cast<PoolAllocFn>(hook_pool_alloc.trampoline());
    auto *result = original(size, tag);

    counter_pool_allocs.fetch_add(1u, std::memory_order_relaxed);

    // A zero-size request returns null without the allocator ever being asked, so
    // counting it would put noise into the one counter whose whole value is that a
    // non-zero reading means the pool is exhausted.
    if (size != 0u)
    {
        if (result == nullptr)
        {
            counter_pool_alloc_failures.fetch_add(1u, std::memory_order_relaxed);
        }

        auto largest = counter_pool_alloc_largest.load(std::memory_order_relaxed);
        while (size > largest &&
               !counter_pool_alloc_largest.compare_exchange_weak(largest, size, std::memory_order_relaxed))
        {
        }
    }

    return result;
}


InstallState install_state{};

struct Planned
{
    InlineHook *hook;
    TargetId id;
    void *detour;
};

auto report_verification(const VerifyReport &report) -> void
{
    if (report.ok())
    {
        log("engine hook {} ({}): verified at {:#010x}", report.name, report.symbol, report.site);
        return;
    }

    // Everything below was already being computed and thrown away, which is why
    // a mismatched game build produced no evidence at all.
    log("engine hook {} ({}): {} at {:#010x}", report.name, report.symbol, report.reason(), report.site);

    if (report.readable && !report.prologue_matched)
    {
        log("  first difference at byte {}: expected {:#04x}, found {:#04x}",
            report.first_mismatch,
            report.expected[report.first_mismatch],
            report.found[report.first_mismatch]);

        auto expected_hex = String{};
        auto found_hex = String{};
        static constexpr auto digits = "0123456789abcdef";
        for (auto i = std::size_t{0}; i < report.patch_len && i < prologue_bytes; ++i)
        {
            expected_hex.push_back(digits[report.expected[i] >> 4]);
            expected_hex.push_back(digits[report.expected[i] & 0x0Fu]);
            expected_hex.push_back(' ');
            found_hex.push_back(digits[report.found[i] >> 4]);
            found_hex.push_back(digits[report.found[i] & 0x0Fu]);
            found_hex.push_back(' ');
        }
        log("  expected {}", expected_hex.c_str());
        log("  found    {}", found_hex.c_str());
    }

    if (report.body_checked && !report.body_matched)
    {
        log("  body sha256 computed {}", report.computed_hash.data());
        log("  body sha256 expected {}", report.table_hash);
    }
}

}

auto install_engine_hooks(const EngineConfig &configuration) -> void
{
    config = configuration;

    // Plan first. Nothing is written until every target has been verified,
    // because a partial install is worse than none: the hooks that did land
    // change engine behaviour while the ones that did not leave the
    // compensating logic absent, and the old code discarded install()'s result
    // so nobody could tell which had happened.
    auto planned = Vector<Planned>{};
    planned.reserve(13u);

    if (config.hook_texture_paths)
    {
        planned.push_back(
            {&hook_load_texture_file,
             TargetId::load_texture_file,
             reinterpret_cast<void *>(&load_texture_file_detour)});
        planned.push_back(
            {&hook_create_texture_cached,
             TargetId::create_texture_cached,
             reinterpret_cast<void *>(&create_texture_cached_detour)});
        planned.push_back(
            {&hook_create_texture_registered,
             TargetId::create_texture_registered,
             reinterpret_cast<void *>(&create_texture_registered_detour)});
        planned.push_back({&hook_stream_load, TargetId::stream_load, reinterpret_cast<void *>(&stream_load_detour)});
        planned.push_back(
            {&hook_decode_texture_memory,
             TargetId::decode_texture_memory,
             reinterpret_cast<void *>(&decode_texture_memory_detour)});
        planned.push_back(
            {&hook_create_texture_2d,
             TargetId::create_texture_2d_factory,
             reinterpret_cast<void *>(&create_texture_2d_detour)});
        planned.push_back(
            {&hook_create_texture_from_memory,
             TargetId::create_texture_from_memory,
             reinterpret_cast<void *>(&create_texture_from_memory_detour)});
        planned.push_back(
            {&hook_create_volume_from_memory,
             TargetId::create_volume_from_memory,
             reinterpret_cast<void *>(&create_volume_from_memory_detour)});
    }

    if (config.hook_cache)
    {
        planned.push_back({&hook_evict, TargetId::texture_cache_evict, reinterpret_cast<void *>(&evict_detour)});
        planned.push_back({&hook_clear, TargetId::texture_cache_clear, reinterpret_cast<void *>(&clear_detour)});
    }

    if (config.hook_allocator)
    {
        planned.push_back({&hook_malloc, TargetId::crt_malloc, reinterpret_cast<void *>(&malloc_detour)});
        planned.push_back({&hook_free, TargetId::crt_free, reinterpret_cast<void *>(&free_detour)});
        planned.push_back({&hook_realloc, TargetId::crt_realloc, reinterpret_cast<void *>(&realloc_detour)});
    }

    if (config.hook_pool)
    {
        planned.push_back({&hook_pool_alloc, TargetId::pool_alloc, reinterpret_cast<void *>(&pool_alloc_detour)});
    }

    const auto delta = image_base_delta();

    // Pass one: read only. Not a single byte of the process is modified here.
    auto failures = std::size_t{0};
    for (const auto &entry : planned)
    {
        const auto report = InlineHook::verify_target(target(entry.id), delta);
        report_verification(report);
        if (!report.ok())
        {
            ++failures;
        }
    }

    install_state.planned = planned.size();
    install_state.failed_verification = failures;
    install_state.base_delta = delta;

    if (failures != 0u)
    {
        install_state.aborted = true;
        log("engine hooks: ABORTED, {} of {} targets failed verification, nothing was patched",
            failures,
            planned.size());
        log("engine hooks: this build of daorigins.exe does not match the table in dao/targets.h");
        return;
    }

    // Pass two: commit. Every target verified a moment ago, so a failure here is
    // a write or protection error rather than a version mismatch.
    for (const auto &entry : planned)
    {
        if (!entry.hook->install(target(entry.id), entry.detour, delta))
        {
            ++install_state.failed_commit;
            log("engine hook {}: commit failed ({})",
                target(entry.id).name,
                InlineHook::status_name(entry.hook->status()));
        }
        else
        {
            ++install_state.installed;
        }
    }

    log("engine hooks: {} of {} installed", install_state.installed, planned.size());
}

auto targets_verify_clean() -> bool
{
    // Read-only, allocation-light, and safe to call from DllMain. Used to decide
    // whether the pool patch may apply at all: shrinking the engine's memory
    // pool while the hooks meant to compensate never install is strictly worse
    // than not loading the mod, and those two decisions used to be made
    // independently at different times.
    const auto delta = image_base_delta();
    for (std::size_t i = 0u; i < static_cast<std::size_t>(TargetId::count); ++i)
    {
        const auto id = static_cast<TargetId>(i);
        if (id == TargetId::crt_malloc || id == TargetId::crt_free || id == TargetId::crt_realloc ||
            id == TargetId::pool_alloc)
        {
            // Allocator and pool hooks are opt-in and off by default, so a mismatch
            // there must not veto everything else.
            continue;
        }
        if (!InlineHook::verify_target(target(id), delta).ok())
        {
            return false;
        }
    }
    return true;
}

auto engine_install_state() -> InstallState
{
    return install_state;
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
        &hook_pool_alloc,
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

auto cache_pending_releases() -> std::int32_t
{
    auto memory = std::int32_t{};
    auto pending = std::int32_t{};
    const auto *cache = texture_cache_ptr.load(std::memory_order_relaxed);
    return cache_snapshot(cache, memory, pending) ? pending : 0;
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

auto emergency_clear_texture_cache() -> bool
{
    auto *cache = const_cast<void *>(texture_cache_ptr.load(std::memory_order_relaxed));
    if (cache == nullptr || !hook_clear.installed() || in_hook)
    {
        return false;
    }

    const auto guard = ReentryGuard{};
    const auto original = reinterpret_cast<ClearFn>(hook_clear.trampoline());
    original(cache, nullptr);
    return true;
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

    state.pool_allocs = counter_pool_allocs.load(std::memory_order_relaxed);
    state.pool_alloc_failures = counter_pool_alloc_failures.load(std::memory_order_relaxed);
    state.pool_alloc_largest_bytes = counter_pool_alloc_largest.load(std::memory_order_relaxed);
    state.main_pool_observed =
        read_main_pool(state.main_pool_base, state.main_pool_bytes, state.main_pool_usable_bytes);

    if (state.main_pool_observed)
    {
        auto tally = BlockTally{};
        auto elapsed = std::uint64_t{};
        auto capped = false;
        const auto finished = walk_main_pool(tally, elapsed, capped);
        state.main_pool_walk_us = elapsed;
        state.main_pool_walk_capped = capped;
        state.main_pool_blocks = tally.blocks;

        // A capped walk stopped before the end, so its sums are a prefix of the
        // pool and not totals. Reporting them as totals produced samples reading
        // free=0 largest_free=0 on a pool that was 70% empty, which looks exactly
        // like the exhaustion this is supposed to detect.
        state.main_pool_walked = finished && !capped;
        if (state.main_pool_walked)
        {
            state.main_pool_used_bytes = tally.used_bytes;
            state.main_pool_free_bytes = tally.free_bytes;
            state.main_pool_largest_free_bytes = tally.largest_free_bytes;
        }
    }

    state.large_allocation_bytes_live = live_allocation_bytes.load(std::memory_order_relaxed);
    {
        auto lock = std::scoped_lock{allocation_mutex};
        state.large_allocations_live = live_allocations.size();
    }

    return state;
}

}
