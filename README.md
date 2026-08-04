# lyrium

A proxy `d3d9.dll` for Dragon Age: Origins that stops the address-space
exhaustion behind the late-session texture flickering and crashes.

## Credits

This project stands on two pieces of work that came before it.

**Nathan Baggs** did the original research that identified the problem and
proved it could be solved from a proxy DLL —
[nathan-baggs/mandrel](https://github.com/nathan-baggs/mandrel). Everything here
starts from that diagnosis.

**Matthew (adarec1994)** built [eluvian](https://github.com/adarec1994/eluvian),
the implementation this is forked from. The core technique that makes the fix
work — steering managed textures to the DEFAULT pool and backing their CPU-side
copy with a pagefile-backed file mapping that is only mapped during a lock — is
his, and it survives here essentially unchanged because it is the right design.

lyrium has diverged substantially: the policy layer is rewritten and unit
tested, the eviction behaviour is bounded rather than unbounded, and there is a
test suite where there was none. But the diagnosis and the central mechanism are
inherited, and the project would not exist without either of them.

## The problem

Dragon Age: Origins is a 32-bit game, so it has 2 GB of address space (4 GB with
the widely used LAA patch). The longer you play, the more fragmented that space
becomes. Every texture needs one contiguous block, and when none is available
DirectX returns an out-of-memory error, which produces the texture flickering
and then the crash.

The failure is not exhaustion. In a captured case there was 20 MB free, split
across roughly 430 separate gaps — the memory existed, just never in one piece.

At startup the engine reserves about 795 MB for its own memory pool and roughly
286 MB goes to module images (DLLs, CUDA, PhysX and so on), leaving around
229 MB for managed texture duplicates. It does not happen on console, most
likely because the PC textures are higher resolution.

## How it works

A `MANAGED` texture keeps a full CPU-side duplicate inside that address space.
Creating it in the `DEFAULT` pool removes the duplicate — but DEFAULT textures
cannot be locked, and the engine writes textures by locking them.

So a wrapper stands in. When the engine locks a texture it is handed a temporary
buffer instead, backed by a pagefile file mapping that is only mapped for the
duration of the lock and therefore costs no persistent address space. It fills
that with pixels, the wrapper copies it to the graphics card and discards the
mapping. The engine cannot tell the difference.

Two supporting mechanisms sit behind that. A **placement policy** decides which
textures are eligible — large, block-compressed, not a render target or dynamic
surface — and runs on every texture creation. A **rescue policy** is the
emergency backstop, evicting from the engine's texture cache when the largest
contiguous free block can no longer hold what is being asked for. It is bounded
and rate-limited, so it trims rather than emptying the cache, and it escalates
only if the pressure persists.

In normal play the placement policy does the work and the rescue never fires.

## Installing

Drop `d3d9.dll` next to `DAOrigins.exe`, in `bin_ship`.

Optionally add a `lyrium.ini` beside it:

```
[lyrium]
logging=1
overlay=1
```

`overlay` toggles an ImGui panel with Shift+F12 showing live texture and
address-space figures. `logging` writes a session log to `lyrium_logs/`. Both
are off by default. See `include/lyrium/config.h` for the full set of keys.

If the game does not start, launch `bin_ship\DAOrigins.exe` directly rather than
through the launcher — `DAOriginsConfig.exe` crashes on modern hardware for
reasons unrelated to this mod, and it stops the chain before the game runs.

## Safety

The engine hooks patch fixed addresses in `daorigins.exe`, so they are specific
to one build. Every target is verified against a recorded prologue and a
SHA-256 of its function body **before anything is written**, and if a single one
does not match, nothing is patched at all and the log says which byte differed.
A different build of the game leaves the executable untouched rather than
patched at the wrong address.

## Building

Two configurations, both 32-bit. The DLL needs an i686 MinGW-w64 GCC 14 or
newer; the tests build as native 32-bit Linux binaries so pointer width matches
what ships.

```
cmake --preset dll-win32   && cmake --build --preset dll-win32
cmake --preset tests-linux32 && cmake --build --preset tests-linux32 && ctest --preset tests-linux32
```

See `CLAUDE.md` for the architecture, the toolchain details, and how to read a
session log, and [docs/design-vs-eluvian.md](docs/design-vs-eluvian.md) for
diagrams of what changed from the implementation this forked from and why the
eviction behaviour is safer despite doing less work.
