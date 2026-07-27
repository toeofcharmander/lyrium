#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

#include <windows.h>

#include <d3d9.h>
#include <processthreadsapi.h>
#include <psapi.h>

#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>
#include <winnt.h>

#include "eluvian/allocators/imgui_allocator.h"
#include "eluvian/config.h"
#include "eluvian/containers/unordered_map.h"
#include "eluvian/containers/unordered_set.h"
#include "eluvian/containers/vector.h"
#include "eluvian/dao/engine_hooks.h"
#include "eluvian/dao/pool_patch.h"
#include "eluvian/diag/alloc_watch.h"
#include "eluvian/diag/process_info.h"
#include "eluvian/diag/sampler.h"
#include "eluvian/diag/texture_size.h"
#include "eluvian/diag/texture_totals.h"
#include "eluvian/diag/va_space.h"
#include "eluvian/hooks/com_hook.h"
#include "eluvian/log.h"
#include "eluvian/resource_tracker.h"
#include "eluvian/texture_recycler.h"
#include "eluvian/texture_stager.h"
#include "eluvian/utils.h"

LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using eluvian::Event;

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

auto com_hook = eluvian::COMHook{};
auto orig_wind_proc = ::WNDPROC{};

std::atomic<bool> overlay_visible{false};
auto config = eluvian::Config{};

auto tracked_vertex_buffers = eluvian::ResourceTracker<void *>{};
auto tracked_index_buffers = eluvian::ResourceTracker<void *>{};
auto tracked_textures = eluvian::ResourceTracker<void *>{};
auto tracked_state_blocks = eluvian::ResourceTracker<void *>{};

std::atomic<std::uint64_t> texture_bytes_by_pool[4]{};
std::atomic<std::uint64_t> texture_bytes_live{};
std::atomic<std::uint64_t> d3d_create_failures{};
std::atomic<std::uint64_t> d3d_creates{};

std::mutex texture_size_mutex;
eluvian::UnorderedMap<void *, std::pair<std::uint64_t, std::uint32_t>> texture_sizes;

auto pool_index(::D3DPOOL pool) -> std::size_t
{
    const auto value = static_cast<std::size_t>(pool);
    return value < 4u ? value : 0u;
}

auto remember_texture(void *texture, std::uint64_t bytes, ::D3DPOOL pool) -> void
{
    if (texture == nullptr)
    {
        return;
    }

    {
        auto lock = std::scoped_lock{texture_size_mutex};
        texture_sizes[texture] = {bytes, static_cast<std::uint32_t>(pool)};
    }

    texture_bytes_by_pool[pool_index(pool)].fetch_add(bytes, std::memory_order_relaxed);
    texture_bytes_live.fetch_add(bytes, std::memory_order_relaxed);
    eluvian::diag::note_texture_created(static_cast<std::uint32_t>(pool), bytes);
}

auto forget_texture(void *texture) -> void
{
    auto bytes = std::uint64_t{};
    auto pool = std::uint32_t{};

    {
        auto lock = std::scoped_lock{texture_size_mutex};
        const auto found = texture_sizes.find(texture);
        if (found == texture_sizes.end())
        {
            return;
        }
        bytes = found->second.first;
        pool = found->second.second;
        texture_sizes.erase(found);
    }

    texture_bytes_by_pool[pool_index(static_cast<::D3DPOOL>(pool))].fetch_sub(bytes, std::memory_order_relaxed);
    texture_bytes_live.fetch_sub(bytes, std::memory_order_relaxed);
    eluvian::diag::note_texture_released(pool, bytes);
}

