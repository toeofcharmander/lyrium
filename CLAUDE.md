# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A proxy `d3d9.dll` (project name "lyrium") for Dragon Age: Origins on PC. It is
dropped next to `daorigins.exe`, which loads it instead of the system DLL.

DAO is a 32-bit game, so it has 2 GB of address space -- 4 GB on installs with
the LAA patch, which most people apply. Long sessions fragment that space until
no contiguous block is left for a texture allocation, DirectX returns
out-of-memory, textures flicker, and the game crashes. The failure is
fragmentation rather than exhaustion: a captured case had 20 MB free split
across roughly 430 gaps.

**Lineage.** Forked from `adarec1994/eluvian`, which built on Nathan Baggs'
original research (`nathan-baggs/mandrel`). The central technique -- relocating
managed textures to the DEFAULT pool and backing their CPU-side copy with a
pagefile-backed file mapping only mapped during a lock -- is inherited and
deliberately unchanged, because it is the right design. What has been rewritten
is everything around it: the policy layer, the eviction behaviour, the install
gate, and a test suite where there was none. `docs/design-vs-eluvian.md` diagrams
the differences; see also README.md.

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
```

There is also a `known-defects` preset, currently empty. It exists so a newly
found bug can be pinned by a failing test without turning the main suite red.

Do not put `-m32` in `CMAKE_CXX_FLAGS`. It has to come from the toolchain file so
it applies before CMake probes the compiler; set after `project()` it leaves
`CMAKE_SIZEOF_VOID_P` at 8 and every `try_compile` answers for the wrong ABI.

ImGui and googletest are fetched at configure time, so a first configure needs
network access.

A test marked `KNOWN_DEFECT` pins a real bug and is expected to fail. Do not
"fix" it by weakening the assertion — fix the code, and the test turns green.
Every defect pinned this way so far has been fixed, so the label is currently
unused.

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

The rescue path is verified in the wild, not just under test: it has been
exercised on both a 2 GB (unpatched) and a 4 GB (LAA-patched) install, each
loaded heavily with mods, which is what actually generates the pressure needed to
reach it. A normal session on a lightly modded install will never trigger it,
because the placement policy keeps the address space healthy enough that it is
not needed -- that is the design working, not a gap in coverage.

Reading a `va[...]` line: `below2g` is the **largest single contiguous block**
below the 2 GB line, which is what the overlay's headroom bar shows and what
predicts a failed allocation. `low_total` is how much is free down there in
total, however scattered. The two together separate the failure modes -- a small
`below2g` with a large `low_total` is fragmentation, the bytes present in pieces;
both small means the space is genuinely used up. At the worst point measured,
`below2g` was 8.8 MB against a `low_total` of 84.7 MB across roughly 360 free
regions, averaging 241 KB each.

A healthy session ends with `va[shutdown]`, `textures[shutdown]`,
`rescue[shutdown]` and then `lyrium: log sealed`. **If those are missing the game
did not exit normally**, which is the one thing a log could not previously tell
you. `lyrium_breadcrumbs.txt` should end with `detach: sealed`; if it ends
earlier, the last breadcrumb names the statement that hung.

**Installed is not the same as called.** Only six of the ten engine hooks are
ever entered on a stock install: `create_texture_2d`, `create_texture_cached`,
`create_texture_registered`, `stream_load`, `texture_cache_evict` and
`texture_cache_clear`. `load_texture_file`, `decode_texture_memory`,
`create_texture_from_memory` and `create_volume_from_memory` never fire, because
assets arrive through the streaming path rather than as loose files. Each detour
writes a one-time breadcrumb on first entry, so `lyrium_breadcrumbs.txt` is how
you tell a dead path from a broken counter -- that is what established the
permanent `texture loads: 0`, whose overlay row was dropped for reading as a
broken gauge. Do not "fix" a zero counter without checking the breadcrumbs first.

Verified baseline on the GOG Ultimate Edition: overlay renders, all ten engine
hooks report `installed` at their preferred addresses with `base_delta == 0`, and
that install is large-address-aware, so calibrate thresholds against ~4 GB there
and against 2 GB on an unpatched one. Because there is no relocation, the SHA-256 body check actually
executes, so `dao/targets.h` is verified against that binary rather than assumed
to match it.

**The address space reaches a steady state and holds it.** This is the result the
project exists to produce, measured over a 2.5 hour session, 1842 samples on the
LAA install:

| | at startup | after load-in | 2.5 hours later |
|---|---|---|---|
| largest block below 2 GB | 577 MB | 143 MB | **143 MB** |
| free regions | 160 | 341 | **341** |
| total free | 3030 MB | 2361 MB | 2362 MB |
| committed private | 856 MB | 1052 MB | 1052 MB |

Every figure settles within roughly twelve minutes and then does not move. 2216
texture creates, none failed; every `rescue[...]` line zero throughout. **Nothing
leaks** -- total free is flat, so a falling `total_free` in some future log means
a genuinely new bug, not this one. What degrades is contiguity, and here it stops
degrading, which is the fix working rather than an absence of load.

**Where the edge is, found by running the log longer.** The table above was
written from the first 1842 samples. Continuing the same session to 4070 samples,
and then deliberately hammering it with roughly ten alt-tabs and ten resolution
changes, moved a number that had not moved in six hours. That is the method
working: the plateau is real for play, and the boundary only appears under an
attack, so both belong in the record.

Under repeated device resets the largest block below 2 GB steps down in units of
**exactly 21,168,128 bytes (0x1430000, 20.1875 MiB)**, five steps running, 143.48
MB down to 22.31 MB. The step tracks new peak DEFAULT-pool bytes rather than the
resets themselves -- peak climbed 733 -> 828 -> 921 -> 1038 MB at the same points
-- and it **recovers** a full step when the working set falls, so it is a
reservation tracking demand rather than a leak. Zero create failures throughout,
3224 creates. Exactly one sample of 4070 sat below the 32 MB rescue floor, five
seconds of exposure.

Do not read that trough as a rescue defect without checking the `headroom=`,
`last=` and `acted=` fields on the `rescue[...]` line. The policy is correct at
those values, pinned by `RescueCoordinator.RecordsTheHeadroomItActuallySaw` and
the trough case in the policy tests.

**The failure ladder gives up, on purpose.** A 2 GB session recorded
`on_failure=182 evictions=122 managed=121 clears=0 released=0` across sixty
failed creates, with `largest_free` pinned at 106,496 bytes and
`shape=exhausted` throughout: the ladder reached its terminal rung sixty times
and moved nothing. That rung clears the engine's entire texture cache and calls
`EvictManagedResources`, and every `~D3DResetable` installs the abstract vtable
before unregistering, so repeating it feeds the game's own reset race for no
measured return. `RescueConfig::failure_ladder_limit` (default 3) bounds it per
*pressure episode* -- relieving pressure re-arms it, so one bad minute cannot
disarm the rest of a save, and `failure_ladder_limit=0` restores the old
behaviour without a rebuild. The `rescue[...]` line carries `ladders=` and
`abandoned=` so a session that stops escalating is distinguishable from one that
stopped failing.

**Which free-block figure the rescue is measured against depends on the install**
(`diag::headroom_bytes`, `include/lyrium/diag/va_region.h`). The probe used to
take the smaller of `largest_free` and `largest_free_below_2g` on every install.
On a large-address-aware process that is the wrong constraint: Windows allocates
bottom-up, serving each reservation from the lowest hole that fits, and serves
from above the 2 GB line by itself when none does. Measured directly --
`largest_free` held at exactly 2,146,115,584 while the low block fell from 579 MB
to 19.6 MB, then stepped down in 21,168,128-byte units as the OS began serving
from above the line. So on LAA the probe reports the whole space; without LAA
there is nothing above the line and it reports the low block, unchanged.

Either reading of zero means *unknown* and falls back to the other; both zero
returns zero, which `is_pressured` treats as pressure rather than as safety.

What that changed, measured on the same install and the same test (Denerim, a
cutscene, then repeated alt-tabbing):

| | before | after |
|---|---|---|
| preemptive rescues | 1 to 51 per session | **0** |
| rescues the old rule would have armed | -- | **697 and 836** (`avoided=`) |
| failed texture creates | 0 | **0** |
| device resets before the crash | 57 | **81, no crash** |

The `avoided=` counter exists to keep this checkable rather than assumed. On a
2 GB install the two figures are identical, so it stays zero there and the same
counter validates both branches.

The crash improvement is a hypothesis, not an established consequence: a rescue
that does not fire does not destroy engine textures, so fewer objects are
mid-construction during the reset broadcast described below. Plausible, one data
point.

**The alt-tab crash is a defect in `DAOrigins.exe`, established by disassembly.**
WER reports `0x0046b1b3` as an offset; the image base is `0x400000` with no ASLR,
so the faulting address is `0x0086b1b3` -- the return address of an indirect call
through vtable slot 2, inside `D3DGraphicsDriver::ResetDevice` at `0x0086b030`.
`D3DResetable`'s abstract vtable at `0x00b2e32c` holds `_purecall` in that slot.
The engine registers each object into the driver's reset-broadcast registry from
the `D3DResetable` base constructor and removes it from the base destructor, so
anything mid-construction or mid-destruction is in the list with the abstract
vtable still installed when the broadcast calls it. Its `GMutex` is recursive and
does not prevent re-entry on the same thread, and it discards `Reset`'s HRESULT.

Two landings, one race: **R6025 with the log still sealing** (the CRT's `exit()`
runs detach) and **silent death with no detach at all** (the loop substitutes an
uninitialised pointer at `0x00c3a0dc` when the list shrinks underneath it). A
MinGW-built DLL cannot raise R6025 -- that is MSVC's pure-virtual trap, and this
game is statically linked MSVC 8 -- so the dialog alone proves the object is the
game's.

**lyrium cannot fix it, only stop widening the window.** Removing two ~13 ms
address-space walks from inside the `Reset` hook took the crash from easy to
reproduce out to 57 resets, and removed a cutscene stutter at the same time.
Anything running inside that hook holds the engine's driver mutex during the
broadcast. Do not chase this as a lyrium bug, and do not move work out of `Reset`
into the draw path -- recreating relocated textures lazily at bind time was tried
and hung the machine hard enough to need a sign-out.

Walk cost across those samples: **median 15 ms, p90 29 ms, max 205 ms.** It tracks
region count, so it climbs during load-in and then plateaus with everything else;
the outliers are the sampler thread being descheduled, not work. It must never run
on the create path. It is safe on the sampler thread because `sample_va()`
completes *before* `snapshot_mutex_` is taken, so even a 205 ms walk cannot block
the render thread's `try_snapshot`.

**It ran on the create path anyway, and the rule was written while it did.** A
failed texture create called `Sampler::sample_now("create_failed")` inline. One
2 GB session logged 240 of those, each carrying `walk_us` of roughly 7 ms --
about 1.7 seconds of walking on the render thread, during the failure cascade it
was describing. A failure now calls `Sampler::request_sample()`, which sets one
atomic; the sampler thread polls it every 100 ms (`diag/sample_schedule.h`,
tested on Linux because `sampler.h` reaches `psapi.h` and cannot be) and a whole
burst of failures collapses into one walk.

Two consequences when reading a log. `va[create_failed]` now describes the
moments just *after* the failure rather than the instant of it, and there is one
of them per 100 ms rather than one per failure. The `rescue[...]` line beside it
is unchanged, because the rescue has always decided from the sampler's cached
figures rather than from a fresh walk.

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

**Never patch a prologue you have not measured.** `hooks/prologue.h` existed
because a five-byte inline hook was applied to `ntdll!RtlReAllocateHeap`, whose
32-bit prologue is `6A 0C` (`push 0Ch`, two bytes) followed by `68 imm32` (five
bytes). Five lands three bytes inside the second instruction, so the trampoline
ended in a `push` whose immediate was assembled out of jump bytes and the game
died executing an address it could not read. `RtlFreeHeap` and `RtlSizeHeap`
begin `8B FF 55 8B EC`, the hotpatch prologue, which is exactly five bytes of
three whole instructions -- which is why two of the three hooks worked and made
the third look like something else. Its `patch_length()` returned 0 for
anything it did not recognise, including relative calls and jumps whose
displacements cannot survive being copied, and 0 meant decline the hook rather
than guess.

The header went with the heap interposer that was its only caller -- the engine
hooks carry recorded prologue bytes per target in `dao/targets.h` and verify
those instead, so their lengths are correct by construction. **Any new inline
hook into code lyrium does not own needs it back** (`git log -- include/lyrium/hooks/prologue.h`).
The rule survives the file: five bytes is a guess, and this one cost a crash.

**Arithmetic does not live in a Windows-only header.** Anything that includes
`windows.h` or `psapi.h` cannot be reached by the test suite, so any calculation
placed there is untestable by construction. Put the numbers in a portable header
and let the Windows code call it. This is not a style preference -- it was
learned from a shipped bug: the free-block size thresholds existed twice, once
descending in the portable `diag/va_region.h` and once ascending inside
`diag/va_space.h`, which needs `psapi.h`. A test asserted the ordering of the
portable copy and passed, while production used the other one, and the overlay
drew a healthy address space as a shattered one. The fix was not to reorder an
array but to move both the accumulation and the differencing into
`diag/free_size_classes.h`, where both sides read the same thresholds through the
same functions and the whole thing is under test. **A green test proves nothing
about an array production does not use** -- when duplicating a constant is
tempting, that is the smell.

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
- **Texture relocation** (`resettable_texture.h/.cpp`) -- the core fix from the
  README: steers texture creation to `D3DPOOL_DEFAULT` (no managed duplicate in
  the 2 GB space). DEFAULT textures cannot be locked, so `LockRect` maps a view
  of a pagefile-backed section and hands the engine a pointer into it; `UnlockRect`
  records the level as owed and the upload is batched at bind time. The section
  is inherited from eluvian. What is not inherited is `texture_stager.h`, which
  eluvian wired into six places in its create path to serve locks from a
  transient `D3DPOOL_SYSTEMMEM` staging texture; lyrium has no such file and
  routes every lock through the mapped section instead. The same wrapper is what
  lets a DEFAULT texture survive device `Reset`, which a plain DEFAULT resource
  does not.
- **Heap arena -- removed, and do not rebuild it.** `allocators/arena.h` and
  `allocators/heap_interposer.h` served `d3d9.dll`'s large heap allocations from
  one contiguous reservation, on the theory that the runtime's MANAGED texture
  duplicates were what cut the low 2 GB into separate holes. They are not, and
  the import table says why. See "Why interposing on d3d9's allocator cannot
  work" below. Recoverable from history if the premise ever changes.

- **Texture recycler** (`texture_recycler.h`) -- optional reuse of released
  textures keyed by shape, with a byte budget.
- **Diagnostics** (`diag/`, `stats.h`, `log.h`, `overlay.*`) -- VA-space and
  texture accounting, a background sampler thread, breadcrumb/file logging, and an
  ImGui overlay. `diag/free_size_classes.h` holds the free-block size
  distribution, shared by the address-space walk and the overlay histogram.
  The overlay reads rescue activity through `rescue_access.h`, **not** through
  `dao::engine_state().evictions`: that counter increments on every call through
  the engine's emergency-evict hook, including the hundreds the game makes
  managing its own cache, so it says nothing about whether lyrium intervened.

**Memory discipline is a hard rule**: the DLL must not consume the game's address
space or CRT heap. `allocators/global_allocator.h` creates a private heap
(`HeapCreate`) and everything persistent goes through it -- use the
`lyrium::Vector`/`Map`/`UnorderedMap`/`String` aliases from `containers/` (std
containers bound to `STDAllocator`) rather than plain std containers; ImGui is
routed through `allocators/imgui_allocator.h` the same way.

Runtime configuration is read from `lyrium.ini` next to the game executable
(`config.h` has every key and its default); logs go to `lyrium_logs/` when
enabled.

### Why interposing on d3d9's allocator cannot work

Two live sessions, two configurations, the same result. With relocation on, the
arena installed correctly and reported `served=0/0kb passed=2936`. With
relocation **off** -- the configuration where the MANAGED duplicates genuinely
exist -- it reported `served=0/0kb passed=1877` while `managed=192643636` sat in
the same log. The hook was live and called 1877 times. Not one call reached the
512 KB threshold.

The reason is the import table of the real `SysWOW64\d3d9.dll`. Its complete
memory surface is `GetProcessHeap`, `HeapAlloc`, `HeapFree` and `LocalAlloc`
from KERNEL32, plus `malloc` and `free` from msvcrt. **No `VirtualAlloc`.** Only
`HeapAlloc` was redirected, so the duplicates arrive through `LocalAlloc` or
msvcrt `malloc` -- or from the display driver's UMD, which d3d9 delegates
surface backing to and which imports nothing of ours at all.

Reaching them would mean interposing `RtlAllocateHeap` process-wide, which is a
larger blast radius than the one that already crashed the game twice on first
contact, and it would carry every allocation the engine, PhysX and CUDA make.
Weigh that against what relocation already measures: 3224 creates and zero
failures over six hours on LAA, against a relocation-off run that reached
`E_OUTOFMEMORY` at create #422 in about two minutes.

The same fact answers pooling and jemalloc/tcmalloc, which get proposed for this
regularly. Both are the right *pattern* -- and pooling is already deployed where
the allocation is ours, in `texture/staging_pool.h`, which measured 96% reuse.
Neither is reachable for memory we do not allocate. A better allocator behind a
call nobody makes is still zero. Separately, jemalloc and tcmalloc take their
chunks with `VirtualAlloc`, and in a 32-bit process those chunks *are* what
fragments the space; the 1.4 MB and 2.8 MB requests that fail here exceed their
bins and become dedicated mappings anyway, which is exactly what the NT heap
already does above its 508 KB threshold.

## Constraints to keep in mind

- Everything is 32-bit: pointers and `size_t` are 4 bytes; address-space math
  assumes the 2 GB user split.
- The addresses and hashes in `dao/targets.h` and `dao/pool_patch.h` are specific
  to one `daorigins.exe` build. Never "fix" a hash or address to make a hook fire;
  the byte verification is the safety mechanism.
- Hooked code runs inside the game's render loop and inside `DllMain` -- be
  conservative about what runs there (no blocking I/O, no game-heap allocation).
