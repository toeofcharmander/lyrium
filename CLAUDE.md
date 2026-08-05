# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## What this is

A proxy `d3d9.dll` (project name "lyrium") for Dragon Age: Origins. It is dropped
next to `daorigins.exe`, which loads it instead of the system DLL.

Forked from `adarec1994/eluvian`, which built on `nathan-baggs/mandrel`. Texture
relocation and the startup pool patch are both inherited. What is new here is the
policy layer, the eviction behaviour, the install gate, the diagnostics, and the
test suite.

## Build

Two configurations, two toolchains, driven by `CMakePresets.json`. Both produce
32-bit binaries so `sizeof(void*) == 4` on both sides.

**Shipping DLL — 32-bit Windows.** Needs i686 MinGW-w64 GCC 14 or newer on PATH
(or `LYRIUM_MINGW32_ROOT` set). `utils.h` uses `<print>` and `src/CMakeLists.txt`
requests `cxx_std_26`, so Ubuntu's `g++-mingw-w64-i686-win32` (GCC 13.2) cannot
build this. winlibs is the toolchain source; MSYS2's MINGW32 environment is
deprecated.

```
cmake --preset dll-win32 && cmake --build --preset dll-win32
```

Output is `build/dll-win32/src/d3d9.dll`. It must keep that filename and must
import only system DLLs — check with `objdump -p ... | grep 'DLL Name'`. Any
`libstdc++-6.dll`, `libgcc_s_dw2-1.dll` or `libwinpthread-1.dll` means static
linking broke.

**Tests — 32-bit Linux.** Needs `g++-multilib`.

```
cmake --preset tests-linux32 && cmake --build --preset tests-linux32
ctest --preset tests-linux32
```

Do not put `-m32` in `CMAKE_CXX_FLAGS`. It has to come from the toolchain file so
it applies before CMake probes the compiler; set after `project()` it leaves
`CMAKE_SIZEOF_VOID_P` at 8 and every `try_compile` answers for the wrong ABI.

ImGui and googletest are fetched at configure time.

A test marked `KNOWN_DEFECT` pins a real bug and is expected to fail. Fix the
code, not the assertion. Currently unused.

Formatting: `.clang-format` at the root (Allman, 4-space, 120 columns). Run it on
anything you touch.

## Running it in the game

Unit tests cover the pure logic; most of this code only does anything once loaded
into the game. A change is confirmed only after it has run there.

```
cp build/dll-win32/src/d3d9.dll "<game>/bin_ship/d3d9.dll"   # close the game first
```

**Launch `DAOrigins.exe` directly.** The launcher chain runs
`DAOriginsConfig.exe`, which crashes with `0xc0000094` on modern hardware and
stops the chain before the game starts. It imports no `d3d9.dll`, so that crash
is never ours.

If `bin_ship/lyrium_logs/` was not created, our code never ran — that points at
the launch path. The Windows Application event log names the faulting module. A
DLL failing to load produces a GUI dialog, invisible from WSL: if you are driving
the build from WSL, ask what appeared on screen.

### Config traps

Pinned by `config_parse_test.cpp`. Every key is parsed section-blind by
`load_config()` except `main_pool_mb`, which `d3d9.cpp` reads with
`GetPrivateProfileIntA` and which therefore needs a literal `[lyrium]` section
header. A comment marker is stripped from anywhere in a line including inside a
value. A malformed number becomes zero rather than falling back to the default.
`Config::overlay` is declared `true` while the parser passes a fallback of
`false`, so an absent key gives the opposite of the declared default.

### Reading a log

`va[...]`: `below2g` is the largest single contiguous block below the 2 GB line —
what the overlay's headroom bar shows and what predicts a failed allocation.
`low_total` is how much is free down there in total, however scattered. Small
`below2g` with large `low_total` is fragmentation; both small is exhaustion.

A healthy session ends with `va[shutdown]`, `textures[shutdown]`,
`rescue[shutdown]` and `lyrium: log sealed`. If those are missing the game did not
exit normally. `lyrium_breadcrumbs.txt` should end with `detach: sealed`; if it
ends earlier, the last breadcrumb names the statement that hung.

`engine[...]` carries the engine's own counters and no verdict. See "Pool
starvation is not detectable".

**Installed is not the same as called.** Six of the ten engine hooks are entered
on a stock install: `create_texture_2d`, `create_texture_cached`,
`create_texture_registered`, `stream_load`, `texture_cache_evict` and
`texture_cache_clear`. `decode_texture_memory`, `create_texture_from_memory` and
`create_volume_from_memory` never fire because assets arrive through streaming.
`load_texture_file` fires rarely — two sessions recorded `loads=2` and `loads=3`.
Each detour writes a one-time breadcrumb on first entry, so
`lyrium_breadcrumbs.txt` distinguishes a dead path from a broken counter. Do not
"fix" a zero counter without checking the breadcrumbs.

