#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include <windows.h>

#include <d3d9.h>
#include <psapi.h>

#include "lyrium/allocators/heap_interposer.h"
#include "lyrium/config.h"
#include "lyrium/containers/unordered_map.h"
#include "lyrium/dao/engine_hooks.h"
#include "lyrium/dao/pool_patch.h"
#include "lyrium/diag/alloc_watch.h"
#include "lyrium/diag/process_info.h"
#include "lyrium/diag/sampler.h"
#include "lyrium/diag/texture_size.h"
#include "lyrium/diag/va_space.h"
#include "lyrium/hooks/com_hook.h"
#include "lyrium/log.h"
#include "lyrium/never_destroyed.h"
#include "lyrium/overlay.h"
#include "lyrium/policy/rescue_coordinator.h"
#include "lyrium/policy/texture_placement_policy.h"
#include "lyrium/rescue_access.h"
#include "lyrium/resettable_texture.h"
#include "lyrium/resource_tracker.h"
#include "lyrium/stats.h"
#include "lyrium/texture/dll_texture_ledger.h"
#include "lyrium/texture_recycler.h"
#include "lyrium/utils.h"

using lyrium::Event;

namespace lyrium
{
auto texture_ledger() -> DllTextureLedger &
{
    static auto ledger = DllTextureLedger{};
    return ledger;
}
}

