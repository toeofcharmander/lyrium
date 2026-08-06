#pragma once

#include <cstddef>
#include <cstdint>

#include "lyrium/dao/pool_budget.h"

// Where the engine keeps the pool it built, so it can be read rather than hooked.
//
// A detour on the registrar cannot work: the pool is created at engine startup,
// through a lazy getter, long before Direct3DCreate9 gives lyrium anywhere to
// install hooks from. A session with the hook installed and verified recorded
// pools=0 -- it had already run and would never run again.
//
// So the figures are read out of the manager instead. From FUN_004b9100, the
// manager holds four pool pointers at +0x4c0 and finds one by comparing the id at
// +0xd4. From FUN_004b93f0, the registrar writes id, base and size; from
// FUN_004ba1d0, the attach step writes the 64 KB-aligned start and the usable
// byte count that survives the alignment.
//
// Nothing here dereferences anything. The reading is mechanism and lives in
// engine_hooks.cpp behind VirtualQuery; this half is the layout and the screen
// that decides whether what came back is a pool at all.

namespace lyrium::dao
{

// Offsets within a pool object.
inline constexpr auto pool_id_offset = std::size_t{0xD4};
inline constexpr auto pool_base_offset = std::size_t{0xD8};
inline constexpr auto pool_aligned_offset = std::size_t{0xDC};
inline constexpr auto pool_size_offset = std::size_t{0xE0};
inline constexpr auto pool_usable_offset = std::size_t{0xE4};

// The manager's array of pools, and how many slots it has.
inline constexpr auto manager_pool_array_offset = std::size_t{0x4C0};
inline constexpr auto manager_pool_slots = std::size_t{4};

// The id the engine's own fallback path looks up when an allocation fails.
inline constexpr auto main_pool_id = std::int32_t{0};

// The manager pointer itself, a global in .data. Unlike everything in targets.h
// this carries no prologue and no hash, so it cannot be verified by comparison --
// which is exactly why every value read through it has to pass the screen below
// before it is believed.
inline constexpr auto manager_pointer_site = std::uintptr_t{0x00C2B584};

// Anything below the engine's own floor is uninitialised memory or a wrong
// offset, not a very small pool: the back-off loop stops rather than going under
// 1 MB. The ceiling is the most a 32-bit process could ever hand over.
inline constexpr auto max_plausible_pool_bytes = std::uint64_t{2u} * 1024u * 1024u * 1024u;

[[nodiscard]] constexpr auto main_pool_reading_is_plausible(std::uint32_t base, std::uint64_t size) -> bool
{
    return base != 0u && size >= main_pool_backoff_step_bytes && size <= max_plausible_pool_bytes;
}

}
