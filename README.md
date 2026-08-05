# lyrium

A proxy `d3d9.dll` for Dragon Age: Origins that stops the address-space
exhaustion behind late-session texture flickering and crashes.

## Credits

- **Nathan Baggs** — [nathan-baggs/mandrel](https://github.com/nathan-baggs/mandrel).
  The original research identifying the problem.
- **Matthew (adarec1994)** — [eluvian](https://github.com/adarec1994/eluvian).
  The implementation this is forked from. Texture relocation and the startup pool
  patch are both his.

## What it does

The game is 32-bit, so it has 2 GB of address space (4 GB with the LAA patch).
Managed textures keep a full CPU-side copy in it, and each copy over 508 KB
becomes its own reservation; they turn over constantly at varying sizes, so the
free space is re-cut every cycle until no unbroken block is left for the next
texture. Separately, the engine reserves 850 MB for itself at launch and does not
need all of it.

lyrium creates eligible textures in `D3DPOOL_DEFAULT`, which has no CPU-side
copy, and hands the engine a temporary pagefile-backed buffer when it locks one.
It can also shrink the engine's startup pool. A bounded eviction path is there as
a backstop and does not fire on a correctly configured install.

## Installing

Drop `d3d9.dll` next to `DAOrigins.exe`, in `bin_ship`. Optionally add
`lyrium.ini` beside it:

```
[lyrium]
overlay=1
logging=1
main_pool_mb=768
```

`overlay` toggles a panel on Shift+F12, `logging` writes to `lyrium_logs/`. Both
are off by default. `include/lyrium/config.h` has every key.

**Run in borderless windowed mode.** Alt-tabbing out of fullscreen makes the game
rebuild the Direct3D device, and it has a long-standing crash in that path that
has nothing to do with memory. A borderless window never loses the device.

If the game does not start, launch `bin_ship\DAOrigins.exe` directly rather than
through the launcher — `DAOriginsConfig.exe` crashes on modern hardware for
reasons unrelated to this mod, and it stops the chain before the game runs.

## main_pool_mb

Hands part of the engine's 850 MB startup pool back to the address space. Off by
default; the right value depends on your mod load.

Measured on a heavily modded 2 GB install — largest unbroken free block during
play:

| `main_pool_mb` | largest free block |
|---|---|
| unset (850) | 2.4 MB |
| 800 | 46 MB |
| 768 | 110 MB |
| 704 | 78 MB |
| 512 | engine starves |

Start at 768. Below 704 there is nothing left to gain.

| symptom | meaning |
|---|---|
| flickering, out-of-memory crashes in long sessions | too high — try 704 |
| missing scenery or characters, hang during a level load | too low — raise it |

The second one is silent: the engine skips whatever did not fit and carries on,
so you get a running game with holes in it rather than an error.

`main_pool_mb=0` restores the stock pool. The patch verifies the exact
instruction and its original value before writing, and declines on a game build
it does not recognise — as do all the engine hooks, which are checked against
recorded prologues and SHA-256 hashes.

On a 4 GB (LAA) install this matters much less; there is a second 2 GB above the
line to fall back on.

## Building

Both configurations are 32-bit. The DLL needs i686 MinGW-w64 GCC 14 or newer; the
tests build as native 32-bit Linux binaries.

```
cmake --preset dll-win32     && cmake --build --preset dll-win32
cmake --preset tests-linux32 && cmake --build --preset tests-linux32 && ctest --preset tests-linux32
```