namespace
{

template <class F>
struct OrigFunc;

template <class R, class Head, class... Tail>
struct OrigFunc<R(WINAPI *)(Head, Tail...)>
{
    using type = R(WINAPI *)(Tail...);
};

template <class F>
using OrigFuncType = OrigFunc<F>::type;

auto com_hook = lyrium::COMHook{};

auto config = lyrium::Config{};

// Whether this process can use the space above the 2 GB line at all. Read once:
// the rescue probe consults it on the create path, and this is a PE header walk.
const auto process_is_large_address_aware = lyrium::diag::read_image_flags().large_address_aware;

// Built once from the loaded configuration. This is a stepping stone: the policy
// is owned by a composition root in a later step, not reached through a global.
auto placement_policy() -> const lyrium::policy::TexturePlacementPolicy &
{
    static const auto policy = lyrium::policy::TexturePlacementPolicy{lyrium::policy::TexturePlacementConfig{
        .prefer_default = config.texture_pool.prefer_default,
        .minimum_bytes = config.texture_pool.minimum_bytes,
        .fall_back_to_managed = config.texture_pool.fall_back_to_managed,
    }};
    return policy;
}

auto remember_texture(void *texture, std::uint64_t bytes, ::D3DPOOL pool, bool relocated) -> void
{
    lyrium::texture_ledger().note_created(texture, static_cast<lyrium::texture::TexturePool>(pool), bytes, relocated);
}

auto forget_texture(void *texture) -> void
{
    (void)lyrium::texture_ledger().note_destroyed(texture);
}

auto forget_resettable_texture(::IDirect3DTexture9 *texture) -> void
{
    forget_texture(texture);
}

auto note_create(::HRESULT hr, std::uint64_t bytes) -> void
{
    const auto count = lyrium::stats::d3d_creates.fetch_add(1u, std::memory_order_relaxed) + 1u;
    lyrium::dao::note_d3d_create_result(static_cast<std::int32_t>(hr), bytes);

    if (count <= 8u || count % 256u == 0u || FAILED(hr))
    {
        lyrium::log("d3d create #{}: hr={:#010x}, bytes={}", count, static_cast<std::uint32_t>(hr), bytes);
    }

    if (FAILED(hr))
    {
        lyrium::stats::d3d_create_failures.fetch_add(1u, std::memory_order_relaxed);
        lyrium::diag::Sampler::instance().sample_now("create_failed");
    }
}

auto main_pool_override_bytes() -> std::uint32_t
{
    char path[MAX_PATH]{};
    if (::GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
    {
        return 0u;
    }
    auto ini = std::string{path};
    const auto slash = ini.find_last_of("\\/");
    ini = (slash == std::string::npos ? std::string{} : ini.substr(0, slash + 1)) + "lyrium.ini";

    const auto megabytes = ::GetPrivateProfileIntA("lyrium", "main_pool_mb", 0, ini.c_str());
    if (megabytes <= 0)
    {
        return 0u;
    }
    return static_cast<std::uint32_t>(megabytes) * 1024u * 1024u;
}

lyrium::dao::PoolPatchResult pool_patch_result{};
lyrium::HeapInterposerResult heap_arena_result{};
const char *allocation_watch_mode{"not attempted"};

auto rescue_coordinator() -> lyrium::policy::RescueCoordinator &;

// Attached to the sampler so every periodic address-space line is accompanied by
// what lyrium believes it is holding. Without this the only way to read the
// texture figures was to watch the overlay, which makes a session impossible to
// diagnose after the fact.
auto log_ledger_snapshot(std::string_view reason, const lyrium::diag::VaStats &) -> void
{
    auto totals = lyrium::texture::TextureTotals{};
    if (!lyrium::texture_ledger().try_totals(totals))
    {
        // Declining is correct here rather than waiting: on the exit path the
        // holder may be a thread ExitProcess already terminated.
        lyrium::log("textures[{}]: unavailable, ledger busy", reason);
        return;
    }

    lyrium::log(
        "textures[{}]: live={} bytes={} peak={} released={} default={} managed={} systemmem={} scratch={} "
        "unknown={} live_relocated={} live_relocated_bytes={} creates={} failures={} overrides={} "
        "uploads={} flushes={} upload_ms={} staging_new={} staging_reused={} "
        "locks={} map_ms={} unmap_ms={} sections={} section_ms={} "
        "cre_def={}/{}ms cre_oth={}/{}ms cre_max={}us "
        "vram={}mb vram_min={}mb "
        "override_bytes={} reverts={}",
        reason,
        totals.live_count,
        totals.total,
        totals.peak,
        totals.released_bytes,
        totals.by_pool[0],
        totals.by_pool[1],
        totals.by_pool[2],
        totals.by_pool[3],
        totals.by_pool[4],
        totals.relocated_count,
        totals.relocated_bytes,
        lyrium::stats::d3d_creates.load(std::memory_order_relaxed),
        lyrium::stats::d3d_create_failures.load(std::memory_order_relaxed),
        lyrium::stats::pool_overrides.load(std::memory_order_relaxed),
        lyrium::stats::uploads.load(std::memory_order_relaxed),
        lyrium::stats::flushes.load(std::memory_order_relaxed),
        lyrium::stats::upload_us.load(std::memory_order_relaxed) / 1000u,
        lyrium::stats::staging_created.load(std::memory_order_relaxed),
        lyrium::stats::staging_reused.load(std::memory_order_relaxed),
        lyrium::stats::locks.load(std::memory_order_relaxed),
        lyrium::stats::map_us.load(std::memory_order_relaxed) / 1000u,
        lyrium::stats::unmap_us.load(std::memory_order_relaxed) / 1000u,
        lyrium::stats::mapping_creates.load(std::memory_order_relaxed),
        lyrium::stats::mapping_create_us.load(std::memory_order_relaxed) / 1000u,
        lyrium::stats::create_default_calls.load(std::memory_order_relaxed),
        lyrium::stats::create_default_us.load(std::memory_order_relaxed) / 1000u,
        lyrium::stats::create_other_calls.load(std::memory_order_relaxed),
        lyrium::stats::create_other_us.load(std::memory_order_relaxed) / 1000u,
        lyrium::stats::create_slowest_us.load(std::memory_order_relaxed),
        lyrium::stats::vram_free_bytes.load(std::memory_order_relaxed) / (1024u * 1024u),
        lyrium::stats::vram_low_water.load(std::memory_order_relaxed) / (1024u * 1024u),
        lyrium::stats::pool_override_bytes.load(std::memory_order_relaxed),
        lyrium::stats::pool_reverts.load(std::memory_order_relaxed));

    if (config.allocation_watch)
    {
        lyrium::diag::report_alloc_records(reason);
    }

    if (heap_arena_result.installed)
    {
        lyrium::log_heap_interposer(reason);
    }

    const auto rescue = rescue_coordinator().stats();
    lyrium::log(
        "rescue[{}]: pressure={} preemptive={} on_failure={} evictions={} clears={} managed={} released={} "
        "suppressed={} scratch={}/{}kb headroom={} laa={} low={} avoided={} shape={}[need={}kb of {}kb in {}kb] "
        "last={} acted={}",
        reason,
        rescue.under_pressure,
        rescue.preemptive,
        rescue.on_failure,
        rescue.evictions,
        rescue.cache_clears,
        rescue.managed_evictions,
        rescue.released_total,
        rescue.suppressed,
        rescue.scratch_flushes,
        rescue.scratch_bytes_released / 1024u,
        rescue.last_largest_free_bytes,
        process_is_large_address_aware,
        lyrium::diag::Sampler::instance().largest_free_below_2g(),
        lyrium::stats::rescue_avoided_low.load(std::memory_order_relaxed),
        lyrium::policy::name_of(rescue.last_shape),
        rescue.largest_request_bytes / 1024u,
        rescue.last_shape_largest_bytes / 1024u,
        rescue.last_shape_total_bytes / 1024u,
        rescue.last_reason,
        rescue.last_action_reason);
}

auto enable_allocation_watch() -> void
{
    static constexpr auto threshold = 8ull * 1024ull * 1024ull;

    if (lyrium::diag::install_nt_alloc_hook(threshold))
    {
        allocation_watch_mode = "NtAllocateVirtualMemory";
    }
    else if (lyrium::diag::install_virtual_alloc_hook(threshold))
    {
        allocation_watch_mode = "VirtualAlloc";
    }
    else if (lyrium::diag::install_alloc_watch(threshold))
    {
        allocation_watch_mode = "main executable IAT";
    }
    else
    {
        allocation_watch_mode = "unavailable";
    }
}

// Windows adapters for the rescue seams. They hold no decisions: every choice
// about whether and how hard to evict belongs to RescuePolicy, which is tested
// without any of this.

// Defined below, next to the Reset hook's scope guard.
[[nodiscard]] auto inside_device_reset() -> bool;

class EngineEvictionBackend final : public lyrium::policy::EvictionBackend
{
  public:
    [[nodiscard]] auto cache_available() const -> bool override
    {
        return lyrium::dao::texture_cache_known();
    }

    [[nodiscard]] auto pending_releases() const -> std::int32_t override
    {
        return lyrium::dao::cache_pending_releases();
    }

    [[nodiscard]] auto evict(std::int32_t max_count) -> std::int32_t override
    {
        // Belt and braces: the create path already declines, but this is the
        // seam that actually runs engine code, so it refuses too.
        return inside_device_reset() ? 0 : lyrium::dao::emergency_evict(max_count);
    }

    [[nodiscard]] auto release_scratch() -> std::uint64_t override
    {
        // The recycler is flushed too when enabled; both pools refill naturally.
        auto freed = lyrium::flush_staging_pool();
        if (config.recycler.enabled)
        {
            lyrium::TextureRecycler::instance().purge();
        }
        return freed;
    }

    [[nodiscard]] auto clear_cache() -> bool override
    {
        if (inside_device_reset())
        {
            return false;
        }
        return lyrium::dao::emergency_clear_texture_cache();
    }

    auto evict_managed_resources() -> void override
    {
        if (auto *device = device_.load(std::memory_order_relaxed); device != nullptr)
        {
            device->EvictManagedResources();
        }
    }

    auto set_device(::IDirect3DDevice9 *device) -> void
    {
        device_.store(device, std::memory_order_relaxed);
    }

  private:
    std::atomic<::IDirect3DDevice9 *> device_{nullptr};
};

// Deliberately the sampler's cached reading rather than a fresh walk. A full
// address-space walk was measured between 9 and 20 ms during gameplay, growing
// with fragmentation, so it can never happen on the create path.
//
// Which of the two readings binds depends on the install; diag::headroom_bytes
// holds that decision and the evidence for it. This used to report the smaller
// unconditionally, which on a large-address-aware process meant evicting the
// engine's cache while 2 GB sat free above the line.
class SamplerFreeSpaceProbe final : public lyrium::policy::FreeSpaceProbe
{
  public:
    [[nodiscard]] auto largest_free_bytes() const -> std::uint64_t override
    {
        const auto &sampler = lyrium::diag::Sampler::instance();
        const auto anywhere = sampler.largest_free();
        const auto below_2g = sampler.largest_free_below_2g();
        const auto headroom = lyrium::diag::headroom_bytes(anywhere, below_2g, process_is_large_address_aware);

        // Counts what the old rule would have done differently, so one session
        // shows whether it was crying wolf. Deliberately compares against the
        // policy's own floor rather than a number repeated here.
        constexpr auto floor_bytes = lyrium::policy::RescueConfig{}.headroom_floor_bytes;
        if (below_2g != 0u && below_2g < floor_bytes && headroom >= floor_bytes)
        {
            lyrium::stats::rescue_avoided_low.fetch_add(1u, std::memory_order_relaxed);
        }

        return headroom;
    }

    // Always the low half, on both install types. This is the space that
    // degrades: on a large-address-aware process the region above the 2 GB line
    // is untouched reserve, so including it would report a healthy space no
    // matter how finely the low half were cut up.
    [[nodiscard]] auto constrained_largest_free_bytes() const -> std::uint64_t override
    {
        return lyrium::diag::Sampler::instance().largest_free_below_2g();
    }

    [[nodiscard]] auto constrained_total_free_bytes() const -> std::uint64_t override
    {
        return lyrium::diag::Sampler::instance().total_free_below_2g();
    }
};

class SystemClock final : public lyrium::policy::Clock
{
  public:
    [[nodiscard]] auto now_us() const -> std::int64_t override
    {
        return lyrium::now_us();
    }
};

// The coordinator points at these three, so they live in the same object and are
// constructed before it. Previously they were namespace-scope while the
// coordinator was a lazily-built function-local static, which happened to give
// the right lifetime relation only because lazy construction meant the
// coordinator was destroyed first. Nothing enforced it, and one non-trivial
// member added to RescueCoordinator would have silently turned that into a
// dangling read at exit.
struct RescueParts
{
    EngineEvictionBackend backend{};
    SamplerFreeSpaceProbe probe{};
    SystemClock clock{};
    lyrium::policy::RescueCoordinator coordinator;

    explicit RescueParts(const lyrium::policy::RescueConfig &configuration)
        : coordinator{configuration, backend, probe, clock}
    {
    }
};

// Built once from the loaded configuration, like the placement policy. Another
// stepping stone until the composition root owns these.
// Attributes one driver CreateTexture call to the pool it was made in. The
// slowest single call is kept because a stall is one long call, not a raised
// average, and an average over thousands of cheap creates would hide it.
auto note_create_cost(::D3DPOOL pool, std::int64_t elapsed_us) -> void
{
    const auto us = elapsed_us < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(elapsed_us);
    if (pool == D3DPOOL_DEFAULT)
    {
        lyrium::stats::create_default_calls.fetch_add(1u, std::memory_order_relaxed);
        lyrium::stats::create_default_us.fetch_add(us, std::memory_order_relaxed);
    }
    else
    {
        lyrium::stats::create_other_calls.fetch_add(1u, std::memory_order_relaxed);
        lyrium::stats::create_other_us.fetch_add(us, std::memory_order_relaxed);
    }

    auto slowest = lyrium::stats::create_slowest_us.load(std::memory_order_relaxed);
    while (us > slowest &&
           !lyrium::stats::create_slowest_us.compare_exchange_weak(slowest, us, std::memory_order_relaxed))
    {
    }
}

// True while IDirect3DDevice9::Reset is on this thread's stack.
//
// The engine's D3DGraphicsDriver::ResetDevice broadcasts to a registry of
// D3DResetable objects, and it registers each object from the D3DResetable base
// constructor and removes it from the base destructor. Anything currently
// between those points sits in the registry with the abstract vtable installed,
// where the reset slot is _purecall. Destroying engine textures while that
// broadcast is running mutates the very vector it iterates, and the engine's
// driver mutex is recursive so it will not stop us re-entering on this thread.
//
// So no rescue may call engine cache-evict or cache-clear with Reset on the
// stack, however desperate the address space looks.
thread_local auto in_device_reset = false;

struct ResetScope
{
    ResetScope()
    {
        in_device_reset = true;
    }
    ~ResetScope()
    {
        in_device_reset = false;
    }
    ResetScope(const ResetScope &) = delete;
    auto operator=(const ResetScope &) -> ResetScope & = delete;
};

[[nodiscard]] auto inside_device_reset() -> bool
{
    return in_device_reset;
}

// Set by the Reset hook, consumed on the next EndScene. Sampling and logging are
// diagnostics, so they can happen a frame later, outside the driver's lock.
std::atomic<std::uint32_t> reset_purged{};
std::atomic<bool> reset_report_pending{false};
std::atomic<std::uint32_t> reset_failed_hr{0u};

auto rescue_parts() -> RescueParts &
{
    static auto parts = lyrium::NeverDestroyed<RescueParts>{lyrium::policy::RescueConfig{
        .preemptive = config.rescue.preemptive,
        .on_failure = config.rescue.on_failure,
        .evict_managed = config.rescue.evict_managed,
        .unbounded = config.rescue.unbounded,
    }};
    return parts.get();
}

auto rescue_coordinator() -> lyrium::policy::RescueCoordinator &
{
    return rescue_parts().coordinator;
}

}

namespace lyrium
{

auto rescue_stats() -> policy::RescueStats
{
    return rescue_coordinator().stats();
}

}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_EndScene_Hook(::PROC orig_func, void *that);
__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_SetStreamSource_hook(
    ::PROC orig_func,
    void *that,
    ::UINT StreamNumber,
    ::IDirect3DVertexBuffer9 *pStreamData,
    ::UINT OffsetInBytes,
    ::UINT Stride);
__declspec(dllexport) ::ULONG WINAPI IDirect3DVertexBuffer9_Release_hook(::PROC orig_func, void *that);
__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateVertexBuffer_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Length,
    ::DWORD Usage,
    ::DWORD FVF,
    ::D3DPOOL Pool,
    ::IDirect3DVertexBuffer9 **ppVertexBuffer,
    ::HANDLE *pSharedHandle);
