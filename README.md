# lyrium

A proxy `d3d9.dll` for Dragon Age: Origins that stops the address-space
exhaustion behind late-session texture flickering and crashes.

## Credits

- **Nathan Baggs** — [nathan-baggs/mandrel](https://github.com/nathan-baggs/mandrel).
  The original research identifying the problem.
- **Matthew (adarec1994)** — [eluvian](https://github.com/adarec1994/eluvian).
  The implementation this is forked from. Texture relocation and the startup
  pool patch are both his.

## The problem

Dragon Age: Origins is 32-bit, so it has 2 GB of address space (4 GB with the
LAA patch). Two things consume it.

**Texture duplicates.** A `D3DPOOL_MANAGED` texture keeps a full CPU-side copy in
that address space. Each copy over 508 KB becomes its own separate reservation,
and they turn over constantly at varying sizes, so the free space is re-cut on
every cycle. Roughly 137 of them per session, about 211 MB. The bytes come back;
the contiguity does not. A texture create needs one unbroken block, and when no
block is large enough DirectX returns out-of-memory — the flickering, then the
crash.

**The engine's startup pool.** The engine reserves 850 MB for itself at launch,
before anything else runs. On a 2 GB install that is most of the space, and it
does not need all of it.

## What it does

**Texture relocation** creates eligible textures in `D3DPOOL_DEFAULT`, which has
no CPU-side copy. DEFAULT textures cannot be locked, so a wrapper hands the
engine a view of a pagefile-backed section for the duration of a lock, copies the
result to the card, and unmaps. The engine cannot tell the difference. On by
default.

**The pool patch** rewrites the immediate in the instruction that sizes the
engine's startup pool. Off by default; see below.

**A rescue path** evicts from the engine's texture cache when the largest free
block can no longer hold what is being asked for. Bounded and rate-limited. On a
correctly configured install it does not fire.

## Installing

Drop `d3d9.dll` next to `DAOrigins.exe`, in `bin_ship`. Optionally add
`lyrium.ini` beside it:

```
[lyrium]
logging=1
overlay=1
main_pool_mb=768
```

`overlay` toggles a panel on Shift+F12. `logging` writes a session log to
`lyrium_logs/`. Both are off by default. `include/lyrium/config.h` has every key.

If the game does not start, launch `bin_ship\DAOrigins.exe` directly rather than
through the launcher — `DAOriginsConfig.exe` crashes on modern hardware for
reasons unrelated to this mod, and it stops the chain before the game runs.

**Run in borderless windowed mode.** Alt-tabbing out of fullscreen makes the game
rebuild the Direct3D device, and the game has a long-standing crash in that path
that has nothing to do with memory. A borderless window never loses the device.

## main_pool_mb

Hands part of the engine's 850 MB startup pool back to the address space. Off by
default because the right value depends on your mod load.

Measured on a heavily modded 2 GB install. Largest unbroken free block below the
2 GB line, during play:

| `main_pool_mb` | largest free block |
|---|---|
| unset (850) | 2.4 MB |
| 800 | 46 MB |
| 768 | 110 MB |
| 704 | 78 MB |
| 512 | engine starves |

Start at 768. Below 704 there is nothing left to gain and real risk.

| symptom | meaning |
|---|---|
| flickering, out-of-memory crashes in long sessions | too high — try 704 |
| missing scenery or characters, hang during a level load | too low — raise it |

The second one is silent. The engine skips whatever did not fit and carries on,
so you get a running game with holes in it rather than an error. Nothing in the
log detects this; only looking at the screen does.

`main_pool_mb=0` restores the stock pool. The patch verifies the exact
instruction and its original value before writing, and declines on a game build
it does not recognise.

On a 4 GB (LAA) install this matters much less — there is a second 2 GB above the
line to fall back on.

## The overlay

Leads with **headroom**: the largest single unbroken block still available, with
a mark where the rescue arms. Below it, how much memory relocation is keeping out
of the address space, and a histogram of free-block sizes from large blocks on
the left to unusable slivers on the right. Fragmentation is that distribution
moving rightward. Folds underneath carry texture, pool and engine counters.

## Safety

The engine hooks patch fixed addresses in `daorigins.exe`. Every target is
verified against a recorded prologue and a SHA-256 of its function body before
anything is written, and if one does not match, nothing is patched and the log
says which byte differed. A different build of the game is left untouched.

## Building

Two configurations, both 32-bit. The DLL needs i686 MinGW-w64 GCC 14 or newer;
the tests build as native 32-bit Linux binaries so pointer width matches what
ships.

```
cmake --preset dll-win32     && cmake --build --preset dll-win32
cmake --preset tests-linux32 && cmake --build --preset tests-linux32 && ctest --preset tests-linux32
```

See `CLAUDE.md` for architecture and toolchain details.
