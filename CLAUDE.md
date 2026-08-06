# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository.

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

## Tests

Every test is its own executable, because some of them drive process-global
counters into states that would corrupt every later assertion in the same
binary. So a single test is either

```
ctest --preset tests-linux32 -R rescue_policy
build/tests-linux32/tests/rescue_policy_test --gtest_filter='*Watermark*'
```

Targets are declared with `lyrium_add_test` in `tests/CMakeLists.txt`, which
takes two flags:

- `KNOWN_DEFECT` labels the test `known_defect`. The `tests-linux32` test preset
  excludes that label, so the green loop stays green; `ctest --preset
  known-defects` runs exactly those and they are **expected to fail** until the
  bug they pin is fixed. Nothing carries the label right now.
- `NEEDS_D3D9_SHIM` puts `tests/shim/` on the include path. That header supplies
  only the `D3DFORMAT` and `D3DPOOL` enumerators, so the texture-sizing code can
  be tested without a Windows SDK. It is handed out per-target so it can never
  shadow the genuine `<d3d9.h>` elsewhere.

The shim's values are checked against the real header by
`tests/conformance/d3d9_shim_conformance.cpp`, which only the `dll-win32`
configure compiles — nothing links it, the `static_assert`s are the product. A
value that drifts therefore breaks the DLL build, not the test run, and until you
build the DLL the Linux sizing tests will happily measure the wrong thing.

`tests/CMakeLists.txt` hard-errors on a 64-bit configure. The premise of the
suite is ABI parity with the shipping DLL; at 8-byte pointers the allocator and
address-space arithmetic would pass tests that mean nothing.

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
  `mov dword ptr [esp+0x18], 0x35200000`. That figure is a **budget for two
  allocations, not the size of one pool** — see "What the pool patch actually
  changes".
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

Shift+F12 toggles the overlay, when `overlay=1` is set.

In `va[...]`, `below2g` is the largest single contiguous block below the 2 GB
line and `low_total` is how much is free down there in total. Small `below2g`
with large `low_total` is fragmentation; both small is exhaustion.

Which of `below2g` and `largest_free` actually binds depends on the image, and
`diag::headroom_bytes` (`diag/va_region.h`) is what decides: `largest_free` when
`daorigins.exe` is large-address-aware, `below2g` when it is not. That figure,
not `below2g`, is what the rescue measures pressure against, and it is logged as
`headroom=` alongside `laa=` and `low=` on the `rescue[...]` line. On an LAA
install Windows allocates bottom-up and never walks past the line until nothing
below it fits, so `largest_free` sits at 2,146,115,584 in every sample while
`below2g` has fallen as low as 12 MB with not one texture create failing — a
small `below2g` there is not a prediction of failure. Zero from a reading means
"not walked yet" and is deliberately treated as pressure rather than as safety.

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

## What the pool patch actually changes

Read out of `DAOrigins.exe` with Ghidra (`tools/ghidra/`, output gitignored under
`analysis/`). The whole sequence lives in one function, `FUN_004b8da0` at
`0x004b8da0`, reached once through a lazy getter at `0x004b92c0`:

```
budget = 0x35200000                       <- 850 MB, the immediate at 0x004B8F30
hHeap  = GetProcessHeap()                 <- the process DEFAULT heap, not a private one
HeapLock(hHeap)
  for each of three records:
    "Strings"        0x3700000 (55 MB)    -> HeapAlloc, budget -= 55 MB
    "StreamingMain"  size 0               -> skipped by `if (dwBytes != 0)`
    "StreamingGPU"   size 0               -> skipped
  while (budget > 1 MB && HeapAlloc(hHeap, 0, budget) == null)
    budget -= 1 MB                        <- backs off until the request fits
HeapUnlock(hHeap)
  register the result as "Main Pool"
```

Three things follow, none of which the earlier behavioural model had:

