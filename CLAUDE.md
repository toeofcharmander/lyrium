# CLAUDE.md

A proxy `d3d9.dll` (project name "lyrium") for Dragon Age: Origins, dropped next
to `daorigins.exe`, which loads it instead of the system DLL.

Forked from `adarec1994/eluvian`, which built on `nathan-baggs/mandrel`. Texture
relocation and the startup pool patch are both inherited.

## Build

Two configurations driven by `CMakePresets.json`, both 32-bit so
`sizeof(void*) == 4` on either side.

```
cmake --preset dll-win32     && cmake --build --preset dll-win32
cmake --preset tests-linux32 && cmake --build --preset tests-linux32 && ctest --preset tests-linux32
```

The DLL needs i686 MinGW-w64 GCC 14 or newer on PATH (or `LYRIUM_MINGW32_ROOT`
set). `utils.h` uses `<print>` and `cxx_std_26` is required, so Ubuntu's
`g++-mingw-w64-i686-win32` (GCC 13.2) cannot build it; winlibs is the toolchain
source. The tests need `g++-multilib`. ImGui and googletest are fetched at
configure time.

Output is `build/dll-win32/src/d3d9.dll`. It must keep that filename and import
only system DLLs — check with `objdump -p ... | grep 'DLL Name'`. Any
`libstdc++-6.dll`, `libgcc_s_dw2-1.dll` or `libwinpthread-1.dll` means static
linking broke and it will not run next to the game.

`-m32` must come from the toolchain file, not `CMAKE_CXX_FLAGS`. Set after
`project()` it leaves `CMAKE_SIZEOF_VOID_P` at 8 and every `try_compile` answers
for the wrong ABI.

Formatting: `.clang-format` at the root (Allman, 4-space, 120 columns).

## Architecture

Almost all logic lives in headers under `include/lyrium/` (an INTERFACE library
compiled with `-Werror` and `cxx_std_26`). Four translation units build into the
DLL: `src/d3d9.cpp` (proxy entry point and D3D hooks), `src/overlay.cpp`,
`src/engine_hooks.cpp`, `src/resettable_texture.cpp`.

`DllMain` at `DLL_PROCESS_ATTACH` verifies every engine target read-only and,
only if all match, patches the pool size immediate (`dao/pool_patch.h`). The
exported `Direct3DCreate9` then loads the real system `d3d9.dll`, forwards the
call, and hooks COM interfaces by vtable slot (`hooks/com_hook.h`).

- **Texture relocation** (`resettable_texture.h/.cpp`) — creates eligible
  textures in `D3DPOOL_DEFAULT`. DEFAULT textures cannot be locked, so `LockRect`
  maps a view of a pagefile-backed section and hands the engine a pointer into it;
  `UnlockRect` marks the level owed and the upload is batched at bind time. The
  wrapper is also what lets a DEFAULT texture survive device `Reset`.
  `texture/staging_pool.h` reuses the staging textures by shape.
- **Engine hooks** (`dao/engine_hooks.h`, `dao/inline_hook.h`, `dao/targets.h`) —
  inline hooks at fixed addresses. `dao/targets.h` is the single source of truth:
  each target carries its address, size, SHA-256 and prologue bytes.
- **Pool patch** (`dao/pool_patch.h`) — rewrites the immediate at `0x004B8F30`,
  `mov dword ptr [esp+0x18], 0x35200000`, the size of the pool the engine
  sub-allocates all its own data from.
- **Rescue** (`policy/rescue_policy.h`, `policy/rescue_coordinator.h`) — bounded,
  rate-limited eviction from the engine's texture cache when the largest free
  block can no longer hold the request.
- **Diagnostics** (`diag/`, `stats.h`, `log.h`, `overlay.*`) — VA accounting, a
  sampler thread, breadcrumb and file logging, ImGui overlay. `allocation_watch`
  additionally hooks `NtAllocateVirtualMemory` and counts d3d9's `LocalAlloc` and
  `malloc` imports; off by default.

Policy is separated from mechanism: `policy/` and most of `diag/` name no D3D or
Windows type, so they compile and are tested on Linux. Anything including
`windows.h` or `psapi.h` is unreachable by the test suite.

The DLL must not consume the game's address space or CRT heap.
`allocators/global_allocator.h` creates a private heap and everything persistent
goes through it — use the `lyrium::Vector`/`Map`/`UnorderedMap`/`String` aliases
from `containers/`; ImGui is routed through `allocators/imgui_allocator.h`.

## Running it

Launch `bin_ship\DAOrigins.exe` directly. The launcher chain runs
`DAOriginsConfig.exe`, which crashes with `0xc0000094` on modern hardware and
stops before the game starts; it imports no `d3d9.dll`, so that crash is never
ours. If `lyrium_logs/` was not created, our code never ran.

