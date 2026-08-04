#pragma once

#include <windows.h>

#include <d3d9.h>

namespace lyrium::overlay
{

auto set_visible(bool value) -> void;
auto visible() -> bool;

auto poll_hotkey() -> void;

auto render(::IDirect3DDevice9 *device) -> void;
auto before_device_reset() -> void;
auto after_device_reset() -> void;

auto window_proc(::HWND window, ::UINT message, ::WPARAM wparam, ::LPARAM lparam) -> ::LRESULT;

}