__declspec(dllexport) ::ULONG WINAPI IDirect3DIndexBuffer9_Release_hook(::PROC orig_func, void *that);
__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateIndexBuffer_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Length,
    ::DWORD Usage,
    ::D3DFORMAT Format,
    ::D3DPOOL Pool,
    ::IDirect3DIndexBuffer9 **ppIndexBuffer,
    ::HANDLE *pSharedHandle);
__declspec(dllexport) ::ULONG WINAPI IDirect3DTexture9_Release_hook(::PROC orig_func, void *that);
__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateTexture_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Width,
    ::UINT Height,
    ::UINT Levels,
    ::DWORD Usage,
    ::D3DFORMAT Format,
    ::D3DPOOL Pool,
    ::IDirect3DTexture9 **ppTexture,
    ::HANDLE *pSharedHandle);
__declspec(dllexport) ::HRESULT WINAPI
IDirect3DDevice9_GetTexture_hook(::PROC orig_func, void *that, ::DWORD Stage, ::IDirect3DBaseTexture9 **ppTexture);
__declspec(dllexport) ::HRESULT WINAPI
IDirect3DDevice9_SetTexture_hook(::PROC orig_func, void *that, ::DWORD Stage, ::IDirect3DBaseTexture9 *pTexture);
__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_UpdateTexture_hook(
    ::PROC orig_func,
    void *that,
    ::IDirect3DBaseTexture9 *pSourceTexture,
    ::IDirect3DBaseTexture9 *pDestinationTexture);