Verified baseline on the GOG Ultimate Edition: overlay renders, all ten hooks
report `installed` at their preferred addresses with `base_delta == 0`. That
install is large-address-aware.

## Sizing the engine's main pool

`main_pool_mb` rewrites one immediate at `0x004B8F30` —
`mov dword ptr [esp+0x18], 0x35200000` — the size the engine passes when creating
the pool it sub-allocates everything from: level data, meshes, animations, and
decoded asset data on its way to D3D. Default 850 MB. Inherited from eluvian,
which shipped it defaulted to 0.

Measured on the 2 GB install, relocation on, one area, sessions of 4 to 10
minutes. The steady-state column is noisy because the sessions are not
controlled. Rescue activity is the clean signal and orders monotonically.

| pool | steady `below2g` | worst sample | resets | rescue armed |
|---|---|---|---|---|
| 850 (stock) | 2.4 MB | 2.4 MB | — | firing, exhaustion |
| 800 | 46.2 MB | 14.2 MB | 143 | 14 |
| **768** | **110.2 MB** | 26.0 MB | 33 | 2 |
| 704 | 78.2 MB | 78.2 MB | 30 | 0 |
| 640 | 87 MB | 87 MB | — | 0 |
| 512 | — | — | — | starves |

768 is the recommendation.

**The relationship is a threshold, not a slope.** 850 -> 768 returns 82 MB of
pool and gains 108 MB of largest free block. A linear fit on three points
predicted 36 MB at 768; the answer was 110. Do not interpolate this table.

**It is mod-load dependent.** These are one heavily modded install.

### Pool starvation is not detectable

Too large and the address space starves: `E_OUTOFMEMORY`, `failures=` climbs, the
rescue arms, `shape=` names the condition. Loud and handled.

Too small and the engine starves inside its own pool, and neither failure mode is
visible from here:

- At 512 MB, one session ran with `creates=4622 failures=0` while the world was
  visibly missing geometry. The engine skips the asset and carries on.
- At 512 MB, another session hung during a level load with `creates=744` frozen,
  `failures=0`, and 512 MB of contiguous headroom. The engine stops making
  textures rather than failing to.

Neither reaches `create_texture_2d`, because the engine allocates pool memory for
decoded asset data before it ever calls D3D. A build that judged the engine
counters shipped `health=healthy` through both and was removed. Detecting this
properly means hooking the engine's own pool allocator, which nobody has located.

Bias upward — toward the failure that announces itself.

## The MANAGED duplicate

Measured by three probes that agree. **It is `d3d9!LocalAlloc`** — roughly 137
calls and 211 MB per session, largest single request 16 MB, reaching the address
space through `RtlAllocateHeap`'s large path where a request over the NT heap's
508 KB `VirtualMemoryThreshold` gets its own reservation.

| probe | result |
|---|---|
| `d3d9!HeapAlloc` IAT redirect | 1877 calls, none 512 KB or larger |
| context marker across `CreateTexture` | 140 allocations, 245 MB, largest 16,440 KB |
| `d3d9!LocalAlloc` counting shim | 180 large calls, 280 MB, largest 16,384 KB |

The same run with relocation on records one `d3d_create` allocation of 532 KB.

Two attribution methods that cannot work here, both tried:

- **`__builtin_return_address(0)` at the syscall.** `RtlAllocateHeap` and
  `VirtualAlloc` are what call `NtAllocateVirtualMemory`, so there is always an
  allocator frame between the client and the syscall. Everything resolves to
  `ntdll.dll` or `KERNEL32.DLL`.
- **A deeper stack walk.** `RtlCaptureStackBackTrace` follows the EBP chain and
  optimised 32-bit system code omits frame pointers, so eight frames got no
  further than one.

Marking the window with a thread-local across our own hooks
(`diag/alloc_context.h`) is what worked. Set `allocation_watch=1` to reproduce.
The threshold is 512 KB; at the 8 MB it was for a long time the watch sat above
its own subject and recorded nothing.

### Do not rebuild the arena

An arena serving `d3d9!LocalAlloc` from one reservation was built against that
measurement and worked — two runs served 137 allocations and 211.4 MB each,
identical internal state, no fallbacks, holding essentially the whole MANAGED
set. It was removed.