A healthy session ends with `va[shutdown]`, `textures[shutdown]`,
`rescue[shutdown]` and `lyrium: log sealed`. `lyrium_breadcrumbs.txt` should end
with `detach: sealed`; if it ends earlier, the last breadcrumb names the
statement that hung.

In `va[...]`, `below2g` is the largest single contiguous block below the 2 GB
line — the number that predicts a failed allocation. `low_total` is how much is
free down there in total. Small `below2g` with large `low_total` is
fragmentation; both small is exhaustion.

Four of the ten engine hooks never fire on a stock install
(`decode_texture_memory`, `create_texture_from_memory`,
`create_volume_from_memory`, and `load_texture_file` almost never), because
assets arrive through streaming. Each detour writes a one-time breadcrumb on
first entry, so the breadcrumb file distinguishes a dead path from a broken
counter.

**Config traps**, pinned by `config_parse_test.cpp`: every key is parsed
section-blind by `load_config()` except `main_pool_mb`, which `d3d9.cpp` reads
with `GetPrivateProfileIntA` and which therefore needs a literal `[lyrium]`
header. A comment marker is stripped from anywhere in a line including inside a
value. A malformed number becomes zero rather than falling back to the default.
`Config::overlay` is declared `true` while the parser passes a fallback of
`false`.

## main_pool_mb

Default 850 MB, and the engine does not need it. Measured on a 2 GB install,
relocation on, sessions of 4 to 10 minutes. Steady state is noisy because the
sessions are not controlled; rescue activity is the clean signal.

| pool | steady `below2g` | worst sample | rescue armed |
|---|---|---|---|
| 850 (stock) | 2.4 MB | 2.4 MB | firing, exhaustion |
| 800 | 46.2 MB | 14.2 MB | 14 |
| **768** | **110.2 MB** | 26.0 MB | 2 |
| 704 | 78.2 MB | 78.2 MB | 0 |
| 640 | 87 MB | 87 MB | 0 |
| 512 | — | — | starves |

The relationship is a threshold, not a slope: 850 -> 768 returns 82 MB of pool
and gains 108 MB of largest free block. Do not interpolate the table. The values
are mod-load dependent — this is one heavily modded install.

**Too small is not detectable from the log.** Too large starves the address space
and is loud:
`E_OUTOFMEMORY`, `failures=` climbing, the rescue arming. Too small starves the
engine inside its own pool and shows up neither way. At 512 MB one session ran
`creates=4622 failures=0` with the world visibly missing geometry, and another
hung during a level load with `creates=744` frozen and 512 MB of contiguous
headroom. Neither reaches `create_texture_2d`, because the engine allocates pool
memory for decoded asset data before it ever calls D3D. Both are visible to a
player and neither is visible to us. Bias upward.

## The alt-tab crash

`0x0046b1b3` / `0x40000015` in `DAOrigins.exe`, and not a lyrium bug. The image
base is `0x400000` with no ASLR, so WER's offset is the return address of an
indirect call through vtable slot 2 inside `D3DGraphicsDriver::ResetDevice` at
`0x0086b030`. `D3DResetable`'s abstract vtable at `0x00b2e32c` holds `_purecall`
in that slot. The engine registers each object into the driver's reset-broadcast
registry from the `D3DResetable` base constructor and removes it from the base
destructor, so anything mid-construction is in the list with the abstract vtable
installed when the broadcast calls it.

A MinGW-built DLL cannot raise R6025 — that is MSVC's pure-virtual trap — so the
dialog alone proves the object is the game's. Borderless windowed avoids it
entirely: no device loss on alt-tab means no reset broadcast.

Anything running inside the `Reset` hook holds the engine's driver mutex during
the broadcast. Two ~13 ms address-space walks used to run there; removing them
took the crash from easy to reproduce out to 57 resets and removed a cutscene
stutter. Moving work the other way — recreating relocated textures lazily at bind
time — was tried and hung the machine hard enough to need a sign-out.

## Constraints

- Everything is 32-bit; pointers and `size_t` are 4 bytes.
- The addresses and hashes in `dao/targets.h` and `dao/pool_patch.h` are specific
  to one `daorigins.exe` build. Never "fix" a hash or address to make a hook
  fire; the byte verification is the safety mechanism.
- Hooked code runs inside the render loop and inside `DllMain` — no blocking I/O,
  no game-heap allocation. The address-space walk (median 15 ms, max 205 ms) must
  never run on the create path.
- Objects with static storage duration are never destroyed
  (`never_destroyed.h`). Static destructors run after `DllMain` under the loader
  lock with every other thread already terminated by `ExitProcess`, so a mutex a
  dead thread was holding is held forever. The log is sealed explicitly at
  detach, on the exiting thread, taking no locks.
