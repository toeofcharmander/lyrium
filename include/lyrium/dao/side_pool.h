#pragma once

#include <cstddef>
#include <cstdint>

// A second heap for the engine, built out of the engine's own parts.
//
// Main Pool's largest free block ratchets down and never recovers. Measured, it
// falls to 17.5 MB while the engine's largest single request is 71.6 MB, at which
// point the engine stops being able to fit that class and skips assets rather than
// asking -- which is why no counter of failures ever saw it.
//
// The fix is not to stop the ratchet but to take the class that fails out of the
// pool that fragments. At a 1 MB threshold the largest allocation Main Pool ever
// has to satisfy drops below 1 MB, against a measured worst floor of 17.5 MB.
//
// The engine's two spare pool slots hold ECPrivate::BlockPool, which is not a heap
// -- it slices its region into fixed-size sub-pools chosen by the high 16 bits of
// the tag, and handed an ordinary block it tries to allocate more metadata than
// the region holds. So instead lyrium constructs a genuine ECPrivate::Pool with the
// engine's own constructor, on a reservation it owns, and registers it in the free
// slot. It is then indistinguishable from Main Pool to every engine walker.
//
// Freeing needs no cooperation: the engine resolves a block's owning pool by
// address, walking all four slots and range-checking each one's aligned start and
// usable extent. Our arena is a separate reservation, disjoint from Main Pool's
// block, so exactly one pool can ever claim a pointer.

namespace lyrium::dao
{

// Byte offsets into the vtable, from the dump of ECPrivate::Pool at 0x00AEF424.
inline constexpr auto pool_vtable_attach = std::size_t{0x04};
inline constexpr auto pool_vtable_allocate = std::size_t{0x10};
inline constexpr auto pool_vtable_free = std::size_t{0x18};
inline constexpr auto pool_vtable_contains = std::size_t{0x3C};

// ECPrivate::Pool runs to +0x11C, the recursion flag of the critical section the
// constructor initialises at +0x100. Allocating less and calling that constructor
// would write past the end.
inline constexpr auto pool_object_bytes = std::size_t{0x120};

// Deliberately outside the 0..3 the engine uses. Tags are masked to 16 bits before
// the lookup, so an id no engine code names means nothing can reach our pool by
// pushing a tag it already knows.
inline constexpr auto side_pool_id = std::int32_t{0x4C59};

// Attach rounds the base up to 64 KB and subtracts 0x10, so a bare reservation
// would be short of what was asked for.
inline constexpr auto attach_alignment = std::uint64_t{0x10000};

// What Main Pool registers, and therefore what every allocation passing -1 as its
// alignment resolves to.
inline constexpr auto pool_default_align_log2 = std::uint32_t{4};

[[nodiscard]] constexpr auto arena_reservation_bytes(std::uint64_t requested) -> std::uint64_t
{
    return requested + attach_alignment;
}

// Whether an allocation should be diverted to the side pool.
//
// Three conditions, each load-bearing. Size is the point of the whole exercise:
// only the class that Main Pool can no longer fit is moved, and the small-block
// population that does the pinning stays where it is, because relocating that
// would mean holding most of the pool.
//
// A non-zero tag means the engine deliberately chose a pool for this allocation,
// so only what would have gone to Main Pool is eligible -- diverting a tagged
// allocation would change routing nobody here understands.
//
// A zero threshold is off, not "everything". This project already documents that
// a malformed number parses as zero, and that has to fail in the safe direction.
[[nodiscard]] constexpr auto should_route(std::uint32_t size, std::uint64_t threshold, std::int32_t tag) -> bool
{
    return threshold != 0u && size >= threshold && (tag & 0xFFFF) == 0;
}

// The fields of a pool object, read back after registration.
struct PoolFields
{
    std::int32_t id;
    std::uint32_t base;
    std::uint32_t aligned;
    std::uint32_t size;
    std::uint32_t usable;
    std::uint32_t align_log2;
    std::uint32_t min_size;
    std::uint32_t fallback;
};

// Whether the engine actually attached our region.
//
// This carries more weight than it looks. The registrar ends with `mov al,1` and
// discards what attach returned, so a pool whose attach failed still reports
// success and sits in the slot half-built. A non-zero aligned start is the only
// evidence available that it worked, and every other check here exists because
// believing a half-built pool would hand the engine memory it cannot use.
[[nodiscard]] constexpr auto side_pool_is_attached(
    const PoolFields &fields,
    std::uint32_t arena_base,
    std::uint64_t arena_bytes) -> bool
{
    if (fields.id != side_pool_id || fields.aligned == 0u)
    {
        return false;
    }
    if (fields.align_log2 != pool_default_align_log2 || fields.fallback > 1u)
    {
        return false;
    }
    // The aligned start has to be inside the reservation we made, and the usable
    // extent has to be all of it bar the alignment slack.
    const auto end = static_cast<std::uint64_t>(arena_base) + arena_bytes;
    if (fields.aligned < arena_base || fields.aligned >= end)
    {
        return false;
    }
    return fields.usable + attach_alignment >= arena_bytes;
}

}