Containment changes the shape; relocation changes the amount. On a 2 GB install
the amount binds: 211 MB tidied still costs 211 MB, 211 MB never allocated costs
nothing. With relocation on there is exactly one large `LocalAlloc` per session
for an arena to catch.

TLSF, slab allocators and segregated free lists all improve how a region is
carved up, and the arena never fragmented internally. jemalloc and tcmalloc take
their chunks with `VirtualAlloc`, and in a 32-bit process those chunks are what
fragments the space.

The escape hazard is unresolved. The first build covered `LocalFree` alone and
died with an access violation in `nvd3dum.dll`. All three of d3d9's freeing
imports were then covered, but two runs recorded `via_heap=0 via_crt=0` — the
added coverage never fired, so it cannot be what stopped the crash.
`nvd3dum.dll` imports `HeapFree`, `HeapSize`, `HeapReAlloc`, `LocalFree`,
`GlobalFree` and `VirtualFree`, none of them ours.

## The alt-tab crash

`0x0046b1b3` / `0x40000015` in `DAOrigins.exe`. Established by disassembly: the
image base is `0x400000` with no ASLR, so WER's `0x0046b1b3` is the return address
of an indirect call through vtable slot 2 inside `D3DGraphicsDriver::ResetDevice`
at `0x0086b030`. `D3DResetable`'s abstract vtable at `0x00b2e32c` holds
`_purecall` in that slot. The engine registers each object into the driver's
reset-broadcast registry from the `D3DResetable` base constructor and removes it
from the base destructor, so anything mid-construction or mid-destruction is in
the list with the abstract vtable installed when the broadcast calls it. Its
`GMutex` is recursive and does not prevent re-entry on the same thread, and it
discards `Reset`'s HRESULT.

