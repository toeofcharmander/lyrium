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
value that drifts therefore breaks the DLL build, not the test run.

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
call, hooks COM interfaces by vtable slot (`hooks/com_hook.h`), installs the
engine hooks, and creates the side pool.

- **Texture relocation** (`resettable_texture.h/.cpp`) — creates eligible
  textures in `D3DPOOL_DEFAULT`. DEFAULT textures cannot be locked, so `LockRect`
  maps a view of a pagefile-backed section and hands the engine a pointer into it;
  `UnlockRect` marks the level owed and the upload is batched at bind time. The
  wrapper is also what lets a DEFAULT texture survive device `Reset`.
  `texture/staging_pool.h` reuses the staging textures by shape.
- **Engine hooks** (`dao/engine_hooks.h`, `dao/inline_hook.h`, `dao/targets.h`) —
  inline hooks at fixed addresses. `dao/targets.h` is the single source of truth:
  each target carries its address, size, SHA-256 and prologue bytes.
- **Side pool** (`dao/side_pool.h`, `dao/pool_layout.h`, `dao/pool_budget.h`) —
  a second heap built from the engine's own allocator class. See below.
- **Pool patch** (`dao/pool_patch.h`) — rewrites the immediate at `0x004B8F30`.
  That figure is a **budget for two allocations, not one pool's size**.
- **Rescue** (`policy/rescue_policy.h`, `policy/rescue_coordinator.h`) — bounded,
  rate-limited eviction from the engine's texture cache under address-space
  pressure. Has never fired in any logged session.
- **Diagnostics** (`diag/`, `stats.h`, `log.h`, `overlay.*`) — VA accounting, a
  sampler thread, breadcrumb and file logging, ImGui overlay. `allocation_watch`
  additionally hooks `NtAllocateVirtualMemory` and counts d3d9's `LocalAlloc` and
  `malloc` imports; off by default.

Policy is separated from mechanism: `policy/`, `dao/pool_budget.h`,
`dao/pool_occupancy.h`, `dao/side_pool.h`, `dao/size_histogram.h` and most of
`diag/` name no D3D or Windows type, so they compile and are tested on Linux.
Anything including `windows.h` or `psapi.h` is unreachable by the test suite.

The DLL must not consume the game's address space or CRT heap.
`allocators/global_allocator.h` creates a private heap and everything persistent
goes through it — use the `lyrium::Vector`/`Map`/`UnorderedMap`/`String` aliases
from `containers/`; ImGui is routed through `allocators/imgui_allocator.h`.

## The memory model

Two things fragment, at two levels, and they are independent. Confusing them
wasted a lot of time; keep them apart.

```
LEVEL 1 — the address space (2 GB, or 4 GB with LAA)
┌──────────────────────────────────────────────────────────────────────┐
│ exe │ dlls │ Strings │   MAIN POOL   │  SIDE POOL  │ textures │ free  │
└──────────────────────────────────────────────────────────────────────┘
                            │  one reservation; Windows never looks inside
                            ▼
LEVEL 2 — inside the main pool
      ┌────────────────────────────────────────────┐
      │▓▓▓ ▓ ▓▓▓░░░░░░░░░░░░░░░░░▓▓░░░░░░░▓ ▓▓░░░░│
      └────────────────────────────────────────────┘
```

| | level 1 | level 2 |
|---|---|---|
| fragmenter | MANAGED texture duplicates | the engine's own small long-lived blocks |
| log line | `va[...]` → `below2g`, `largest_free` | `pool[...]` → `largest_free` |
| fix | texture relocation | the side pool |

Neither fix *prevents* fragmentation. Relocation removes the fragmenter at level 1;
the side pool leaves level 2 fragmenting exactly as before and moves out the only
thing that needed contiguity.

### Level 1 detail

`diag::headroom_bytes` (`diag/va_region.h`) decides which figure binds:
`largest_free` when the image is large-address-aware, `below2g` when it is not.
That figure, not `below2g`, is what the rescue measures against, logged as
`headroom=` beside `laa=` and `low=`.