__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateCubeTexture_hook(
    ::PROC orig_func,
    void *that,
    ::UINT EdgeLength,
    ::UINT Levels,
    ::DWORD Usage,
    ::D3DFORMAT Format,
    ::D3DPOOL Pool,
    ::IDirect3DCubeTexture9 **ppCubeTexture,
    ::HANDLE *pSharedHandle);
__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateVolumeTexture_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Width,
    ::UINT Height,
    ::UINT Depth,
    ::UINT Levels,
    ::DWORD Usage,
    ::D3DFORMAT Format,
    ::D3DPOOL Pool,
    ::IDirect3DVolumeTexture9 **ppVolumeTexture,
    ::HANDLE *pSharedHandle);
__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateRenderTarget_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Width,
    ::UINT Height,
    ::D3DFORMAT Format,
    ::D3DMULTISAMPLE_TYPE MultiSample,
    ::DWORD MultisampleQuality,
    ::BOOL Lockable,
    ::IDirect3DSurface9 **ppSurface,
    ::HANDLE *pSharedHandle);
__declspec(dllexport) ::HRESULT WINAPI
IDirect3DDevice9_Reset_hook(::PROC orig_func, void *that, ::D3DPRESENT_PARAMETERS *pPresentationParameters);
__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_EvictManagedResources_hook(::PROC orig_func, void *that);
__declspec(dllexport) ::HRESULT WINAPI IDirect3DStateBlock9_Release_hook(::PROC orig_func, void *that);
__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateStateBlock_hook(
    ::PROC orig_func,
    void *that,
    ::D3DSTATEBLOCKTYPE Type,
    ::IDirect3DStateBlock9 **ppSB);
__declspec(dllexport) ::HRESULT WINAPI IDirect3D9_CreateDevice_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Adapter,
    ::D3DDEVTYPE DeviceType,
    ::HWND hFocusWindow,
    ::DWORD BehaviorFlags,
    ::D3DPRESENT_PARAMETERS *pPresentationParameters,
    ::IDirect3DDevice9 **ppReturnedDeviceInterface);

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_EndScene_Hook(::PROC orig_func, void *that)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_EndScene_Hook)>;

    if (reset_report_pending.exchange(false, std::memory_order_acquire))
    {
        // Deliberately here rather than in the Reset hook: an address-space walk
        // measured 13 ms per sample, and two of them ran inside the engine's
        // driver mutex on every reset. See ResetScope for what that widens.
        if (const auto purged = reset_purged.exchange(0u, std::memory_order_relaxed); purged > 0u)
        {
            lyrium::log("device reset: purged {} idle textures from the recycler", purged);
        }
        if (const auto hr = reset_failed_hr.exchange(0u, std::memory_order_relaxed); hr != 0u)
        {
            lyrium::log("device reset failed (hr={:#010x}), deferring texture restore", hr);
        }
        lyrium::diag::Sampler::instance().sample_now("after_reset");
    }

    lyrium::overlay::poll_hotkey();
    lyrium::overlay::render(reinterpret_cast<::IDirect3DDevice9 *>(that));

    // Frame timing moved to Present. EndScene closes the scene and returns
    // without doing driver work -- measured at 2 ms total across 2237 frames of
    // a session that stuttered visibly -- so timing it answered nothing.
    //
    // Sampled once a second rather than per frame: the driver call is not free
    // and the number moves slowly. Kept even though VRAM is now ruled out --
    // 3543 MB free, 3532 MB low-water mark, across a stuttering session -- since
    // it is the evidence that rules it out and it costs almost nothing.
    static auto last_vram_us = std::int64_t{};
    if (lyrium::now_us() - last_vram_us > 1'000'000)
    {
        last_vram_us = lyrium::now_us();
        const auto free_vram =
            static_cast<std::uint64_t>(reinterpret_cast<::IDirect3DDevice9 *>(that)->GetAvailableTextureMem());
        lyrium::stats::vram_free_bytes.store(free_vram, std::memory_order_relaxed);
        auto low = lyrium::stats::vram_low_water.load(std::memory_order_relaxed);
        while (free_vram < low &&
               !lyrium::stats::vram_low_water.compare_exchange_weak(low, free_vram, std::memory_order_relaxed))
        {
        }
    }

    return reinterpret_cast<orig_call_type>(orig_func)(that);
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_SetStreamSource_hook(
    ::PROC orig_func,
    void *that,
    ::UINT StreamNumber,
    ::IDirect3DVertexBuffer9 *pStreamData,
    ::UINT OffsetInBytes,
    ::UINT Stride)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_SetStreamSource_hook)>;

    return reinterpret_cast<orig_call_type>(orig_func)(that, StreamNumber, pStreamData, OffsetInBytes, Stride);
}

__declspec(dllexport) ::ULONG WINAPI IDirect3DVertexBuffer9_Release_hook(::PROC orig_func, void *that)
{
    using orig_call_type = OrigFunc<decltype(&IDirect3DVertexBuffer9_Release_hook)>::type;

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(that);

    if (res == 0)
    {
        lyrium::stats::live_vertex_buffers.untrack(that);
    }

    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateVertexBuffer_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Length,
    ::DWORD Usage,
    ::DWORD FVF,
    ::D3DPOOL Pool,
    ::IDirect3DVertexBuffer9 **ppVertexBuffer,
    ::HANDLE *pSharedHandle)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_CreateVertexBuffer_hook)>;

    const auto res =
        reinterpret_cast<orig_call_type>(orig_func)(that, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);

    auto *buffer = (SUCCEEDED(res) && ppVertexBuffer != nullptr) ? *ppVertexBuffer : nullptr;

    if (buffer != nullptr)
    {
        com_hook.add_hook<2zu>(buffer, IDirect3DVertexBuffer9_Release_hook);
        lyrium::stats::live_vertex_buffers.track(buffer);
    }
    else if (FAILED(res))
    {
        lyrium::diag::Sampler::instance().sample_now("vertex_buffer_failed");
    }

    return res;
}

