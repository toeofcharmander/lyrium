# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A proxy `d3d9.dll` (project name "lyrium") for Dragon Age: Origins on PC. DAO is a
32-bit game with a 2 GB address space; long sessions fragment that space until no
contiguous block is left for a texture allocation, DirectX returns out-of-memory,
textures flicker, and the game crashes. This DLL sits between the game and the real
`d3d9.dll` and attacks the fragmentation from several angles (see README.md for the
full analysis). It is dropped next to `daorigins.exe`, which loads it instead of the
system DLL.

## Build

Two configurations, two toolchains, driven by `CMakePresets.json`. Both produce
32-bit binaries so `sizeof(void*) == 4` is identical on both sides.

**Shipping DLL — 32-bit Windows.** Needs i686 MinGW-w64 GCC 14 or newer on PATH
(or `LYRIUM_MINGW32_ROOT` set). `utils.h` uses `<print>` and `src/CMakeLists.txt`
requests `cxx_std_26`, both of which need GCC 14+, so Ubuntu's
`g++-mingw-w64-i686-win32` (GCC 13.2) cannot build this and a plain apt
cross-compile is not an option. winlibs is the toolchain source; MSYS2's MINGW32
environment is deprecated.

```
cmake --preset dll-win32 && cmake --build --preset dll-win32
```

Output is `build/dll-win32/src/d3d9.dll`. It must keep exactly that filename, and
must import only system DLLs — check with
`objdump -p ... | grep 'DLL Name'`. Any `libstdc++-6.dll`, `libgcc_s_dw2-1.dll`
or `libwinpthread-1.dll` means static linking broke and it will not run next to
the game.

**Tests — 32-bit Linux.** Needs `g++-multilib`; needs no MinGW and does not fetch
ImGui.

```
cmake --preset tests-linux32 && cmake --build --preset tests-linux32
ctest --preset tests-linux32          # must be green
ctest --preset known-defects          # pinned bugs, expected to FAIL until fixed
```

Do not put `-m32` in `CMAKE_CXX_FLAGS`. It has to come from the toolchain file so
it applies before CMake probes the compiler; set after `project()` it leaves
`CMAKE_SIZEOF_VOID_P` at 8 and every `try_compile` answers for the wrong ABI.

ImGui and googletest are fetched at configure time, so a first configure needs
network access.

A test marked `KNOWN_DEFECT` pins a real bug and is expected to fail. Do not
"fix" it by weakening the assertion — fix the code, and the test turns green.

Formatting: `.clang-format` at the root (Allman braces, 4-space indent, 120
columns). Run `clang-format` on anything you touch.

## Running it in the game

Unit tests cover the pure logic, but most of this code only does anything once
loaded into the game, so treat "it compiles and the suite is green" as a weak
signal. A change is confirmed only after it has run in the game.

Copy the built DLL to the game's `bin_ship` directory and **launch
`DAOrigins.exe` directly**:

```
cp build/dll-win32/src/d3d9.dll "<game>/bin_ship/d3d9.dll"   # close the game first
```

**Do not launch through `DAOriginsLauncher.exe` or the desktop shortcut.** The
chain is `DAOriginsLauncher.exe` -> `DAOriginsConfig.exe` -> `DAOrigins.exe`, and
the config utility crashes with `0xc0000094` (integer divide by zero) inside
itself on modern hardware. It imports no `d3d9.dll` at all, so that crash is never
caused by this project -- but it stops the chain before the game ever starts.
Only `DAOrigins.exe` loads the proxy.

Diagnosing a failed launch: if `bin_ship/lyrium_logs/` was not created, our code
never ran, which points at the launch path rather than at a change. The Windows
Application event log names the faulting module and is the fastest way to tell
those apart. Note that a DLL failing to load produces a **GUI dialog**, which is
invisible from WSL and shows up only as an odd exit code with empty stderr -- if
you are driving the build from WSL, ask what appeared on screen.

The `lyrium.ini` next to the executable controls everything (see `config.h`).
Useful for a smoke test:

```
logging=1
overlay=1
```

**Config traps**, all pinned by tests in `config_parse_test.cpp`: every key is
parsed section-blind by `load_config()` except `main_pool_mb`, which `d3d9.cpp`
reads with `GetPrivateProfileIntA` and which therefore needs a literal
`[lyrium]` section header. A comment marker is stripped from anywhere in a line
including inside a value. A malformed number becomes zero rather than falling
back to the default, so `recycler_budget_mb=abc` disables the recycler. And
`Config::overlay` is declared `true` while the parser passes a fallback of
`false`, so an absent key gives the opposite of the declared default.

A healthy session ends with `va[shutdown]`, `textures[shutdown]`,
`rescue[shutdown]` and then `lyrium: log sealed`. **If those are missing the game
did not exit normally**, which is the one thing a log could not previously tell
you. `lyrium_breadcrumbs.txt` should end with `detach: sealed`; if it ends
earlier, the last breadcrumb names the statement that hung.