On LAA, Windows allocates bottom-up and never walks past the 2 GB line until
nothing below fits, so `largest_free` sat at exactly **2,146,115,584** in every
session ever logged while `below2g` fell as low as 12 MB with not one texture
create failing. A small `below2g` there is not a prediction of failure. A zero
reading means "not walked yet" and is deliberately treated as pressure, not safety.

That figure has moved exactly once: at 1969 MB of pools on a 4 GB image it fell to
1,155,137,536 and the side pool was placed at `0x7ffffff0`, right on the line. That
is the only configuration ever observed to use the upper half at all.

### Level 2 detail — what the pool patch actually changes

Read out of the binary with Ghidra (`tools/ghidra/`, output gitignored under
`analysis/`). The whole sequence is `FUN_004b8da0` at `0x004b8da0`, reached once
through a lazy getter at `0x004b92c0`:

```
budget = 0x35200000                       <- 850 MB, the immediate at 0x004B8F30
hHeap  = GetProcessHeap()                 <- the process DEFAULT heap
HeapLock(hHeap)
  "Strings"        0x3700000 (55 MB)    -> HeapAlloc, budget -= 55 MB
  "StreamingMain"  size 0               -> skipped
  "StreamingGPU"   size 0               -> skipped
  while (budget > 1 MB && HeapAlloc(hHeap, 0, budget) == null)
    budget -= 1 MB                        <- backs off until the request fits
HeapUnlock(hHeap)
  register the result as "Main Pool"
```

So `main_pool_mb` is a **budget**: 55 MB goes to `Strings` first, the rest is
requested for `Main Pool`, and if that will not fit the engine settles for less
without saying so. The engine calls neither `HeapCreate` nor `VirtualAlloc` itself;
every other Win32 allocation site in the image belongs to the statically linked
VS2005 CRT (`__heap_init`, `___sbh_alloc_new_region`, `__calloc_impl`).

The manager is `malloc(0x4d4)` at `DAT_00c2b584`. Class names come from MSVC RTTI:

```
+0x020  ECPrivate::Pool        vtable 0x00AEF424   slot 0  "Main Pool"  id 0
+0x140  ECPrivate::StringPool  vtable 0x00AEF7F4   slot 1  "Strings"    id 1
+0x270  ECPrivate::BlockPool   vtable 0x00AEFAC4   slot 2  FREE
+0x398  ECPrivate::BlockPool   vtable 0x00AEFAC4   slot 3  FREE
+0x4c0  Pool* pools[4]         +0x4d0 index (startup scratch, never read again)
```

Pool object fields: `+0xD4` id, `+0xD8` base, `+0xDC` aligned start, `+0xE0` size,
`+0xE4` usable, `+0xE8` default alignment log2, `+0xEC` min size, `+0xF4`
fallback eligibility, `+0x100` lock. Blocks are a boundary-tag free list: size at
header `+0`, alignment shift `+6`, in-use byte `+7`, payload `0x10` past the
header, `next = block + size`.

### Why a small main_pool_mb breaks the game

Measured over two sessions on the same route through Denerim:

| | 4 GB, pool 713 MB | 2 GB, pool 457 MB |
|---|---|---|
| `largest_free` floor | 246 MB | **17.5 MB** |
| largest single request the engine makes | 71.6 MB | 71.6 MB |
| pool alloc failures | 0 | 0 |
| outcome | played fine | missing NPCs, no cutscene, crash |

The ratchet is monotonic and does not recover: one session gave back 330 MB while
`largest_free` did not grow by a byte. Block counts reach 1.4 million at a ~300
byte mean.

Below the line the engine **skips assets it cannot fit rather than asking**, which
is why `failures` stays zero through the whole failure and why no counter of
refusals could ever have detected it. The crash that follows lands inside a
bulk-copy routine, consistent with copying into memory that was never obtained.

