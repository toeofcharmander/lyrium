#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/dao/side_pool.h"

using lyrium::dao::arena_reservation_bytes;
using lyrium::dao::attach_alignment;
using lyrium::dao::PoolFields;
using lyrium::dao::pool_object_bytes;
using lyrium::dao::side_pool_id;
using lyrium::dao::side_pool_is_attached;

namespace
{

constexpr auto mb = std::uint64_t{1024} * 1024;
constexpr auto arena_base = std::uint32_t{0x30000000};
constexpr auto arena_bytes = std::uint64_t{192} * mb;

// What the fields look like after a registration the engine honoured.
constexpr auto attached() -> PoolFields
{
    return PoolFields{
        .id = side_pool_id,
        .base = arena_base,
        .aligned = arena_base + 0xFFF0,
        .size = static_cast<std::uint32_t>(arena_bytes),
        .usable = static_cast<std::uint32_t>(arena_bytes - 0xFFF0),
        .align_log2 = 4,
        .min_size = 0x28,
        .fallback = 0,
    };
}

}

TEST(SidePool, TheObjectIsExactlyTheSizeTheEngineConstructorExpects)
{
    // ECPrivate::Pool runs to +0x11C (the lock's recursion flag), so 0x120.
    // Allocating less and calling the engine's constructor on it corrupts the heap.
    EXPECT_EQ(pool_object_bytes, 0x120u);
}

TEST(SidePool, TheIdIsOutsideTheRangeTheEngineItselfUses)
{
    // The engine uses 0..3 and masks tags to 16 bits. A distinctive id means no
    // engine path can reach our pool by pushing a tag it already knows about.
    EXPECT_GT(side_pool_id, 3);
    EXPECT_LT(side_pool_id, 0x10000);
}

TEST(SidePool, AReservationCarriesEnoughSlackForTheAttachAlignment)
{
    // Attach rounds the base up to 64 KB and then subtracts 0x10, so a bare
    // reservation would come up short of the size actually asked for.
    EXPECT_EQ(arena_reservation_bytes(192u * mb), 192u * mb + attach_alignment);
}

TEST(SidePool, AProperlyAttachedPoolPasses)
{
    EXPECT_TRUE(side_pool_is_attached(attached(), arena_base, arena_bytes));
}

TEST(SidePool, AZeroAlignedStartIsTheOnlySignalThatAttachFailed)
{
    // The registrar does `mov al,1` unconditionally and discards the attach
    // result, so a failed attach still reports success. +0xDC staying zero is the
    // only evidence available, which makes this the single most important check.
    auto fields = attached();
    fields.aligned = 0;

    EXPECT_FALSE(side_pool_is_attached(fields, arena_base, arena_bytes));
}

TEST(SidePool, AnAlignedStartOutsideOurReservationIsRejected)
{
    // If this fires, some other pool answered or the slot was taken. Allocating
    // from it would hand engine memory to the engine twice.
    auto fields = attached();
    fields.aligned = arena_base - 0x10000;

    EXPECT_FALSE(side_pool_is_attached(fields, arena_base, arena_bytes));
}

TEST(SidePool, TheWrongIdIsRejected)
{
    auto fields = attached();
    fields.id = 0;

    EXPECT_FALSE(side_pool_is_attached(fields, arena_base, arena_bytes));
}

TEST(SidePool, AnAlignmentUnlikeMainPoolsIsRejected)
{
    // Main Pool registers 4, so every allocation whose caller passes -1 gets
    // 16-byte alignment. A different value here silently changes alignment for
    // everything we divert.
    auto fields = attached();
    fields.align_log2 = 1;

    EXPECT_FALSE(side_pool_is_attached(fields, arena_base, arena_bytes));
}

TEST(SidePool, APoolThatLostMostOfItsRegionToAlignmentIsRejected)
{
    auto fields = attached();
    fields.usable = static_cast<std::uint32_t>(arena_bytes / 2u);

    EXPECT_FALSE(side_pool_is_attached(fields, arena_base, arena_bytes));
}

TEST(SidePool, APoolIneligibleForTheEnginesFallbackIsRejected)
{
    // +0xF4 must be 0 or 1 or the engine will not retry against Main Pool when we
    // refuse. We register 0; anything else means the write did not land.
    auto fields = attached();
    fields.fallback = 7;

    EXPECT_FALSE(side_pool_is_attached(fields, arena_base, arena_bytes));
}
