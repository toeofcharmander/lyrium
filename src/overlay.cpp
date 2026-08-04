#include "lyrium/overlay.h"

#include <atomic>
#include <cstdint>
#include <limits>

#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include "lyrium/allocators/imgui_allocator.h"
#include "lyrium/containers/vector.h"
#include "lyrium/dao/engine_hooks.h"
#include "lyrium/diag/sampler.h"
#include "lyrium/diag/texture_totals.h"
#include "lyrium/stats.h"

LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace lyrium::overlay
{

namespace
{

std::atomic<bool> is_visible{false};
::WNDPROC original_window_proc{};
bool backend_initialised{false};

auto megabytes(std::uint64_t bytes) -> float
{
    return static_cast<float>(bytes) / (1024.0f * 1024.0f);
}

auto ensure_initialised(::IDirect3DDevice9 *device) -> void
{
    if (backend_initialised)
    {
        return;
    }

    ::ImGui::SetAllocatorFunctions(imgui_allocator, imgui_deallocator, nullptr);

    auto params = ::D3DDEVICE_CREATION_PARAMETERS{};
    device->GetCreationParameters(&params);
    const auto window = params.hFocusWindow;

    original_window_proc = reinterpret_cast<::WNDPROC>(
        ::SetWindowLongPtr(window, GWLP_WNDPROC, reinterpret_cast<::LONG_PTR>(&window_proc)));

    IMGUI_CHECKVERSION();
    ::ImGui::CreateContext();

    auto &io = ::ImGui::GetIO();
    io.ConfigFlags |= ::ImGuiConfigFlags_DockingEnable;

    ::ImGui_ImplWin32_Init(window);
    ::ImGui_ImplDX9_Init(device);
    backend_initialised = true;
}

auto draw_address_space() -> void
{
    const auto largest_free = diag::Sampler::instance().largest_free();
    const auto total_free = diag::Sampler::instance().total_free();

    static auto history = Vector<float>(600u);
    static auto last_plotted = std::uint64_t{};
    if (largest_free != last_plotted)
    {
        last_plotted = largest_free;
        history.erase(std::ranges::begin(history));
        history.push_back(megabytes(largest_free));
    }

    ::ImGui::Text("largest contiguous free block: %.1f MB", megabytes(largest_free));
    ::ImGui::Text("total free: %.1f MB", megabytes(total_free));
    ::ImGui::PlotLines(
        "largest block (MB)",
        history.data(),
        history.size(),
        0,
        nullptr,
        0.0f,
        std::numeric_limits<float>::max(),
        ::ImVec2(0, 80.0f));
}

auto draw_textures() -> void
{
    const auto totals = diag::texture_totals();

    ::ImGui::Text(
        "texture memory: %.1f MB live (managed %.1f MB, default %.1f MB)",
        megabytes(stats::texture_bytes_live.load(std::memory_order_relaxed)),
        megabytes(stats::texture_bytes_by_pool[D3DPOOL_MANAGED].load(std::memory_order_relaxed)),
        megabytes(stats::texture_bytes_by_pool[D3DPOOL_DEFAULT].load(std::memory_order_relaxed)));
    ::ImGui::Text("peak: %.1f MB", megabytes(totals.peak));
    ::ImGui::Text(
        "d3d creates: %llu (%llu failed)",
        static_cast<unsigned long long>(stats::d3d_creates.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(stats::d3d_create_failures.load(std::memory_order_relaxed)));
    ::ImGui::Text("live textures: %zu", stats::live_textures.live_count());
    ::ImGui::Text(
        "live vb/ib/state blocks: %zu / %zu / %zu",
        stats::live_vertex_buffers.live_count(),
        stats::live_index_buffers.live_count(),
        stats::live_state_blocks.live_count());
}

auto draw_staging() -> void
{
    const auto moved = stats::pool_overrides.load(std::memory_order_relaxed);
    if (moved == 0u)
    {
        ::ImGui::TextDisabled("staging inactive (textures left in the managed pool)");
        return;
    }

    ::ImGui::Text(
        "moved out of managed pool: %llu textures, %.1f MB",
        static_cast<unsigned long long>(moved),
        megabytes(stats::pool_override_bytes.load(std::memory_order_relaxed)));
    if (const auto reverts = stats::pool_reverts.load(std::memory_order_relaxed); reverts > 0u)
    {
        ::ImGui::TextColored(
            ::ImVec4{1.0f, 0.8f, 0.3f, 1.0f},
            "fell back to managed: %llu",
            static_cast<unsigned long long>(reverts));
    }
}

auto draw_engine() -> void
{
    const auto engine = dao::engine_state();

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
}

}

auto set_visible(bool value) -> void
{
    is_visible.store(value, std::memory_order_relaxed);
}

auto visible() -> bool
{
    return is_visible.load(std::memory_order_relaxed);
}

auto poll_hotkey() -> void
{
    static auto previously_held = false;
    const auto held = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 &&
                      (::GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (held && !previously_held)
    {
        set_visible(!visible());
    }
    previously_held = held;
}

auto render(::IDirect3DDevice9 *device) -> void
{
    if (!visible() || device == nullptr)
    {
        return;
    }

    ensure_initialised(device);

    ::ImGui_ImplDX9_NewFrame();
    ::ImGui_ImplWin32_NewFrame();
    ::ImGui::NewFrame();

    ::ImGui::DockSpaceOverViewport(0, ::ImGui::GetMainViewport(), ::ImGuiDockNodeFlags_PassthruCentralNode);

    ::ImGui::Begin("lyrium");

    if (::ImGui::CollapsingHeader("address space", ::ImGuiTreeNodeFlags_DefaultOpen))
    {
        draw_address_space();
    }
    if (::ImGui::CollapsingHeader("textures", ::ImGuiTreeNodeFlags_DefaultOpen))
    {
        draw_textures();
    }
    if (::ImGui::CollapsingHeader("staging", ::ImGuiTreeNodeFlags_DefaultOpen))
    {
        draw_staging();
    }
    if (::ImGui::CollapsingHeader("engine"))
    {
        draw_engine();
    }

    ::ImGui::End();

    ::ImGui::EndFrame();
    ::ImGui::Render();
    ::ImGui_ImplDX9_RenderDrawData(::ImGui::GetDrawData());
}

auto before_device_reset() -> void
{
    if (backend_initialised)
    {
        ::ImGui_ImplDX9_InvalidateDeviceObjects();
    }
}

auto after_device_reset() -> void
{
    if (backend_initialised)
    {
        ::ImGui_ImplDX9_CreateDeviceObjects();
    }
}

auto window_proc(::HWND window, ::UINT message, ::WPARAM wparam, ::LPARAM lparam) -> ::LRESULT
{
    if (visible() && ::ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam))
    {
        return true;
    }
    return ::CallWindowProc(original_window_proc, window, message, wparam, lparam);
}

}