__declspec(dllexport) ::ULONG WINAPI IDirect3DIndexBuffer9_Release_hook(::PROC orig_func, void *that)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DIndexBuffer9_Release_hook)>;

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(that);

    if (res == 0)
    {
        lyrium::stats::live_index_buffers.untrack(that);
    }

    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateIndexBuffer_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Length,
    ::DWORD Usage,
    ::D3DFORMAT Format,
    ::D3DPOOL Pool,
    ::IDirect3DIndexBuffer9 **ppIndexBuffer,
    ::HANDLE *pSharedHandle)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_CreateIndexBuffer_hook)>;

    const auto res =
        reinterpret_cast<orig_call_type>(orig_func)(that, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);

    auto *buffer = (SUCCEEDED(res) && ppIndexBuffer != nullptr) ? *ppIndexBuffer : nullptr;

    if (buffer != nullptr)
    {
        com_hook.add_hook<2zu>(buffer, IDirect3DIndexBuffer9_Release_hook);
        lyrium::stats::live_index_buffers.track(buffer);
    }
    else if (FAILED(res))
    {
        lyrium::diag::Sampler::instance().sample_now("index_buffer_failed");
    }

    return res;
}

__declspec(dllexport) ::ULONG WINAPI IDirect3DTexture9_Release_hook(::PROC orig_func, void *that)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DTexture9_Release_hook)>;

    auto *texture = reinterpret_cast<::IDirect3DTexture9 *>(that);

    if (lyrium::TextureRecycler::instance().enabled())
    {
        texture->AddRef();
        const auto references = reinterpret_cast<orig_call_type>(orig_func)(that);

        if (references == 1u && lyrium::TextureRecycler::instance().retain(texture))
        {
            // Retained, not destroyed: the recycler owns it now and it still
            // occupies address space, so it stays on the ledger. Re-acquiring it
            // re-registers the same handle, which replaces the record rather
            // than adding a second one.
            return 0u;
        }
    }

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(that);

    if (res == 0)
    {
        lyrium::TextureRecycler::instance().forget(texture);
        forget_texture(that);
    }

    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateTexture_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Width,
    ::UINT Height,
    ::UINT Levels,
    ::DWORD Usage,
    ::D3DFORMAT Format,
    ::D3DPOOL Pool,
    ::IDirect3DTexture9 **ppTexture,
    ::HANDLE *pSharedHandle)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_CreateTexture_hook)>;

    const auto original = reinterpret_cast<orig_call_type>(orig_func);
    const auto bytes = lyrium::diag::texture_bytes(Width, Height, 1u, Levels, Format);

    if (!inside_device_reset() && rescue_coordinator().consider(bytes, 0u).acted)
    {
        lyrium::stats::rescue_preemptive.fetch_add(1u, std::memory_order_relaxed);
    }

    const auto placement = placement_policy().place(
        lyrium::texture::TextureDesc{
            .width = Width,
            .height = Height,
            .depth = 1u,
            .levels = Levels,
            .usage = lyrium::texture::TextureUsage{static_cast<std::uint32_t>(Usage)},
            .format = static_cast<lyrium::texture::PixelFormat>(Format),
            .pool = static_cast<lyrium::texture::TexturePool>(Pool),
        },
        bytes);

    auto pool = static_cast<::D3DPOOL>(placement.pool);
    auto pool_overridden = placement.relocated();

    const auto key = lyrium::TextureRecycler::Key{
        .width = Width,
        .height = Height,
        .levels = Levels,
        .usage = static_cast<std::uint32_t>(Usage),
        .format = static_cast<std::uint32_t>(Format),
        .pool = static_cast<std::uint32_t>(pool)};

    if (!pool_overridden && ppTexture != nullptr)
    {
        if (auto *recycled = lyrium::TextureRecycler::instance().acquire(key); recycled != nullptr)
        {
            *ppTexture = recycled;

            // A recycler hit is still a creation from the game's point of view.
            // Returning early without counting it made d3d_creates undercount,
            // hid these calls from the failure counter, and skipped the result
            // correlation the engine-side diagnostics rely on. It also made the
            // create sequence in the log jump, which is how it was noticed.
            note_create(S_OK, bytes);
            remember_texture(recycled, bytes, pool, placement.relocated());
            return S_OK;
        }
    }

    const auto create_started = lyrium::now_us();
    auto res = original(that, Width, Height, Levels, Usage, Format, pool, ppTexture, pSharedHandle);
    note_create_cost(pool, lyrium::now_us() - create_started);

    if (pool_overridden && SUCCEEDED(res) && ppTexture != nullptr && *ppTexture != nullptr)
    {
        auto *created = *ppTexture;
        auto *wrapped = lyrium::make_resettable_texture(
            reinterpret_cast<::IDirect3DDevice9 *>(that),
            original,
            created,
            lyrium::ResettableTextureShape{
                .width = Width,
                .height = Height,
                .levels = Levels,
                .usage = Usage,
                .format = Format,
            },
            forget_resettable_texture);
        if (wrapped != nullptr)
        {
            created->Release();
            *ppTexture = wrapped;
        }
        else
        {
            created->Release();
            *ppTexture = nullptr;
            res = E_OUTOFMEMORY;
        }
    }
    if (FAILED(res) && placement_policy().may_fall_back(placement))
    {
        res = original(that, Width, Height, Levels, Usage, Format, D3DPOOL_MANAGED, ppTexture, pSharedHandle);
        pool = D3DPOOL_MANAGED;
        lyrium::stats::pool_reverts.fetch_add(1u, std::memory_order_relaxed);
        pool_overridden = false;
    }

    if (pool_overridden)
    {
        lyrium::stats::pool_overrides.fetch_add(1u, std::memory_order_relaxed);
        lyrium::stats::pool_override_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    // Escalating retries. Each pass asks the coordinator for a stronger action
    // than the last, terminating in a full cache clear, so bounding the
    // preemptive path above cannot leave a real failure unanswered.
    for (auto attempt = std::uint32_t{1}; FAILED(res) && ppTexture != nullptr && attempt <= 3u; ++attempt)
    {
        if (!rescue_coordinator().consider(bytes, attempt).acted)
        {
            break;
        }

        lyrium::stats::rescue_attempts.fetch_add(1u, std::memory_order_relaxed);

        // Retry with the pool actually in effect, not the originally requested
        // one. They differ once placement has relocated the texture or the
        // managed fallback has already fired, and using the wrong one recorded
        // the result against the wrong pool.
        const auto retry = original(that, Width, Height, Levels, Usage, Format, pool, ppTexture, pSharedHandle);
        if (SUCCEEDED(retry))
        {
            lyrium::stats::rescue_successes.fetch_add(1u, std::memory_order_relaxed);
        }

        res = retry;
    }

    note_create(res, bytes);
    auto *texture = (SUCCEEDED(res) && ppTexture != nullptr) ? *ppTexture : nullptr;
    if (texture != nullptr)
    {
        remember_texture(texture, bytes, pool, pool_overridden);
        if (pool_overridden)
        {
            return res;
        }

        lyrium::TextureRecycler::instance().note_created(texture, key, bytes);
        com_hook.add_hook<2zu>(texture, IDirect3DTexture9_Release_hook);
    }

    return res;
}

