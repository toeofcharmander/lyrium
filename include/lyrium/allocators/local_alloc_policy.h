#pragma once

#include <cstdint>

namespace lyrium::allocators
{

// Which LocalAlloc calls the arena takes, and what it owes them.
//
// Portable and free of windows.h on purpose. The interposer that uses this
// cannot be reached by the test suite, and a decision made in there is untestable
// by construction -- the rule free_size_classes.h exists to enforce after a
// duplicated threshold array shipped a broken histogram.
//
// Why LocalAlloc at all. With relocation off, 140 allocations totalling 245 MB
// happen inside IDirect3DDevice9::CreateTexture, and a counting shim on
// d3d9.dll's imports found them: LocalAlloc took 104,598 calls, of which 180
// were 512 KB or larger and carried 280 MB, largest 16 MB. malloc took 2 calls
// and no bytes. The heap arena that preceded this redirected HeapAlloc, measured
// 1877 calls with none of them large, and served nothing for two sessions --
// because it was built against a guessed call instead of a measured one.

// The Windows values, restated rather than included, so this header stays
// portable. Pinned by a test against the real numbers.
inline constexpr auto lmem_fixed = std::uint32_t{0x0000};
inline constexpr auto lmem_moveable = std::uint32_t{0x0002};
inline constexpr auto lmem_zeroinit = std::uint32_t{0x0040};

// True when the arena should serve this request rather than pass it through.
//
// Below the threshold the NT heap sub-allocates inside a segment it already
// owns, so the request costs no new region and taking it would be work for
// nothing. At or above it, the heap gives the request its own reservation --
// which is exactly what turns 180 textures into 180 separate regions in a 2 GB
// space.
//
// LMEM_MOVEABLE is refused however large. It makes LocalAlloc return a handle
// the caller must LocalLock before touching, and an arena returns a pointer, so
// serving one would hand back something the caller then dereferences as a
// handle. d3d9.dll imports no LocalLock, so it cannot be asking for moveable
// memory today; this refuses anyway, because the cost of the check is passing
// through a call that never comes and the cost of being wrong is silent
// corruption.
[[nodiscard]] constexpr auto serve_from_arena(std::uint32_t flags, std::uint64_t bytes, std::uint64_t threshold_bytes)
    -> bool
{
    if (bytes == 0u || (flags & lmem_moveable) != 0u)
    {
        return false;
    }
    return bytes >= threshold_bytes;
}

// LPTR is LMEM_FIXED | LMEM_ZEROINIT, the usual way to ask for zeroed memory.
// Arena memory is recycled and therefore dirty, so a served request that asked
// for zeroes must be given them: skipping it hands the runtime garbage where it
// expects zeroes, which surfaces as wrong pixels rather than as a crash.
[[nodiscard]] constexpr auto needs_zeroing(std::uint32_t flags) -> bool
{
    return (flags & lmem_zeroinit) != 0u;
}

}