auto note_create(::HRESULT hr, std::uint64_t bytes) -> void
{
    d3d_creates.fetch_add(1u, std::memory_order_relaxed);
    eluvian::dao::note_d3d_create_result(static_cast<std::int32_t>(hr), bytes);

    if (FAILED(hr))
    {
        d3d_create_failures.fetch_add(1u, std::memory_order_relaxed);
        eluvian::diag::Sampler::instance().sample_now("create_failed");
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
    ini = (slash == std::string::npos ? std::string{} : ini.substr(0, slash + 1)) + "eluvian.ini";

    const auto megabytes = ::GetPrivateProfileIntA("eluvian", "main_pool_mb", 0, ini.c_str());
    if (megabytes <= 0)
    {
        return 0u;
    }
    return static_cast<std::uint32_t>(megabytes) * 1024u * 1024u;
}

eluvian::dao::PoolPatchResult pool_patch_result{};

std::atomic<std::uint64_t> pool_overrides{};
std::atomic<std::uint64_t> pool_override_bytes{};
std::atomic<std::uint64_t> pool_reverts{};

std::atomic<std::uint64_t> rescue_attempts{};
std::atomic<std::uint64_t> rescue_successes{};
std::atomic<std::uint64_t> rescue_preemptive{};

thread_local auto in_rescue = false;

auto reclaim(void *device) -> int
{
    const auto released = eluvian::dao::emergency_evict(INT_MAX);

    if (config.rescue.evict_managed && device != nullptr)
    {
        reinterpret_cast<::IDirect3DDevice9 *>(device)->EvictManagedResources();
    }

    return released;
}

::LRESULT WINAPI wind_proc(const ::HWND hWnd, ::UINT uMsg, ::WPARAM wParam, ::LPARAM lParam)
{

    if (overlay_visible.load(std::memory_order_relaxed) &&
        ::ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
    {
        return true;
    }
    return ::CallWindowProc(orig_wind_proc, hWnd, uMsg, wParam, lParam);
}

auto megabytes(std::uint64_t bytes) -> float
{
    return static_cast<float>(bytes) / (1024.0f * 1024.0f);
}

}

namespace eluvian::diag
{

auto texture_pool_overrides() -> std::uint64_t
{
    return pool_overrides.load(std::memory_order_relaxed);
}

auto texture_pool_override_bytes() -> std::uint64_t
{
    return pool_override_bytes.load(std::memory_order_relaxed);
}

auto texture_pool_reverts() -> std::uint64_t
{
    return pool_reverts.load(std::memory_order_relaxed);
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
__declspec(dllexport) ::HRESULT WINAPI IDirect3DTexture9_LockRect_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Level,
    ::D3DLOCKED_RECT *pLockedRect,
    const ::RECT *pRect,
    ::DWORD Flags);
__declspec(dllexport) ::HRESULT WINAPI IDirect3DTexture9_UnlockRect_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Level);
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

    {
        static auto previously_held = false;
        const auto held = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 &&
                          (::GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (held && !previously_held)
        {
            const auto now_visible = !overlay_visible.load(std::memory_order_relaxed);
            overlay_visible.store(now_visible, std::memory_order_relaxed);
        }
        previously_held = held;
    }

    if (!overlay_visible.load(std::memory_order_relaxed))
    {
        return reinterpret_cast<orig_call_type>(orig_func)(that);
    }

    [[maybe_unused]] static auto initialised = [that]()
    {
        auto *device = reinterpret_cast<::LPDIRECT3DDEVICE9>(that);

        ::ImGui::SetAllocatorFunctions(eluvian::imgui_allocator, eluvian::imgui_deallocator, nullptr);

        auto params = ::D3DDEVICE_CREATION_PARAMETERS{};
        device->GetCreationParameters(&params);
        const auto window = params.hFocusWindow;

        orig_wind_proc = reinterpret_cast<::WNDPROC>(
            ::SetWindowLongPtr(window, GWLP_WNDPROC, reinterpret_cast<::LONG_PTR>(wind_proc)));

        IMGUI_CHECKVERSION();
        ::ImGui::CreateContext();

        auto &io = ::ImGui::GetIO();
        io.ConfigFlags |= ::ImGuiConfigFlags_DockingEnable;

        ::ImGui_ImplWin32_Init(window);
        ::ImGui_ImplDX9_Init(device);

        return true;
    }();

    ::ImGui_ImplDX9_NewFrame();
    ::ImGui_ImplWin32_NewFrame();
    ::ImGui::NewFrame();

    ::ImGui::DockSpaceOverViewport(0, ::ImGui::GetMainViewport(), ::ImGuiDockNodeFlags_PassthruCentralNode);

    ::ImGui::Begin("Address space");

    const auto largest_free = eluvian::diag::Sampler::instance().largest_free();
    const auto total_free = eluvian::diag::Sampler::instance().total_free();

    static auto largest_free_samples = eluvian::Vector<float>(600u);
    static auto last_plotted = std::uint64_t{};
    if (largest_free != last_plotted)
    {
        last_plotted = largest_free;
        largest_free_samples.erase(std::ranges::begin(largest_free_samples));
        largest_free_samples.push_back(megabytes(largest_free));
    }

    ::ImGui::Text("largest contiguous free block: %.1f MB", megabytes(largest_free));
    ::ImGui::Text("total free: %.1f MB", megabytes(total_free));
    ::ImGui::PlotLines(
        "largest block (MB)",
        largest_free_samples.data(),
        largest_free_samples.size(),
        0,
        nullptr,
        0.0f,
        std::numeric_limits<float>::max(),
        ::ImVec2(0, 80.0f));

    ::ImGui::Separator();
    ::ImGui::Text(
        "texture memory: %.1f MB live (managed %.1f MB, default %.1f MB)",
        megabytes(texture_bytes_live.load(std::memory_order_relaxed)),
        megabytes(texture_bytes_by_pool[D3DPOOL_MANAGED].load(std::memory_order_relaxed)),
        megabytes(texture_bytes_by_pool[D3DPOOL_DEFAULT].load(std::memory_order_relaxed)));
    ::ImGui::Text(
        "d3d creates: %llu (%llu failed)",
        static_cast<unsigned long long>(d3d_creates.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(d3d_create_failures.load(std::memory_order_relaxed)));

    const auto attempts = rescue_attempts.load(std::memory_order_relaxed);
    if (attempts > 0u || rescue_preemptive.load(std::memory_order_relaxed) > 0u)
    {
        const auto recovered = rescue_successes.load(std::memory_order_relaxed);
        ::ImGui::TextColored(
            recovered == attempts ? ::ImVec4{0.4f, 1.0f, 0.4f, 1.0f} : ::ImVec4{1.0f, 0.8f, 0.3f, 1.0f},
            "rescues: %llu/%llu recovered, %llu preemptive",
            static_cast<unsigned long long>(recovered),
            static_cast<unsigned long long>(attempts),
            static_cast<unsigned long long>(rescue_preemptive.load(std::memory_order_relaxed)));
    }
    ::ImGui::Text("live textures: %zu", tracked_textures.live_count());
    ::ImGui::Text(
        "live vb/ib/state blocks: %zu / %zu / %zu",
        tracked_vertex_buffers.live_count(),
        tracked_index_buffers.live_count(),
        tracked_state_blocks.live_count());

    ::ImGui::Separator();
    const auto engine = eluvian::dao::engine_state();
    ::ImGui::Text("texture loads: %llu", static_cast<unsigned long long>(engine.texture_loads));
    ::ImGui::Text(
        "engine creates: %llu (%llu returned null)",
        static_cast<unsigned long long>(engine.engine_texture_creates),
        static_cast<unsigned long long>(engine.engine_texture_failures));
    if (engine.suspect_textures > 0u)
    {
        ::ImGui::TextColored(
            ::ImVec4{1.0f, 0.3f, 0.3f, 1.0f},
            "SUSPECT textures (built on a failed d3d call): %llu",
            static_cast<unsigned long long>(engine.suspect_textures));
    }
    if (engine.cache_readable)
    {
        ::ImGui::Text("cache texture memory: %d", engine.texture_memory);
        ::ImGui::Text("cache pending releases: %d", engine.pending_releases);
    }
    ::ImGui::Text(
        "evictions: %llu releasing %llu textures",
        static_cast<unsigned long long>(engine.evictions),
        static_cast<unsigned long long>(engine.evicted_textures));
    if (engine.large_allocations > 0u)
    {
        ::ImGui::Text(
            "large allocations: %llu (%.1f MB live in %llu blocks)",
            static_cast<unsigned long long>(engine.large_allocations),
            megabytes(engine.large_allocation_bytes_live),
            static_cast<unsigned long long>(engine.large_allocations_live));
    }

    ::ImGui::End();

    ::ImGui::EndFrame();
    ::ImGui::Render();
    ::ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

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
        tracked_vertex_buffers.untrack(that);
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
        tracked_vertex_buffers.track(buffer);
    }
    else if (FAILED(res))
    {
        eluvian::diag::Sampler::instance().sample_now("vertex_buffer_failed");
    }

    return res;
}

__declspec(dllexport) ::ULONG WINAPI IDirect3DIndexBuffer9_Release_hook(::PROC orig_func, void *that)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DIndexBuffer9_Release_hook)>;

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(that);

    if (res == 0)
    {
        tracked_index_buffers.untrack(that);
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
        tracked_index_buffers.track(buffer);
    }
    else if (FAILED(res))
    {
        eluvian::diag::Sampler::instance().sample_now("index_buffer_failed");
    }

    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DTexture9_LockRect_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Level,
    ::D3DLOCKED_RECT *pLockedRect,
    const ::RECT *pRect,
    ::DWORD Flags)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DTexture9_LockRect_hook)>;

    auto *texture = reinterpret_cast<::IDirect3DTexture9 *>(that);
    if (eluvian::TextureStager::instance().is_staged(texture))
    {
        const auto hr =
            eluvian::TextureStager::instance().lock_rect(texture, Level, pLockedRect, pRect, Flags);
        if (SUCCEEDED(hr))
        {
            return hr;
        }

    }

    return reinterpret_cast<orig_call_type>(orig_func)(that, Level, pLockedRect, pRect, Flags);
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DTexture9_UnlockRect_hook(
    ::PROC orig_func,
    void *that,
    ::UINT Level)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DTexture9_UnlockRect_hook)>;

    auto *texture = reinterpret_cast<::IDirect3DTexture9 *>(that);
    if (eluvian::TextureStager::instance().is_staged(texture))
    {
        return eluvian::TextureStager::instance().unlock_rect(texture, Level);
    }

    return reinterpret_cast<orig_call_type>(orig_func)(that, Level);
}

