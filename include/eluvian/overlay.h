#pragma once

#include <windows.h>

#include <d3d9.h>

namespace eluvian::overlay
{

auto set_visible(bool value) -> void;
auto visible() -> bool;

auto poll_hotkey() -> void;

auto render(::IDirect3DDevice9 *device) -> void;

auto window_proc(::HWND window, ::UINT message, ::WPARAM wparam, ::LPARAM lparam) -> ::LRESULT;

}
