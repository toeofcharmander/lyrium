#include "lyrium/overlay.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include "lyrium/allocators/imgui_allocator.h"
#include "lyrium/containers/vector.h"
#include "lyrium/dao/engine_hooks.h"
#include "lyrium/diag/sampler.h"
#include "lyrium/rescue_access.h"
#include "lyrium/stats.h"
#include "lyrium/texture/dll_texture_ledger.h"

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

// ---------------------------------------------------------------------------
// Red lyrium styling.
//
// Severity is carried by light rather than by a label: the panel sits deep and
// dim while the address space is healthy and runs hot as it degrades. That works
// because nothing is bright until something is wrong, which is also what keeps
// red usable as an alarm colour in a UI that is otherwise entirely red.
//
// Hard edges throughout. Red lyrium is jagged crystal, and ImGui's default
// rounding is the single thing that most made this look like a generic debug
// panel.
// ---------------------------------------------------------------------------

constexpr auto crystal_dim = ::ImVec4{0.42f, 0.08f, 0.11f, 1.0f};
constexpr auto crystal_lit = ::ImVec4{0.85f, 0.19f, 0.17f, 1.0f};
constexpr auto crystal_hot = ::ImVec4{1.00f, 0.32f, 0.25f, 1.0f};
constexpr auto crystal_core = ::ImVec4{1.00f, 0.81f, 0.77f, 1.0f};
constexpr auto crystal_ash = ::ImVec4{0.54f, 0.38f, 0.40f, 1.0f};

auto apply_style() -> void
{
    auto &style = ::ImGui::GetStyle();

    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = ::ImVec2{10.0f, 9.0f};
    style.ItemSpacing = ::ImVec2{8.0f, 7.0f};

    auto *c = style.Colors;
    c[::ImGuiCol_WindowBg] = ::ImVec4{0.045f, 0.026f, 0.030f, 0.95f};
    c[::ImGuiCol_ChildBg] = ::ImVec4{0.0f, 0.0f, 0.0f, 0.0f};
    c[::ImGuiCol_Border] = ::ImVec4{0.55f, 0.10f, 0.13f, 0.85f};
    c[::ImGuiCol_BorderShadow] = ::ImVec4{0.0f, 0.0f, 0.0f, 0.0f};
    c[::ImGuiCol_Text] = ::ImVec4{0.94f, 0.85f, 0.84f, 1.0f};
    c[::ImGuiCol_TextDisabled] = crystal_ash;
    c[::ImGuiCol_FrameBg] = ::ImVec4{0.13f, 0.040f, 0.052f, 1.0f};
    c[::ImGuiCol_FrameBgHovered] = ::ImVec4{0.22f, 0.06f, 0.08f, 1.0f};
    c[::ImGuiCol_FrameBgActive] = ::ImVec4{0.30f, 0.08f, 0.10f, 1.0f};
    c[::ImGuiCol_TitleBg] = ::ImVec4{0.16f, 0.045f, 0.058f, 1.0f};
    c[::ImGuiCol_TitleBgActive] = ::ImVec4{0.30f, 0.065f, 0.085f, 1.0f};
    c[::ImGuiCol_TitleBgCollapsed] = ::ImVec4{0.10f, 0.03f, 0.04f, 0.85f};
    c[::ImGuiCol_Header] = ::ImVec4{0.34f, 0.07f, 0.09f, 0.85f};
    c[::ImGuiCol_HeaderHovered] = ::ImVec4{0.52f, 0.11f, 0.13f, 0.9f};
    c[::ImGuiCol_HeaderActive] = ::ImVec4{0.66f, 0.14f, 0.15f, 1.0f};
    c[::ImGuiCol_Separator] = ::ImVec4{0.42f, 0.09f, 0.11f, 0.7f};
    c[::ImGuiCol_ScrollbarBg] = ::ImVec4{0.08f, 0.03f, 0.035f, 0.9f};
    c[::ImGuiCol_ScrollbarGrab] = crystal_dim;
    c[::ImGuiCol_ScrollbarGrabHovered] = crystal_lit;
    c[::ImGuiCol_ResizeGrip] = ::ImVec4{0.42f, 0.09f, 0.11f, 0.5f};
    c[::ImGuiCol_ResizeGripHovered] = crystal_lit;
}