__declspec(dllexport) ::ULONG WINAPI IDirect3DTexture9_Release_hook(::PROC orig_func, void *that)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DTexture9_Release_hook)>;

    auto *texture = reinterpret_cast<::IDirect3DTexture9 *>(that);

    if (eluvian::TextureRecycler::instance().enabled())
    {
        texture->AddRef();
        const auto references = reinterpret_cast<orig_call_type>(orig_func)(that);

        if (references == 1u && eluvian::TextureRecycler::instance().retain(texture))
        {

            tracked_textures.untrack(that);
            return 0u;
        }
    }

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(that);

    if (res == 0)
    {
        eluvian::TextureRecycler::instance().forget(texture);
        eluvian::TextureStager::instance().forget(texture);
        if (tracked_textures.untrack(that))
        {
            forget_texture(that);
        }
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
    const auto bytes = eluvian::diag::texture_bytes(Width, Height, 1u, Levels, Format);

    if (config.rescue.preemptive && !in_rescue && bytes >= config.rescue.large_create_bytes &&
        eluvian::dao::texture_cache_known())
    {
        const auto largest = eluvian::diag::Sampler::instance().largest_free();
        if (largest != 0u && largest < config.rescue.low_watermark_bytes)
        {
            in_rescue = true;
            rescue_preemptive.fetch_add(1u, std::memory_order_relaxed);
            reclaim(that);
            in_rescue = false;
        }
    }

    auto pool = Pool;
    auto pool_overridden = false;
    if (config.texture_pool.prefer_default && Pool == D3DPOOL_MANAGED &&
        bytes >= config.texture_pool.minimum_bytes &&
        (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC)) == 0)
    {
        pool = D3DPOOL_DEFAULT;
        pool_overridden = true;
    }

    const auto key = eluvian::TextureRecycler::Key{
        .width = Width,
        .height = Height,
        .levels = Levels,
        .usage = static_cast<std::uint32_t>(Usage),
        .format = static_cast<std::uint32_t>(Format),
        .pool = static_cast<std::uint32_t>(pool)};

    if (ppTexture != nullptr)
    {
        if (auto *recycled = eluvian::TextureRecycler::instance().acquire(key); recycled != nullptr)
        {
            *ppTexture = recycled;
            tracked_textures.track(recycled);
            remember_texture(recycled, bytes, pool);
            return S_OK;
        }
    }

    auto res = original(that, Width, Height, Levels, Usage, Format, pool, ppTexture, pSharedHandle);
    if (pool_overridden && FAILED(res) && config.texture_pool.fall_back_to_managed)
    {
        res = original(that, Width, Height, Levels, Usage, Format, D3DPOOL_MANAGED, ppTexture, pSharedHandle);
        pool = D3DPOOL_MANAGED;
        pool_reverts.fetch_add(1u, std::memory_order_relaxed);
        pool_overridden = false;
    }

    if (pool_overridden)
    {
        pool_overrides.fetch_add(1u, std::memory_order_relaxed);
        pool_override_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    if (FAILED(res) && config.rescue.on_failure && !in_rescue && ppTexture != nullptr)
    {
        in_rescue = true;
        rescue_attempts.fetch_add(1u, std::memory_order_relaxed);

        reclaim(that);
        const auto retry = original(that, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
        if (SUCCEEDED(retry))
        {
            rescue_successes.fetch_add(1u, std::memory_order_relaxed);
        }

        res = retry;
        in_rescue = false;
    }

    note_create(res, bytes);
    auto *texture = (SUCCEEDED(res) && ppTexture != nullptr) ? *ppTexture : nullptr;
    if (texture != nullptr)
    {
        tracked_textures.track(texture);
        remember_texture(texture, bytes, pool);
        eluvian::TextureRecycler::instance().note_created(texture, key, bytes);
        com_hook.add_hook<2zu>(texture, IDirect3DTexture9_Release_hook);

        if (pool_overridden)
        {

            eluvian::TextureStager::instance().track(
                texture,
                eluvian::TextureStager::Shape{
                    .width = Width, .height = Height, .levels = Levels, .format = Format});
            com_hook.add_hook<19zu>(texture, IDirect3DTexture9_LockRect_hook);
            com_hook.add_hook<20zu>(texture, IDirect3DTexture9_UnlockRect_hook);
        }
    }

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
    const auto bytes = eluvian::diag::texture_bytes(EdgeLength, EdgeLength, 1u, Levels, Format, 6u);
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
    const auto bytes = eluvian::diag::texture_bytes(Width, Height, Depth, Levels, Format);
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
    note_create(res, eluvian::diag::texture_bytes(Width, Height, 1u, 1u, Format));
    return res;
}

__declspec(dllexport) ::HRESULT WINAPI IDirect3DDevice9_Reset_hook(
    ::PROC orig_func,
    void *that,
    ::D3DPRESENT_PARAMETERS *pPresentationParameters)
{
    using orig_call_type = OrigFuncType<decltype(&IDirect3DDevice9_Reset_hook)>;

    const auto purged = eluvian::TextureRecycler::instance().purge();
    if (purged > 0u)
    {
    }

    eluvian::diag::Sampler::instance().sample_now("before_reset");

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(that, pPresentationParameters);

    eluvian::diag::Sampler::instance().sample_now("after_reset");

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
        tracked_state_blocks.untrack(that);
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
        tracked_state_blocks.track(*ppSB);
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

    const auto res = reinterpret_cast<orig_call_type>(orig_func)(
        that, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);

    if (FAILED(res) || ppReturnedDeviceInterface == nullptr || *ppReturnedDeviceInterface == nullptr)
    {
        return res;
    }

    auto *device = *ppReturnedDeviceInterface;

    com_hook.add_hook<5zu>(device, IDirect3DDevice9_EvictManagedResources_hook);
    com_hook.add_hook<16zu>(device, IDirect3DDevice9_Reset_hook);
    com_hook.add_hook<23zu>(device, IDirect3DDevice9_CreateTexture_hook);
    com_hook.add_hook<24zu>(device, IDirect3DDevice9_CreateVolumeTexture_hook);
    com_hook.add_hook<25zu>(device, IDirect3DDevice9_CreateCubeTexture_hook);
    com_hook.add_hook<26zu>(device, IDirect3DDevice9_CreateVertexBuffer_hook);
    com_hook.add_hook<27zu>(device, IDirect3DDevice9_CreateIndexBuffer_hook);
    com_hook.add_hook<28zu>(device, IDirect3DDevice9_CreateRenderTarget_hook);
    com_hook.add_hook<42zu>(device, IDirect3DDevice9_EndScene_Hook);
    com_hook.add_hook<60zu>(device, IDirect3DDevice9_CreateStateBlock_hook);
    com_hook.add_hook<100zu>(device, IDirect3DDevice9_SetStreamSource_hook);

    auto identifier = ::D3DADAPTER_IDENTIFIER9{};
    auto *d3d9 = reinterpret_cast<::IDirect3D9 *>(that);
    d3d9->GetAdapterIdentifier(Adapter, 0, &identifier);

    auto caps = ::D3DCAPS9{};
    device->GetDeviceCaps(&caps);

    eluvian::TextureStager::instance().configure(config.texture_pool.prefer_default, device);

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

        config = eluvian::load_config();

        eluvian::log_set_enabled(config.logging);

        if (config.logging)
        {
            eluvian::log_start(
                config.log_directory,
                "eluvian_" + eluvian::wall_clock("%Y%m%d_%H%M%S") + "_" +
                    std::to_string(::GetCurrentProcessId()));
        }

        overlay_visible.store(config.overlay, std::memory_order_relaxed);

        eluvian::TextureRecycler::instance().configure(
            config.recycler.enabled, config.recycler.budget_bytes, config.recycler.max_per_key);

        eluvian::dao::install_engine_hooks(config.engine);

        eluvian::diag::Sampler::instance().start(config.sample_interval_ms);

    }

    char system_path[MAX_PATH]{};
    ::GetSystemDirectoryA(system_path, MAX_PATH);
    const auto d3d9_path = std::string{system_path} + "\\d3d9.dll";

    const auto d3d9_lib = ::LoadLibraryA(d3d9_path.c_str());
    eluvian::ensure(d3d9_lib != nullptr, "could not load {}", d3d9_path);

    const auto direct_create =
        reinterpret_cast<decltype(&Direct3DCreate9)>(::GetProcAddress(d3d9_lib, "Direct3DCreate9"));
    eluvian::ensure(direct_create != NULL, "failed to get address of Direct3DCreate9");

    auto *d3d9 = direct_create(SDKVersion);

    if (d3d9 != nullptr)
    {
        com_hook.add_hook<16zu>(d3d9, IDirect3D9_CreateDevice_hook);
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

            eluvian::dao::remove_engine_hooks();
            break;
        }
        case DLL_PROCESS_ATTACH:
        {

            static constexpr auto threshold = 8ull * 1024ull * 1024ull;

            if (!eluvian::diag::install_nt_alloc_hook(threshold) &&
                !eluvian::diag::install_virtual_alloc_hook(threshold))
            {
                eluvian::diag::install_alloc_watch(threshold);
            }

            pool_patch_result = eluvian::dao::patch_main_pool(main_pool_override_bytes());
            break;
        }
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH: break;
    }

    return 1;
}
}