__declspec(dllexport) ::HRESULT WINAPI
IDirect3DDevice9_GetTexture_hook(::PROC orig_func, void *that, ::DWORD Stage, ::IDirect3DBaseTexture9 **ppTexture)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_GetTexture_hook)>;

    const auto result = reinterpret_cast<orig_call_type>(orig_func)(that, Stage, ppTexture);
    if (SUCCEEDED(result) && ppTexture != nullptr && *ppTexture != nullptr)
    {
        *ppTexture = lyrium::rewrap_resettable_texture(*ppTexture);
    }
    return result;
}

__declspec(dllexport) ::HRESULT WINAPI
IDirect3DDevice9_SetTexture_hook(::PROC orig_func, void *that, ::DWORD Stage, ::IDirect3DBaseTexture9 *pTexture)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_SetTexture_hook)>;

    // acquire_bound_texture hands back a reference; SetTexture takes its own, so
    // ours is released as soon as the call returns.
    auto *texture = lyrium::acquire_bound_texture(pTexture);

    // A wrapper whose inner texture could not be recreated used to make this
    // return D3DERR_DEVICELOST, an error the game never asked for and which can
    // drive it into a reset loop. Binding nothing leaves the draw untextured,
    // which is survivable and recovers on the next frame.
    const auto res = reinterpret_cast<orig_call_type>(orig_func)(that, Stage, texture);
    if (texture != nullptr)
    {
        texture->Release();
    }
    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_UpdateTexture_hook(
    ::PROC orig_func,
    void *that,
    ::IDirect3DBaseTexture9 *pSourceTexture,
    ::IDirect3DBaseTexture9 *pDestinationTexture)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_UpdateTexture_hook)>;

    auto *source = lyrium::acquire_bound_texture(pSourceTexture);
    auto *destination = lyrium::acquire_bound_texture(pDestinationTexture);

    const auto release = [](::IDirect3DBaseTexture9 *texture)
    {
        if (texture != nullptr)
        {
            texture->Release();
        }
    };

    // A wrapper resolves to nothing only when it is already being destroyed or
    // when its inner texture could not be recreated under memory pressure. In
    // both cases the update has nowhere useful to go, and the shadow copy will
    // be re-uploaded on the next restore. Reporting a device loss the game never
    // caused is worse than skipping a copy it will not miss.
    if ((pSourceTexture != nullptr && source == nullptr) || (pDestinationTexture != nullptr && destination == nullptr))
    {
        release(source);
        release(destination);
        return D3D_OK;
    }

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(that, source, destination);
    release(source);
    release(destination);
    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateCubeTexture_hook(
    ::PROC orig_func,
    void *that,
    ::UINT EdgeLength,
    ::UINT Levels,
    ::DWORD Usage,
    ::D3DFORMAT Format,
    ::D3DPOOL Pool,
    ::IDirect3DCubeTexture9 **ppCubeTexture,
    ::HANDLE *pSharedHandle)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_CreateCubeTexture_hook)>;

    const auto res = reinterpret_cast<orig_call_type>(
        orig_func)(that, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
    const auto bytes = lyrium::diag::texture_bytes(EdgeLength, EdgeLength, 1u, Levels, Format, 6u);
    note_create(res, bytes);
    auto *texture = (SUCCEEDED(res) && ppCubeTexture != nullptr) ? *ppCubeTexture : nullptr;
    if (texture != nullptr)
    {
        remember_texture(texture, bytes, Pool, false);
    }

    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateVolumeTexture_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Width,
    ::UINT Height,
    ::UINT Depth,
    ::UINT Levels,
    ::DWORD Usage,
    ::D3DFORMAT Format,
    ::D3DPOOL Pool,
    ::IDirect3DVolumeTexture9 **ppVolumeTexture,
    ::HANDLE *pSharedHandle)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_CreateVolumeTexture_hook)>;

    const auto res = reinterpret_cast<orig_call_type>(
        orig_func)(that, Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
    const auto bytes = lyrium::diag::texture_bytes(Width, Height, Depth, Levels, Format);
    note_create(res, bytes);
    auto *texture = (SUCCEEDED(res) && ppVolumeTexture != nullptr) ? *ppVolumeTexture : nullptr;
    if (texture != nullptr)
    {
        remember_texture(texture, bytes, Pool, false);
    }

    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateRenderTarget_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Width,
    ::UINT Height,
    ::D3DFORMAT Format,
    ::D3DMULTISAMPLE_TYPE MultiSample,
    ::DWORD MultisampleQuality,
    ::BOOL Lockable,
    ::IDirect3DSurface9 **ppSurface,
    ::HANDLE *pSharedHandle)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_CreateRenderTarget_hook)>;

    const auto res = reinterpret_cast<orig_call_type>(
        orig_func)(that, Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
    note_create(res, lyrium::diag::texture_bytes(Width, Height, 1u, 1u, Format));
    return res;
}

