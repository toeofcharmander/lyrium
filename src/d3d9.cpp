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


#include "lyrium/config.h"
#include "lyrium/overlay.h"
#include "lyrium/stats.h"
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
#include "lyrium/resettable_texture.h"
#include "lyrium/policy/texture_placement_policy.h"
#include "lyrium/texture/dll_texture_ledger.h"
#include "lyrium/resource_tracker.h"
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

auto remember_texture(void *texture, std::uint64_t bytes, ::D3DPOOL pool) -> void
{
    lyrium::texture_ledger().note_created(
        texture, static_cast<lyrium::texture::TexturePool>(pool), bytes);
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
    const auto count =
        lyrium::stats::d3d_creates.fetch_add(1u, std::memory_order_relaxed) + 1u;
    lyrium::dao::note_d3d_create_result(static_cast<std::int32_t>(hr), bytes);

    if (count <= 8u || count % 256u == 0u || FAILED(hr))
    {
        lyrium::log(
            "d3d create #{}: hr={:#010x}, bytes={}",
            count,
            static_cast<std::uint32_t>(hr),
            bytes);
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
const char *allocation_watch_mode{"not attempted"};

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



thread_local auto in_rescue = false;

auto reclaim(void *device) -> int
{
    const auto released = lyrium::dao::emergency_evict(INT_MAX);

    if (config.rescue.evict_managed && device != nullptr)
    {
        reinterpret_cast<::IDirect3DDevice9 *>(device)->EvictManagedResources();
    }

    return released;
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
__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_GetTexture_hook(
    ::PROC orig_func,
    void *that,
    ::DWORD Stage,
    ::IDirect3DBaseTexture9 **ppTexture);
__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_SetTexture_hook(
    ::PROC orig_func,
    void *that,
    ::DWORD Stage,
    ::IDirect3DBaseTexture9 *pTexture);
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
__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_Reset_hook(
    ::PROC orig_func,
    void *that,
    ::D3DPRESENT_PARAMETERS *pPresentationParameters);
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

    lyrium::overlay::poll_hotkey();
    lyrium::overlay::render(reinterpret_cast<::IDirect3DDevice9 *>(that));

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

    if (config.rescue.preemptive && !in_rescue && bytes >= config.rescue.large_create_bytes &&
        lyrium::dao::texture_cache_known())
    {
        const auto largest = lyrium::diag::Sampler::instance().largest_free();
        if (largest != 0u && largest < config.rescue.low_watermark_bytes)
        {
            in_rescue = true;
            lyrium::stats::rescue_preemptive.fetch_add(1u, std::memory_order_relaxed);
            reclaim(that);
            in_rescue = false;
        }
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
            remember_texture(recycled, bytes, pool);
            return S_OK;
        }
    }

    auto res = original(that, Width, Height, Levels, Usage, Format, pool, ppTexture, pSharedHandle);
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

    if (FAILED(res) && config.rescue.on_failure && !in_rescue && ppTexture != nullptr)
    {
        in_rescue = true;
        lyrium::stats::rescue_attempts.fetch_add(1u, std::memory_order_relaxed);

        reclaim(that);
        const auto retry = original(that, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
        if (SUCCEEDED(retry))
        {
            lyrium::stats::rescue_successes.fetch_add(1u, std::memory_order_relaxed);
        }

        res = retry;
        in_rescue = false;
    }

    note_create(res, bytes);
    auto *texture = (SUCCEEDED(res) && ppTexture != nullptr) ? *ppTexture : nullptr;
    if (texture != nullptr)
    {
        remember_texture(texture, bytes, pool);
        if (pool_overridden)
        {
            return res;
        }

        lyrium::TextureRecycler::instance().note_created(texture, key, bytes);
        com_hook.add_hook<2zu>(texture, IDirect3DTexture9_Release_hook);
    }

    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_GetTexture_hook(
    ::PROC orig_func,
    void *that,
    ::DWORD Stage,
    ::IDirect3DBaseTexture9 **ppTexture)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_GetTexture_hook)>;

    const auto result = reinterpret_cast<orig_call_type>(orig_func)(that, Stage, ppTexture);
    if (SUCCEEDED(result) && ppTexture != nullptr && *ppTexture != nullptr)
    {
        *ppTexture = lyrium::rewrap_resettable_texture(*ppTexture);
    }
    return result;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_SetTexture_hook(
    ::PROC orig_func,
    void *that,
    ::DWORD Stage,
    ::IDirect3DBaseTexture9 *pTexture)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_SetTexture_hook)>;

    auto *texture = lyrium::unwrap_resettable_texture(pTexture);
    if (pTexture != nullptr && texture == nullptr)
    {
        return D3DERR_DEVICELOST;
    }
    return reinterpret_cast<orig_call_type>(orig_func)(that, Stage, texture);
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_UpdateTexture_hook(
    ::PROC orig_func,
    void *that,
    ::IDirect3DBaseTexture9 *pSourceTexture,
    ::IDirect3DBaseTexture9 *pDestinationTexture)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_UpdateTexture_hook)>;

    auto *source = lyrium::unwrap_resettable_texture(pSourceTexture);
    auto *destination = lyrium::unwrap_resettable_texture(pDestinationTexture);
    if ((pSourceTexture != nullptr && source == nullptr) ||
        (pDestinationTexture != nullptr && destination == nullptr))
    {
        return D3DERR_DEVICELOST;
    }
    return reinterpret_cast<orig_call_type>(orig_func)(that, source, destination);
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
        remember_texture(texture, bytes, Pool);
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
        remember_texture(texture, bytes, Pool);
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

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(
        that, Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
    note_create(res, lyrium::diag::texture_bytes(Width, Height, 1u, 1u, Format));
    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_Reset_hook(
    ::PROC orig_func,
    void *that,
    ::D3DPRESENT_PARAMETERS *pPresentationParameters)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_Reset_hook)>;

    lyrium::overlay::before_device_reset();
    lyrium::before_resettable_texture_reset(reinterpret_cast<::IDirect3DDevice9 *>(that));

    const auto purged = lyrium::TextureRecycler::instance().purge();
    if (purged > 0u)
    {
    }

    lyrium::diag::Sampler::instance().sample_now("before_reset");

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(that, pPresentationParameters);
    lyrium::after_resettable_texture_reset();

    if (SUCCEEDED(res))
    {
        lyrium::overlay::after_device_reset();
    }

    lyrium::diag::Sampler::instance().sample_now("after_reset");

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
                "lyrium_" + lyrium::wall_clock("%Y%m%d_%H%M%S") + "_" +
                    std::to_string(::GetCurrentProcessId()));
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

            if (reserved != nullptr)
            {
                break;
            }

            lyrium::dao::remove_engine_hooks();
            break;
        }
        case DLL_PROCESS_ATTACH:
        {
            pool_patch_result = lyrium::dao::patch_main_pool(main_pool_override_bytes());
            break;
        }
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH: break;
    }

    return 1;
}
}