Verified baseline on the GOG Ultimate Edition: overlay renders, all ten engine
hooks report `installed` at their preferred addresses with `base_delta == 0`, and
the executable is large-address-aware so it has ~4 GB rather than the 2 GB the
README assumes. Because there is no relocation, the SHA-256 body check actually
executes, so `dao/targets.h` is verified against that binary rather than assumed
to match it. Note a full address-space walk costs 6-20 ms and grows with
fragmentation, so it must never run on the create path.

## Architecture

Almost all logic lives in headers under `include/lyrium/` (an INTERFACE library
compiled with `-Werror` and `cxx_std_26`). Only four translation units build into
the DLL: `src/d3d9.cpp` (proxy entry point and D3D hooks), `src/overlay.cpp`
(ImGui overlay), `src/engine_hooks.cpp` (engine-side hooks), and
`src/resettable_texture.cpp`.

Load path: `DllMain` at `DLL_PROCESS_ATTACH` verifies every engine target
read-only and, only if all of them match, patches the size immediate of the
engine's main memory pool reservation (`dao/pool_patch.h`). The exported
`Direct3DCreate9` then loads the real system `d3d9.dll`, forwards the call, and
hooks COM interfaces by vtable slot (`hooks/com_hook.h`): first
`IDirect3D9::CreateDevice`, then device methods.

**Policy is separated from mechanism.** The decisions live in portable headers
that name no D3D or Windows type, so they compile and are tested on Linux:
`policy/texture_placement_policy.h` decides which pool a texture uses,
`policy/rescue_policy.h` decides when and how hard to evict, and
`policy/rescue_coordinator.h` executes plans through abstract seams. The D3D
hooks convert at the boundary and contain no decisions of their own.

**Exit is deliberately destructor-free.** Objects with static storage duration
are never destroyed (`never_destroyed.h`); objects with automatic or dynamic
duration keep their destructors unchanged. This is not laziness: static
destructors run after `DllMain` under the loader lock with every other thread
already terminated by `ExitProcess`, so a mutex a dead thread was holding is
held forever and anything that waits on it deadlocks inside the loader. The log
is sealed explicitly at detach instead, on the exiting thread, taking no locks.

The main mechanisms, each mostly independent:

- **Pool patch** (`dao/pool_patch.h`) -- shrinks the engine's ~795 MB startup pool
  by rewriting one instruction immediate at process attach.
- **Engine hooks** (`dao/engine_hooks.h`, `dao/inline_hook.h`, `dao/targets.h`) --
  inline hooks into engine functions (texture load/create paths, texture cache
  evict/clear, optionally CRT malloc/free) at fixed addresses. `dao/targets.h` is
  the single source of truth: each target carries its address, size, SHA-256, and
  prologue bytes, and hooks verify those bytes before patching, so they no-op on an
  unknown game build. Supports "rescue" emergency cache eviction when free VA drops
  below a watermark.
- **Texture stager** (`texture_stager.h`) -- the core fix from the README: steers
  texture creation to `D3DPOOL_DEFAULT` (no managed duplicate in the 2 GB space).
  DEFAULT textures can't be locked, so Lock hands the engine a temporary buffer,
  and Unlock copies it to the GPU and discards it; the engine can't tell the
  difference.
- **Resettable textures** (`resettable_texture.h/.cpp`) -- wraps DEFAULT-pool
  textures so they survive device `Reset` (resolution/graphics changes), which
  plain DEFAULT resources do not.
- **Texture recycler** (`texture_recycler.h`) -- optional reuse of released
  textures keyed by shape, with a byte budget.
- **Diagnostics** (`diag/`, `stats.h`, `log.h`, `overlay.*`) -- VA-space and
  texture accounting, a background sampler thread, breadcrumb/file logging, and an
  ImGui overlay.

**Memory discipline is a hard rule**: the DLL must not consume the game's address
space or CRT heap. `allocators/global_allocator.h` creates a private heap
(`HeapCreate`) and everything persistent goes through it -- use the
`lyrium::Vector`/`Map`/`UnorderedMap`/`String` aliases from `containers/` (std
containers bound to `STDAllocator`) rather than plain std containers; ImGui is
routed through `allocators/imgui_allocator.h` the same way.

Runtime configuration is read from `lyrium.ini` next to the game executable
(`config.h` has every key and its default); logs go to `lyrium_logs/` when
enabled.

## Constraints to keep in mind

- Everything is 32-bit: pointers and `size_t` are 4 bytes; address-space math
  assumes the 2 GB user split.
- The addresses and hashes in `dao/targets.h` and `dao/pool_patch.h` are specific
  to one `daorigins.exe` build. Never "fix" a hash or address to make a hook fire;
  the byte verification is the safety mechanism.
- Hooked code runs inside the game's render loop and inside `DllMain` -- be
  conservative about what runs there (no blocking I/O, no game-heap allocation).