__declspec(dllexport) ::HRESULT WINAPI
IDirect3DDevice9_Reset_hook(::PROC orig_func, void *that, ::D3DPRESENT_PARAMETERS *pPresentationParameters)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_Reset_hook)>;

    // Nothing slow, and nothing that runs engine code, may happen here.
    //
    // This hook executes inside D3DGraphicsDriver::ResetDevice with the engine's
    // driver mutex held. That function broadcasts to a registry of D3DResetable
    // objects, and the engine registers an object into that registry from the
    // D3DResetable base constructor and removes it from the base destructor --
    // so any texture, shader or buffer currently between its base constructor
    // and its most-derived constructor is in the list with the abstract vtable
    // still installed, where slot 2 is _purecall. A broadcast hitting it lands
    // in the CRT as R6025, which is the crash reported against this project. The
    // engine's mutex is recursive and does not prevent it.
    //
    // The race is the game's, not ours, and needs no wrapper to exist. But its
    // window is normally microseconds, and this hook used to hold it open for
    // 26 ms per reset with two full address-space walks measured at ~13 ms each,
    // turning a lottery into a near-certainty across the dozens of resets that
    // alt-tabbing produces. The samples are taken after the hook returns instead;
    // they are diagnostics and nothing depends on their timing.
    const auto reset_guard = ResetScope{};

    lyrium::overlay::before_device_reset();
    lyrium::before_resettable_texture_reset(reinterpret_cast<::IDirect3DDevice9 *>(that));

    reset_purged.store(lyrium::TextureRecycler::instance().purge(), std::memory_order_relaxed);

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(that, pPresentationParameters);

    // Only restore once the device is actually back. Recreating DEFAULT-pool
    // textures on a device whose Reset failed is guaranteed to fail too, and it
    // used to run unconditionally: every texture would attempt a creation
    // against a still-lost device, and the shadow copies stay valid regardless,
    // so the restore is simply deferred to the reset that eventually succeeds.
    if (SUCCEEDED(res))
    {
        lyrium::after_resettable_texture_reset();
        lyrium::overlay::after_device_reset();
    }
    else
    {
        reset_failed_hr.store(static_cast<std::uint32_t>(res), std::memory_order_relaxed);
    }

    reset_report_pending.store(true, std::memory_order_release);
    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_EvictManagedResources_hook(::PROC orig_func, void *that)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_EvictManagedResources_hook)>;

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(that);
    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DStateBlock9_Release_hook(::PROC orig_func, void *that)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DStateBlock9_Release_hook)>;

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(that);

    if (res == 0)
    {
        lyrium::stats::live_state_blocks.untrack(that);
    }

    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_CreateStateBlock_hook(
    ::PROC orig_func,
    void *that,
    ::D3DSTATEBLOCKTYPE Type,
    ::IDirect3DStateBlock9 **ppSB)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_CreateStateBlock_hook)>;

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(that, Type, ppSB);

    if (SUCCEEDED(res) && ppSB != nullptr && *ppSB != nullptr)
    {
        com_hook.add_hook<2zu>(*ppSB, IDirect3DStateBlock9_Release_hook);
        lyrium::stats::live_state_blocks.track(*ppSB);
    }

    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3D9_CreateDevice_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Adapter,
    ::D3DDEVTYPE DeviceType,
    ::HWND hFocusWindow,
    ::DWORD BehaviorFlags,
    ::D3DPRESENT_PARAMETERS *pPresentationParameters,
    ::IDirect3DDevice9 **ppReturnedDeviceInterface)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3D9_CreateDevice_hook)>;

    lyrium::breadcrumb("d3d CreateDevice: enter");
    const auto res = reinterpret_cast<orig_call_type>(orig_func)(
        that, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
    lyrium::log(
        "d3d CreateDevice returned hr={:#010x}, output={}",
        static_cast<std::uint32_t>(res),
        ppReturnedDeviceInterface != nullptr ? static_cast<void *>(*ppReturnedDeviceInterface) : nullptr);

    if (FAILED(res) || ppReturnedDeviceInterface == nullptr || *ppReturnedDeviceInterface == nullptr)
    {
        lyrium::breadcrumb("d3d CreateDevice: failed");
        return res;
    }

    lyrium::breadcrumb("d3d CreateDevice: installing COM hooks");
    auto *device = *ppReturnedDeviceInterface;

    com_hook.add_hook<5zu>(device, IDirect3DDevice9_EvictManagedResources_hook);
    com_hook.add_hook<16zu>(device, IDirect3DDevice9_Reset_hook);
    com_hook.add_hook<23zu>(device, IDirect3DDevice9_CreateTexture_hook);
    com_hook.add_hook<24zu>(device, IDirect3DDevice9_CreateVolumeTexture_hook);
    com_hook.add_hook<25zu>(device, IDirect3DDevice9_CreateCubeTexture_hook);
    com_hook.add_hook<26zu>(device, IDirect3DDevice9_CreateVertexBuffer_hook);
    com_hook.add_hook<27zu>(device, IDirect3DDevice9_CreateIndexBuffer_hook);
    com_hook.add_hook<28zu>(device, IDirect3DDevice9_CreateRenderTarget_hook);
    com_hook.add_hook<31zu>(device, IDirect3DDevice9_UpdateTexture_hook);
    com_hook.add_hook<42zu>(device, IDirect3DDevice9_EndScene_Hook);
    com_hook.add_hook<60zu>(device, IDirect3DDevice9_CreateStateBlock_hook);
    com_hook.add_hook<64zu>(device, IDirect3DDevice9_GetTexture_hook);
    com_hook.add_hook<65zu>(device, IDirect3DDevice9_SetTexture_hook);
    com_hook.add_hook<100zu>(device, IDirect3DDevice9_SetStreamSource_hook);

    auto identifier = ::D3DADAPTER_IDENTIFIER9{};
    auto *d3d9 = reinterpret_cast<::IDirect3D9 *>(that);
    d3d9->GetAdapterIdentifier(Adapter, 0, &identifier);

    auto caps = ::D3DCAPS9{};
    device->GetDeviceCaps(&caps);

    // The backend needs the device to evict managed resources.
    rescue_parts().backend.set_device(device);

    lyrium::breadcrumb("d3d CreateDevice: returned");
    return res;
}