Ruled out by measurement: address space is not the mechanism (219 MB of headroom
remained and the rescue never armed), and texture eviction is not a lever
(evictions climbed to 414 while pool usage climbed to 399 MB; the pool and the
texture set are separate arenas, 399 MB against 462 MB).

## The side pool

lyrium constructs a genuine `ECPrivate::Pool` with the engine's own constructor,
on a `VirtualAlloc` reservation it owns, and registers it in slot 2 with the
engine's own registrar. To every engine walker it is then indistinguishable from
Main Pool. Allocations at or above `side_pool_threshold_kb` (default 1 MB) are
served from it by calling its `vtable[4]` directly.

**Freeing needs no cooperation.** The engine resolves a block's owning pool by
address — `FUN_004b8b90` walks all four slots calling `vtable[15]`, a range check
on `+0xDC`/`+0xE4`. Our arena is a separate reservation, disjoint from Main Pool's
block, so exactly one pool can ever claim a pointer.

Three call-only rows in `targets.h` carry this: `pool_ctor` `0x004b9850`,
`pool_register` `0x004b93f0`, `pool_get_tag` `0x004b8af0`. They are marked
`call_only`, verify their whole 16-byte prologue rather than the 5-9 a patch would
need, and are excluded from `targets_verify_clean()`'s veto set — an opt-in
feature must not disable texture relocation.

Things that are load-bearing and easy to undo by accident:

- **Do not route by pushing a tag.** `FUN_004b8b20` (push) is a silent no-op at
  depth 32 while `FUN_004b8b60` (pop) always decrements, so an unbalanced pair
  permanently misroutes every later allocation on that thread. Direct dispatch also
  keeps the fallback ours rather than resting on the engine global at `0x00BF4628`.
- **`FUN_004b93f0` ends with `mov al,1` and discards the attach result.** A pool
  whose attach failed still reports success. A non-zero `+0xDC` is the only
  evidence it worked, which is why `side_pool_is_attached` exists.
- **The object must be built by `FUN_004b9850`**, not hand-rolled. `FUN_004b90c0`
  makes a direct, non-virtual call to `ECPrivate::Pool`'s size routine regardless
  of what vtable is installed.
- **Never unregister.** Blocks handed out are resolved by address; removing the
  pool would leave their frees with no owner. `remove_engine_hooks` clears the
  armed pointer only.
- The two dormant `StreamingMain`/`StreamingGPU` slots are `BlockPool`, a
  size-banded allocator, not a heap — handed an ordinary block it tries to
  allocate more metadata than the region holds. Nothing routes to them. If the
  flat side pool ever fragments, that is the escalation path.

### Sizing

`plan_pool_split` (`dao/pool_budget.h`) decides in `DllMain`, before the budget is
written, so no combination of config keys can over-commit:

- **LAA image** — additive; the budget is left alone.
- **non-LAA** — the side pool comes out of the budget, so the total is whatever
  `main_pool_mb` said.
- If the subtraction would leave the main pool under `minimum_main_pool_bytes`,
  the side pool is dropped instead. Degrading to today's behaviour is always safe.

Measured: on 2 GB, 768 MB of pools (55 + 425 + 288) runs clean; 960 MB froze during
a map load. The main pool needs capacity only (307 MB peak) since nothing over the
threshold lives there; the side pool needs capacity (178 MB peak) **and** a 71.6 MB
clear run, which is why 192 MB was too small — it spent most of a session at 51-63
MB largest run and got away with it only because the big block was allocated early.
At 288 MB it holds 137-170 MB clear.

On 4 GB the recommendation is 1024/945 -- 55 + 969 + 945 = 1969 MB -- which is what
the README documents because it is what has been run. It is far larger than the
measured working set (the side pool used 74 MB of 945) and that is deliberate: the
space is otherwise unused, and a heavy texture mod load is precisely the case a
tight arena would fail on. It is also the only configuration ever observed to place
an allocation above the 2 GB line -- `largest_free` fell to 1,155,137,536 and the
side pool landed at `0x7ffffff0`. The cost is ~1.9 GB of committed memory, most of
it never touched.