// How hot the panel runs, 0 healthy to 1 critical, from the headroom that
// actually moves. Everything visual is derived from this one number so the whole
// panel brightens together rather than in pieces.
auto heat_from(std::uint64_t headroom_bytes) -> float
{
    constexpr auto comfortable = 384.0f; // MB, well clear of any threshold
    constexpr auto floor_mb = 32.0f;     // MB, where the rescue arms

    const auto mb = megabytes(headroom_bytes);
    if (mb >= comfortable)
    {
        return 0.0f;
    }
    if (mb <= floor_mb)
    {
        return 1.0f;
    }
    return 1.0f - ((mb - floor_mb) / (comfortable - floor_mb));
}

auto lerp(const ::ImVec4 &a, const ::ImVec4 &b, float t) -> ::ImVec4
{
    return ::ImVec4{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

// The colour the whole panel keys off.
auto heat_colour(float heat) -> ::ImVec4
{
    return heat < 0.5f ? lerp(crystal_dim, crystal_lit, heat * 2.0f)
                       : lerp(crystal_lit, crystal_core, (heat - 0.5f) * 2.0f);
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

    apply_style();

    ::ImGui_ImplWin32_Init(window);
    ::ImGui_ImplDX9_Init(device);
    backend_initialised = true;
}

// Draws the headroom meter: a bar that fills with how much contiguous room is
// left, with a tick where the rescue arms. Seeing yourself approach the
// threshold is worth more than being told after it fires.
auto draw_meter(float fraction, float arm_fraction, float heat) -> void
{
    auto *draw = ::ImGui::GetWindowDrawList();
    const auto origin = ::ImGui::GetCursorScreenPos();
    const auto width = ::ImGui::GetContentRegionAvail().x;
    constexpr auto height = 9.0f;

    const auto top_left = origin;
    const auto bottom_right = ::ImVec2{origin.x + width, origin.y + height};

    draw->AddRectFilled(top_left, bottom_right, ::ImGui::GetColorU32(::ImVec4{0.11f, 0.035f, 0.045f, 1.0f}));

    const auto filled = width * (fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction));
    if (filled > 1.0f)
    {
        // Dim at the tail, hot at the leading edge, so the bar reads as a glow
        // rather than a flat block.
        draw->AddRectFilledMultiColor(
            ::ImVec2{top_left.x + 1.0f, top_left.y + 1.0f},
            ::ImVec2{top_left.x + filled, bottom_right.y - 1.0f},
            ::ImGui::GetColorU32(lerp(crystal_dim, crystal_lit, heat)),
            ::ImGui::GetColorU32(heat_colour(heat)),
            ::ImGui::GetColorU32(heat_colour(heat)),
            ::ImGui::GetColorU32(lerp(crystal_dim, crystal_lit, heat)));
    }

    const auto arm_x = top_left.x + width * arm_fraction;
    draw->AddLine(
        ::ImVec2{arm_x, top_left.y - 2.0f},
        ::ImVec2{arm_x, bottom_right.y + 2.0f},
        ::ImGui::GetColorU32(crystal_core),
        1.0f);

    draw->AddRect(top_left, bottom_right, ::ImGui::GetColorU32(::ImVec4{0.36f, 0.08f, 0.10f, 1.0f}));
    ::ImGui::Dummy(::ImVec2{width, height + 4.0f});
}

// Fragmentation, drawn as the distribution of free-block sizes.
//
// The first attempt drew the largest blocks to scale and was actively
// misleading: one enormous untouched region dominated the bar, and showing only
// the biggest blocks hides fragmentation by construction, because the big ones
// are the healthy ones. What matters is the shape of the distribution. The same
// free bytes migrating from a few large blocks into hundreds of small ones is
// precisely the failure, and it shows here as mass moving to the right.
auto draw_fragmentation(const diag::FreeSpaceSnapshot &snapshot, float heat) -> void
{
    // Differenced by the same code the walk accumulates with, so the two can no
    // longer disagree about threshold order. That disagreement is what made the
    // first histogram draw a healthy space as a shattered one.
    const auto classes = diag::free_size_classes(snapshot.size_buckets, snapshot.free_regions);

    auto peak = std::uint32_t{1};
    for (const auto count : classes)
    {
        peak = count > peak ? count : peak;
    }

    auto *draw = ::ImGui::GetWindowDrawList();
    const auto origin = ::ImGui::GetCursorScreenPos();
    const auto width = ::ImGui::GetContentRegionAvail().x;
    constexpr auto height = 34.0f;
    const auto slot = width / static_cast<float>(classes.size());

    draw->AddRectFilled(
        origin,
        ::ImVec2{origin.x + width, origin.y + height},
        ::ImGui::GetColorU32(::ImVec4{0.10f, 0.032f, 0.042f, 1.0f}));

    for (auto i = std::size_t{0}; i < classes.size(); ++i)
    {
        // An empty class draws nothing at all. A minimum bar height would make
        // "no blocks this size" and "one block this size" look alike, and which
        // classes are empty is the reading -- with four blocks over 4 MB, most of
        // this axis should be visibly bare.
        if (classes[i] == 0u)
        {
            continue;
        }

        // Logarithmic, because the counts span two or three orders of magnitude
        // in any real session -- 329 slivers against 6 usable blocks was the
        // reading that showed a linear scale draws every honest bar as a stub.
        const auto fraction = std::log1p(static_cast<float>(classes[i])) / std::log1p(static_cast<float>(peak));
        const auto bar_height = 2.0f + (height - 5.0f) * fraction;
        const auto x = origin.x + slot * static_cast<float>(i) + 1.5f;

        // The rightmost classes are the slivers nothing can be allocated from, so
        // they carry the heat regardless of overall pressure.
        const auto locality = static_cast<float>(i) / static_cast<float>(classes.size() - 1u);
        const auto tint = heat_colour(heat * 0.45f + locality * 0.55f);

        draw->AddRectFilled(
            ::ImVec2{x, origin.y + height - 1.5f - bar_height},
            ::ImVec2{x + slot - 3.0f, origin.y + height - 1.5f},
            ::ImGui::GetColorU32(tint));
    }

    draw->AddRect(
        origin,
        ::ImVec2{origin.x + width, origin.y + height},
        ::ImGui::GetColorU32(::ImVec4{0.36f, 0.08f, 0.10f, 1.0f}));
    ::ImGui::Dummy(::ImVec2{width, height + 1.0f});

    ::ImGui::TextDisabled("large");
    ::ImGui::SameLine();
    const auto slivers = diag::sliver_count(classes);
    ::ImGui::PushStyleColor(::ImGuiCol_Text, heat_colour(0.55f + heat * 0.45f));
    ::ImGui::Text("%u under %llu MB", slivers, static_cast<unsigned long long>(diag::sliver_threshold >> 20u));
    ::ImGui::PopStyleColor();
    ::ImGui::SameLine(::ImGui::GetContentRegionAvail().x - 26.0f);
    ::ImGui::TextDisabled("small");
}

auto draw_summary() -> void
{
    auto snapshot = diag::FreeSpaceSnapshot{};
    const auto have_snapshot = diag::Sampler::instance().try_snapshot(snapshot);

    auto totals = texture::TextureTotals{};
    const auto have_totals = texture_ledger().try_totals(totals);

    // The block below the 2 GB line, not the overall largest. On a
    // large-address-aware install the overall figure sits near 2 GB and never
    // moves, which is why the old graph showed nothing.
    const auto headroom = snapshot.largest_free_below_2g != 0u ? snapshot.largest_free_below_2g : snapshot.largest_free;
    const auto heat = heat_from(headroom);
    const auto colour = heat_colour(heat);

    // From the rescue coordinator, not from dao::engine_state().evictions --
    // that counter includes every eviction the game performs on its own cache,
    // which made the panel claim RECLAIMING while lyrium had done nothing.
    const auto rescue = rescue_stats();
    const auto *state = rescue.under_pressure ? "RECLAIMING" : (heat > 0.35f ? "WATCHFUL" : "DORMANT");

    ::ImGui::PushStyleColor(::ImGuiCol_Text, colour);
    ::ImGui::TextUnformatted(state);
    ::ImGui::PopStyleColor();

    if (!have_snapshot)
    {
        ::ImGui::TextDisabled("sampling...");
        return;
    }

    ::ImGui::Spacing();
    ::ImGui::TextDisabled("HEADROOM");
    ::ImGui::PushStyleColor(::ImGuiCol_Text, colour);
    ::ImGui::Text("%.0f MB", static_cast<double>(megabytes(headroom)));
    ::ImGui::PopStyleColor();
    ::ImGui::SameLine();
    ::ImGui::TextDisabled("largest contiguous block");

    // Scaled against the comfortable mark the heat curve uses, so the bar and
    // the colour always agree.
    constexpr auto full_scale = 384.0f;
    draw_meter(megabytes(headroom) / full_scale, 32.0f / full_scale, heat);

    ::ImGui::Spacing();
    ::ImGui::TextDisabled("KEPT OUT OF MEMORY");
    if (have_totals)
    {
        ::ImGui::Text("%.1f MB", static_cast<double>(megabytes(totals.relocated_bytes)));
        ::ImGui::SameLine();
        ::ImGui::TextDisabled("across %llu textures", static_cast<unsigned long long>(totals.relocated_count));
    }
    else
    {
        ::ImGui::TextDisabled("busy");
    }

    ::ImGui::Spacing();
    ::ImGui::TextDisabled("FRAGMENTATION");
    ::ImGui::SameLine();
    ::ImGui::TextDisabled("- %u free blocks", snapshot.free_regions);
    draw_fragmentation(snapshot, heat);
}

auto draw_diagnostics() -> void
{
    auto totals = texture::TextureTotals{};
    if (!texture_ledger().try_totals(totals))
    {
        ::ImGui::TextDisabled("ledger busy");
        return;
    }

    constexpr auto managed = static_cast<std::size_t>(texture::TexturePool::D3DPOOL_MANAGED);
    constexpr auto def = static_cast<std::size_t>(texture::TexturePool::D3DPOOL_DEFAULT);

    ::ImGui::Text(
        "textures: %llu live, %.1f MB (peak %.1f MB)",
        static_cast<unsigned long long>(totals.live_count),
        static_cast<double>(megabytes(totals.total)),
        static_cast<double>(megabytes(totals.peak)));
    ::ImGui::Text(
        "pools: default %.1f MB, managed %.1f MB",
        static_cast<double>(megabytes(totals.by_pool[def])),
        static_cast<double>(megabytes(totals.by_pool[managed])));

    const auto failures = stats::d3d_create_failures.load(std::memory_order_relaxed);
    ::ImGui::Text("creates: %llu", static_cast<unsigned long long>(stats::d3d_creates.load(std::memory_order_relaxed)));
    ::ImGui::SameLine();
    if (failures != 0u)
    {
        ::ImGui::PushStyleColor(::ImGuiCol_Text, crystal_core);
        ::ImGui::Text("- %llu FAILED", static_cast<unsigned long long>(failures));
        ::ImGui::PopStyleColor();
    }
    else
    {
        ::ImGui::TextDisabled("- none failed");
    }

    if (const auto reverts = stats::pool_reverts.load(std::memory_order_relaxed); reverts != 0u)
    {
        ::ImGui::PushStyleColor(::ImGuiCol_Text, crystal_hot);
        ::ImGui::Text("fell back to managed: %llu", static_cast<unsigned long long>(reverts));
        ::ImGui::PopStyleColor();
    }

    ::ImGui::Text(
        "buffers vb/ib/state: %zu / %zu / %zu",
        stats::live_vertex_buffers.live_count(),
        stats::live_index_buffers.live_count(),
        stats::live_state_blocks.live_count());

    auto snapshot = diag::FreeSpaceSnapshot{};
    if (diag::Sampler::instance().try_snapshot(snapshot))
    {
        ::ImGui::Text(
            "address space: %.0f MB free total, walk %lld us",
            static_cast<double>(megabytes(snapshot.total_free)),
            static_cast<long long>(snapshot.walk_us));
    }
}

auto draw_engine() -> void
{
    const auto engine = dao::engine_state();

    // No "texture loads" row. Its counter only ever tracked load_texture_file,
    // the loose-file entry point, which a stock install never calls -- assets
    // arrive through stream_load instead. The hook and the counter stay, since
    // they cost nothing on a path that never runs and dropping a target would
    // change the all-or-nothing install gate, but a permanent zero on screen
    // reads as a broken gauge rather than an unused path.
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
    const auto held = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 && (::GetAsyncKeyState(VK_F12) & 0x8000) != 0;
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

    ::ImGui::SetNextWindowSize(::ImVec2{330.0f, 0.0f}, ::ImGuiCond_FirstUseEver);
    ::ImGui::SetNextWindowSizeConstraints(::ImVec2{300.0f, 0.0f}, ::ImVec2{560.0f, FLT_MAX});
    ::ImGui::Begin("LYRIUM");

    draw_summary();

    // Everything below is for debugging rather than playing, so it starts shut.
    ::ImGui::Spacing();
    ::ImGui::SetNextItemOpen(false, ::ImGuiCond_Once);
    if (::ImGui::CollapsingHeader("diagnostics"))
    {
        draw_diagnostics();
    }
    ::ImGui::SetNextItemOpen(false, ::ImGuiCond_Once);
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