Two landings: R6025 with the log still sealing (the CRT's `exit()` runs detach),
and silent death with no detach at all. A MinGW-built DLL cannot raise R6025 —
that is MSVC's pure-virtual trap — so the dialog alone proves the object is the
game's.

**Borderless windowed avoids it.** No device loss on alt-tab means no reset
broadcast. Every crash logged while sizing the pool was this one, always with
plentiful headroom; one went at 33 device resets and another survived 143.

lyrium cannot fix it, only avoid widening the window. Removing two ~13 ms
address-space walks from inside the `Reset` hook took it from easy to reproduce
out to 57 resets and removed a cutscene stutter. Do not move work out of `Reset`
into the draw path — recreating relocated textures lazily at bind time was tried
and hung the machine hard enough to need a sign-out.

## The address-space walk

Median 15 ms, p90 29 ms, max 205 ms. Tracks region count. **It must never run on
the create path.** It is safe on the sampler thread because `sample_va()`
completes before `snapshot_mutex_` is taken, so even a 205 ms walk cannot block
`try_snapshot`.

It ran on the create path anyway for a long time: a failed create called
`sample_now("create_failed")` inline, and one session logged 240 of those at
about 7 ms each. A failure now calls `Sampler::request_sample()`, which sets one
atomic; the sampler thread polls it every 100 ms (`diag/sample_schedule.h`) and a
burst collapses into one walk. So `va[create_failed]` describes the moments after
the failure rather than the instant of it.

## Architecture

Almost all logic lives in headers under `include/lyrium/` (an INTERFACE library
compiled with `-Werror` and `cxx_std_26`). Four translation units build into the
DLL: `src/d3d9.cpp` (proxy entry point and D3D hooks), `src/overlay.cpp`,
`src/engine_hooks.cpp`, and `src/resettable_texture.cpp`.

Load path: `DllMain` at `DLL_PROCESS_ATTACH` verifies every engine target
read-only and, only if all match, patches the pool size immediate
(`dao/pool_patch.h`). The exported `Direct3DCreate9` then loads the real system
`d3d9.dll`, forwards the call, and hooks COM interfaces by vtable slot
(`hooks/com_hook.h`): first `IDirect3D9::CreateDevice`, then device methods.

**Policy is separated from mechanism.** The decisions live in portable headers
that name no D3D or Windows type, so they compile and are tested on Linux:
`policy/texture_placement_policy.h` decides which pool a texture uses,
`policy/rescue_policy.h` decides when and how hard to evict, and
`policy/rescue_coordinator.h` executes plans through abstract seams.

**Arithmetic does not live in a Windows-only header.** Anything that includes
`windows.h` or `psapi.h` cannot be reached by the test suite, so any calculation
placed there is untestable by construction. This was learned from a shipped bug:
the free-block size thresholds existed twice, once descending in the portable
`diag/va_region.h` and once ascending inside `diag/va_space.h`. A test asserted
the ordering of the portable copy and passed while production used the other one,
and the overlay drew a healthy address space as a shattered one. The fix was to
move both the accumulation and the differencing into `diag/free_size_classes.h`.
**A green test proves nothing about an array production does not use.**

**Exit is deliberately destructor-free.** Objects with static storage duration are
never destroyed (`never_destroyed.h`). Static destructors run after `DllMain`
under the loader lock with every other thread already terminated by
`ExitProcess`, so a mutex a dead thread was holding is held forever and anything
that waits on it deadlocks inside the loader. The log is sealed explicitly at
detach, on the exiting thread, taking no locks.

### Mechanisms

- **Pool patch** (`dao/pool_patch.h`) — see "Sizing the engine's main pool".
- **Engine hooks** (`dao/engine_hooks.h`, `dao/inline_hook.h`, `dao/targets.h`) —
  inline hooks into engine functions at fixed addresses. `dao/targets.h` is the
  single source of truth: each target carries its address, size, SHA-256 and
  prologue bytes, and hooks verify those bytes before patching, so they no-op on
  an unknown game build.
- **Texture relocation** (`resettable_texture.h/.cpp`) — steers texture creation
  to `D3DPOOL_DEFAULT`. DEFAULT textures cannot be locked, so `LockRect` maps a
  view of a pagefile-backed section and hands the engine a pointer into it;
  `UnlockRect` records the level as owed and the upload is batched at bind time.
  The same wrapper lets a DEFAULT texture survive device `Reset`.
  `texture/staging_pool.h` reuses the staging textures, keyed by shape — measured
  96% reuse. `texture/dirty_levels.h` is the atomic owed-level bitmask.
- **Rescue** (`policy/rescue_policy.h`, `policy/rescue_coordinator.h`) — bounded,
  rate-limited eviction from the engine's texture cache. The failure ladder is
  capped per pressure episode by `failure_ladder_limit` (default 3) and re-arms
  when pressure is relieved; `ladders=` and `abandoned=` report it. A 2 GB session
  once ran the ladder to its terminal rung sixty times, releasing nothing.
- **Diagnostics** (`diag/`, `stats.h`, `log.h`, `overlay.*`) — VA-space and
  texture accounting, a background sampler thread, breadcrumb/file logging, and an
  ImGui overlay. `diag/free_size_classes.h` holds the free-block size
  distribution, shared by the walk and the histogram. The overlay reads rescue
  activity through `rescue_access.h`, **not** `dao::engine_state().evictions` —
  that counter increments on every call through the engine's emergency-evict hook,
  including the hundreds the game makes managing its own cache.
- **Allocation watch** (`diag/alloc_watch.h`, `alloc_context.h`,
  `alloc_attribution.h`, `import_probe.h`, `size_tally.h`) — off by default behind
  `allocation_watch`. Hooks `NtAllocateVirtualMemory` and counts d3d9's
  `LocalAlloc` and `malloc` imports. This is what identified the MANAGED
  duplicate; keep it for reproducing that.

**Memory discipline is a hard rule**: the DLL must not consume the game's address
space or CRT heap. `allocators/global_allocator.h` creates a private heap
(`HeapCreate`) and everything persistent goes through it — use the
`lyrium::Vector`/`Map`/`UnorderedMap`/`String` aliases from `containers/` rather
than plain std containers; ImGui is routed through
`allocators/imgui_allocator.h` the same way.

## Constraints

- Everything is 32-bit: pointers and `size_t` are 4 bytes.
- The addresses and hashes in `dao/targets.h` and `dao/pool_patch.h` are specific
  to one `daorigins.exe` build. Never "fix" a hash or address to make a hook fire;
  the byte verification is the safety mechanism.
- Hooked code runs inside the game's render loop and inside `DllMain` — no
  blocking I/O, no game-heap allocation.
- **Never patch a prologue you have not measured.** A five-byte inline hook was
  applied to `ntdll!RtlReAllocateHeap`, whose 32-bit prologue is `6A 0C` (2 bytes)
  followed by `68 imm32` (5 bytes). Five lands three bytes inside the second
  instruction, and the game died executing an address it could not read.
  `RtlFreeHeap` and `RtlSizeHeap` begin `8B FF 55 8B EC`, exactly five bytes of
  three whole instructions, which is why two of three hooks worked. The engine
  hooks are safe because `dao/targets.h` carries recorded prologue bytes per
  target. Any new inline hook into code lyrium does not own needs a length
  decoder; one lived at `include/lyrium/hooks/prologue.h` and is in history.