extern "C"
{
::DWORD WINAPI DllMain(void *, ::DWORD, void *);

::IDirect3D9 *WINAPI Direct3DCreate9(::UINT SDKVersion)
{

    static auto initialised = false;
    if (!initialised)
    {
        initialised = true;

        config = lyrium::load_config();

        if (config.allocation_watch)
        {
            enable_allocation_watch();
        }
        else
        {
            allocation_watch_mode = "disabled";
        }

        lyrium::log_set_enabled(config.logging);

        if (config.logging)
        {
            lyrium::log_start(
                config.log_directory,
                "lyrium_" + lyrium::wall_clock("%Y%m%d_%H%M%S") + "_" + std::to_string(::GetCurrentProcessId()));
            lyrium::breadcrumb_reset(config.log_directory);
            lyrium::breadcrumb("Direct3DCreate9: logging started");
            lyrium::log("lyrium diagnostic startup, pid={}", ::GetCurrentProcessId());
            lyrium::log(
                "config: engine_hooks={}, cache_hooks={}, allocator_hooks={}, allocation_watch={}, overlay={}, "
                "texture_pool_default={}, recycler={}, sample_interval_ms={}",
                config.engine.hook_texture_paths,
                config.engine.hook_cache,
                config.engine.hook_allocator,
                config.allocation_watch,
                config.overlay,
                config.texture_pool.prefer_default,
                config.recycler.enabled,
                config.sample_interval_ms);
            // Which address-space ceiling this process actually has. Without the
            // LAA patch the game gets 2 GB; with it, up to 4 GB on 64-bit
            // Windows. Every pressure threshold means something different
            // between the two, and both are in the wild, so a session is not
            // interpretable without knowing which one produced it.
            const auto image = lyrium::diag::read_image_flags();
            if (image.valid)
            {
                lyrium::log(
                    "image: large_address_aware={} dynamic_base={} nx={} base={:#010x} delta={:#x} size={}",
                    image.large_address_aware,
                    image.dynamic_base,
                    image.nx_compatible,
                    static_cast<std::uint32_t>(image.actual_base),
                    static_cast<std::uint32_t>(image.base_delta),
                    image.size_of_image);
            }
            else
            {
                lyrium::log("image: header unreadable, address-space ceiling unknown");
            }

            lyrium::log("allocation watch: {}", allocation_watch_mode);
            lyrium::log(
                "main pool patch: {} (original={}, patched={})",
                pool_patch_result.reason != nullptr ? pool_patch_result.reason : "unknown",
                pool_patch_result.original_bytes,
                pool_patch_result.patched_bytes);
        }

        lyrium::overlay::set_visible(config.overlay);

        lyrium::TextureRecycler::instance().configure(
            config.recycler.enabled, config.recycler.budget_bytes, config.recycler.max_per_key);

        lyrium::breadcrumb("Direct3DCreate9: installing engine hooks");
        lyrium::dao::install_engine_hooks(config.engine);
        lyrium::breadcrumb("Direct3DCreate9: engine hooks installed");

        if (const auto install = lyrium::dao::engine_install_state(); install.aborted)
        {
            // Loud on purpose. A verification abort means the executable does not
            // match the table, so the mod is inert; without saying so the user
            // sees a game that simply behaves as though lyrium were not there.
            lyrium::log(
                "engine hooks: ABORTED - {} of {} targets did not verify, the process was left unmodified",
                install.failed_verification,
                install.planned);
        }
        else
        {
            lyrium::log(
                "engine hooks: {} of {} installed (base_delta={:#x})",
                install.installed,
                install.planned,
                static_cast<std::uint32_t>(install.base_delta));
        }

        lyrium::diag::Sampler::instance().set_observer(&log_ledger_snapshot);
        lyrium::diag::Sampler::instance().start(config.sample_interval_ms);
        lyrium::breadcrumb("Direct3DCreate9: sampler started");
    }

    lyrium::breadcrumb("Direct3DCreate9: loading system d3d9");
    char system_path[MAX_PATH]{};
    ::GetSystemDirectoryA(system_path, MAX_PATH);
    const auto d3d9_path = std::string{system_path} + "\\d3d9.dll";

    const auto d3d9_lib = ::LoadLibraryA(d3d9_path.c_str());
    lyrium::ensure(d3d9_lib != nullptr, "could not load {}", d3d9_path);
    lyrium::breadcrumb("Direct3DCreate9: system d3d9 loaded");

    // Only now can the import be redirected: d3d9.dll has to be in the process
    // before its import table exists to patch. Reserving here rather than at
    // attach also means the region is taken while the space is still clean.
    if (config.heap_arena_mb > 0u)
    {
        heap_arena_result = lyrium::install_heap_interposer(
            static_cast<std::uint64_t>(config.heap_arena_mb) * 1024ull * 1024ull,
            static_cast<std::size_t>(config.heap_arena_threshold_kb) * 1024u);
        lyrium::log(
            "heap arena: {} (reserved={}kb, threshold={}kb)",
            heap_arena_result.reason,
            heap_arena_result.reserved_bytes / 1024u,
            config.heap_arena_threshold_kb);
    }

    const auto direct_create =
        reinterpret_cast<decltype(&Direct3DCreate9)>(::GetProcAddress(d3d9_lib, "Direct3DCreate9"));
    lyrium::ensure(direct_create != NULL, "failed to get address of Direct3DCreate9");

    auto *d3d9 = direct_create(SDKVersion);
    lyrium::log("system Direct3DCreate9 returned {}", static_cast<void *>(d3d9));

    if (d3d9 != nullptr)
    {
        com_hook.add_hook<16zu>(d3d9, IDirect3D9_CreateDevice_hook);
        lyrium::breadcrumb("Direct3DCreate9: proxy hook installed");
    }

    return d3d9;
}

::DWORD WINAPI DllMain(void *, ::DWORD fdwReason, void *reserved)
{
    switch (fdwReason)
    {
        case DLL_PROCESS_DETACH:
        {
            // This runs for BOTH detach cases, deliberately. reserved != nullptr
            // means process termination, which for a statically imported proxy
            // like this one is the only case that ever actually happens -- so
            // putting the work behind the early break would mean it never ran.
            //
            // Every breadcrumb below opens, appends and closes its own file, so
            // it shares no state with any terminated thread. If the exit sequence
            // hangs, the last breadcrumb names the statement it hung on.
            if (lyrium::log_is_enabled())
            {
                lyrium::breadcrumb("detach: sealing log");
                lyrium::detail::LogSink::instance().begin_seal();

                lyrium::breadcrumb("detach: final sample");
                lyrium::diag::Sampler::instance().sample_now("shutdown");

                lyrium::breadcrumb("detach: sealed");
                lyrium::detail::LogSink::instance().end_seal();
            }

            if (reserved != nullptr)
            {
                break;
            }

            lyrium::dao::remove_engine_hooks();
            break;
        }
        case DLL_PROCESS_ATTACH:
        {
            // The pool patch and the compensating hooks used to be decided
            // independently, at different times: this ran unconditionally at
            // attach while the hooks were attempted much later and their result
            // discarded. A shrunk pool with no hooks is strictly worse than not
            // loading at all, so the patch now defers to the same verification
            // the hooks will use.
            if (lyrium::dao::targets_verify_clean())
            {
                pool_patch_result = lyrium::dao::patch_main_pool(main_pool_override_bytes());
            }
            else
            {
                pool_patch_result.reason = "skipped: engine targets did not verify";
            }
            break;
        }
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH: break;
    }

    return 1;
}
}