- **It is two blocks, not one**, and `main_pool_mb` sets the sum. Stock 850 MB
  yields 55 MB of `Strings` plus a 795 MB `Main Pool`. Setting `main_pool_mb=768`
  asks for a **713 MB** Main Pool, not 768.
- **It is `HeapAlloc` on the process default heap.** Both requests are far over the
  NT heap's 508 KB threshold, so each still becomes its own reservation — but the
  engine never calls `HeapCreate` or `VirtualAlloc` itself. Every other Win32
  allocation site in the image belongs to the statically linked MSVC CRT
  (`__heap_init`, `___sbh_alloc_new_region`, `__calloc_impl` and friends).
- **The Main Pool request already degrades gracefully.** If the address space
  cannot satisfy it the engine retries 1 MB smaller, down to a 1 MB floor, and
  carries on with whatever it got. So the engine's own pool may already be smaller
  than the patched value implies, silently, and asking for too much is partly
  self-correcting. `Strings` has no such loop.

`FUN_004b93f0` at `0x004b93f0` is the registrar every pool goes through,
`__thiscall`, taking `(id, name, base, size, flags, 0)`. It `wcsncpy`s the name to
`this+0x54` (63 wchars), stores `base` at `this+0xD8` and **the size actually
obtained at `this+0xE0`**, then dispatches to vtable slot 1. That is the one place
where the real Main Pool size is visible, and it is the obvious hook if we want
"how big did the pool actually end up" in the log.

### What sub-allocates from it

The manager is `malloc(0x4d4)` at `DAT_00c2b584`, holding four sub-allocators
embedded at `+0x20`, `+0x140`, `+0x270`, `+0x398` and pointed to by an array at
`+0x4c0..+0x4cc`. `FUN_004b9100(manager, id)` walks that array comparing `+0xD4`,
the pool id. **Main Pool is id 0 and is the first entry**, `manager+0x20`, the base
class with vtable `0x00AEF424` — established from the `mov ecx,[edx+0x4c0]` that
sets up its registration call at `0x004b9039`.

| what | where |
|---|---|
| allocator entry point | `FUN_004b92c0` at `0x004b92c0` |
| pool lookup by id | `FUN_004b9100` at `0x004b9100` |
| Main Pool `allocate` (vtable slot 4) | `FUN_004ba880` at `0x004ba880` |
| Main Pool `free` (vtable slot 6) | `FUN_004b9d90` at `0x004b9d90` |
| attach memory (vtable slot 1) | `FUN_004ba1d0` at `0x004ba1d0` |
| next / previous block | `FUN_004b9a70` / `FUN_004b9aa0` |

`FUN_004ba1d0` aligns the block up to 64 KB and stores the aligned start at
`this+0xDC` and the usable byte count at `this+0xE4` — so the capacity actually in
play is a little under what `HeapAlloc` returned. There is **no used or high-water
counter on the object**: the bookkeeping is a free list built in place inside the
block, with a size at header `+0`, the alignment shift at `+6`, an in-use byte at
`+7`, and the payload `0x10` past the header.

That gives two ways to make pool pressure visible, which is the thing the log has
never had:

- **Cheap, on the hot path.** `FUN_004b92c0` returns 0 when an allocation fails,
  and it only does so after already retrying against Main Pool. A counter there is
  the first direct signal that `main_pool_mb` is set too low — today that condition
  reaches no counter at all, which is why the failure is invisible.
- **Rich, off the hot path.** The block chain is walkable from `this+0xDC` with the
  header layout above, giving used, free and largest-free inside the pool. It needs
  the pool's own lock at `this+0x100`, so it belongs on the sampler thread with the
  address-space walk, never on the create path.

Neither is implemented. The addresses above are read from the binary but are not
in `dao/targets.h` and carry no recorded prologue or hash yet, so nothing may hook
them until they go through the same verification every other target does.

## main_pool_mb

Default 850 MB, and the engine does not need it. Note from the section above that
this is the budget for `Strings` plus `Main Pool`, so the pool itself is 55 MB
smaller than the number set here. Measured on a 2 GB install,
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