Note that `largest_free` is constant *within* a session -- it drops once when the
pools are placed and then does not move, because nothing else ever allocates above
the line. A figure that never changes is not evidence that the upper half heals; it
is evidence that nothing touches it.

The pool that demonstrably heals is the side pool. On the 945 MB configuration its
largest run went 944.9 -> 803.9 -> 850.4 MB across a session while used rose to 131
MB and fell back to 74 MB, and on the 288 MB configuration 62 -> 63.6 -> 97.4 MB.
Few large blocks mean a freed block's neighbours are usually free too, so they
merge. The main pool never does this: over the same session it went 955.9 -> 768.2
-> 692.6 -> 679.7 MB, and one earlier session gave back 330 MB without the largest
gap growing by a byte.

## Running it

Launch `bin_ship\DAOrigins.exe` directly. The launcher chain runs
`DAOriginsConfig.exe`, which crashes with `0xc0000094` on modern hardware and
stops before the game starts; it imports no `d3d9.dll`, so that crash is never
ours. If `lyrium_logs/` was not created, our code never ran.

A healthy session ends with `va[shutdown]`, `textures[shutdown]`,
`rescue[shutdown]` and `lyrium: log sealed`. `lyrium_breadcrumbs.txt` should end
with `detach: sealed`; if it ends earlier, the last breadcrumb names the
statement that hung.

Shift+F12 toggles the overlay when `overlay=1` is set. It leads with the largest
clear run in each of the three arenas against what each must still fit in one
piece — green above 2x, amber 1-2x, red below. Read against the run that emptied
Denerim, the main pool row would have been red hours before anything showed on
screen.

Log lines worth knowing: `pool[...]` is Main Pool occupancy, `side[...]` the side
pool including `full=` (non-zero means the arena refused and it fell through),
`poolhist[...]` the cumulative request and resident size distributions that set the
threshold.

Four of the ten texture hooks never fire on a stock install
(`decode_texture_memory`, `create_texture_from_memory`,
`create_volume_from_memory`, and `load_texture_file` almost never), because assets
arrive through streaming. Each detour writes a one-time breadcrumb on first entry,
so the breadcrumb file distinguishes a dead path from a broken counter.

**Config traps**, pinned by `config_parse_test.cpp`: every key is parsed
section-blind by `load_config()` except `main_pool_mb` and `side_pool_mb`, which
`d3d9.cpp` reads with `GetPrivateProfileIntA` and which therefore need a literal
`[lyrium]` header. A comment marker is stripped from anywhere in a line including
inside a value. A malformed number becomes zero rather than falling back to the
default — for `side_pool_mb` that means "off", which is the safe direction and is
deliberate. `Config::overlay` is declared `true` while the parser passes a fallback
of `false`.

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
  fire; the byte verification is the safety mechanism. Rows are generated by
  `tools/ghidra/emit_target_rows.py`, which reproduces every existing row byte for
  byte — **generate masks from `4gb/DAOrigins.exe`**, since the 2 GB copy has its
  relocation table stripped and every `reloc_mask` comes back zero.
- Hooked code runs inside the render loop and inside `DllMain` — no blocking I/O,
  no game-heap allocation. The address-space walk (median 15 ms, max 205 ms) and
  the pool walk (up to 17 ms over 1.4 million blocks) must never run on the create
  path or per frame. `engine_state()` is called from the overlay every frame and
  reads cached figures only; `refresh_pool_occupancy()` does the walking and is
  called from the sampler.
- The pool walk deliberately does not take the engine's pool lock. Taking an engine
  mutex from the sampler thread is what produced the one hang this project could
  only recover from by signing out. It is bounded, guarded per block, and abandons
  the sample on anything inconsistent.
- Objects with static storage duration are never destroyed
  (`never_destroyed.h`). Static destructors run after `DllMain` under the loader
  lock with every other thread already terminated by `ExitProcess`, so a mutex a
  dead thread was holding is held forever. The log is sealed explicitly at
  detach, on the exiting thread, taking no locks.
