#pragma once

#include <cstdint>

// How full the engine's main pool is, which is the question a failure counter
// cannot answer.
//
// A session at main_pool_mb=512 lost the Denerim cutscene and every NPC in the
// market while pool_alloc reported 17,071,111 allocations and not one failure.
// The engine is not being refused; it is looking at the pool, deciding an asset
// will not fit, and skipping it without ever asking. So the measurable quantity is
// occupancy, and specifically the largest free block -- a pool with plenty free but
// no single block large enough produces exactly that symptom.
//
// The layout comes from FUN_004ba0a0, which initialises a block header, and
// FUN_004b9a70/FUN_004b9aa0, which walk the chain:
//
//   +0  uint32  size, including this header
//   +5  byte    0xff
//   +6  byte    alignment shift
//   +7  byte    in use
//   next = block + size          (blocks are contiguous)
//   prev = block - *(block - 8)  (boundary tag behind)
//
// So a walk is `cursor += *cursor` from the pool's aligned start. This half is the
// tally and the guards; the walking lives in engine_hooks.cpp.
//
// It deliberately does NOT take the engine's pool lock. Taking an engine mutex from
// the sampler thread is the hazard that already produced one hard hang in this
// project, and a diagnostic is never worth that. The cost is that a walk can race a
// live allocation, so every step is bounded and anything inconsistent abandons the
// sample rather than reporting it.

namespace lyrium::dao
{

// A hard ceiling on the walk, and a backstop rather than an expected outcome. An
// earlier value of 500,000 was reached during ordinary play in Denerim -- the pool
// held over a quarter of a million blocks before the player entered a building --
// so it truncated most samples. A walk that hits this reports itself incomplete;
// its sums are a prefix of the pool, not totals.
inline constexpr auto max_walk_blocks = std::uint32_t{4000000};

struct BlockTally
{
    std::uint32_t blocks;
    std::uint64_t used_bytes;
    std::uint64_t free_bytes;

    // The figure that matters. Free space that is not contiguous cannot hold an
    // asset, so a total on its own says nothing about whether the next load fits.
    std::uint64_t largest_free_bytes;
};

// Folds one block into the tally. Returns false when the block cannot be believed,
// which is the signal to abandon the walk -- a zero size would never advance the
// cursor, and a size past the end of the pool means the read was torn.
[[nodiscard]] constexpr auto tally_block(BlockTally &tally, std::uint32_t size, bool in_use, std::uint64_t remaining)
    -> bool
{
    if (size == 0u || size > remaining)
    {
        return false;
    }

    ++tally.blocks;
    if (in_use)
    {
        tally.used_bytes += size;
    }
    else
    {
        tally.free_bytes += size;
        if (size > tally.largest_free_bytes)
        {
            tally.largest_free_bytes = size;
        }
    }
    return true;
}

}
