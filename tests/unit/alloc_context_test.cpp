#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/diag/alloc_context.h"

using lyrium::diag::AllocContext;
using lyrium::diag::AllocContextScope;
using lyrium::diag::name_of;

// Attribution by stack walk cannot work here and two live sessions proved it:
// every record resolved to ntdll.dll, KERNEL32.DLL or KERNELBASE.dll, because
// RtlCaptureStackBackTrace follows the EBP chain and optimised 32-bit system
// code omits frame pointers. The walk stops inside the allocator.
//
// So mark the window instead. lyrium already intercepts the exact call that
// creates a MANAGED duplicate; setting a marker across it means an allocation
// can say which of our own hooks it happened inside, with no unwinding at all.

TEST(AllocContext, StartsOutsideAnyHook)
{
    auto current = AllocContext::none;

    EXPECT_EQ(current, AllocContext::none);
}

TEST(AllocContext, MarksTheWindowItCovers)
{
    auto current = AllocContext::none;

    {
        const auto scope = AllocContextScope{current, AllocContext::d3d_create_texture};
        EXPECT_EQ(current, AllocContext::d3d_create_texture);
    }

    EXPECT_EQ(current, AllocContext::none);
}

// The engine's texture create calls into D3D's, so the two windows nest. The
// inner one is the more specific answer and must win while it is open.
TEST(AllocContext, NestedScopesReportTheInnermost)
{
    auto current = AllocContext::none;

    const auto outer = AllocContextScope{current, AllocContext::engine_texture};
    ASSERT_EQ(current, AllocContext::engine_texture);

    {
        const auto inner = AllocContextScope{current, AllocContext::d3d_create_texture};
        EXPECT_EQ(current, AllocContext::d3d_create_texture);
    }

    EXPECT_EQ(current, AllocContext::engine_texture) << "the outer window must survive the inner one closing";
}

// Restoring rather than clearing is the whole reason this is a scope: a hook
// that returns must not blank a window somebody else still has open.
TEST(AllocContext, RestoresRatherThanClears)
{
    auto current = AllocContext::device_reset;

    {
        const auto scope = AllocContextScope{current, AllocContext::d3d_create_texture};
        ASSERT_EQ(current, AllocContext::d3d_create_texture);
    }

    EXPECT_EQ(current, AllocContext::device_reset);
}

TEST(AllocContext, EveryContextHasAName)
{
    EXPECT_STREQ(name_of(AllocContext::none), "none");
    EXPECT_STREQ(name_of(AllocContext::d3d_create_texture), "d3d_create");
    EXPECT_STREQ(name_of(AllocContext::engine_texture), "engine_texture");
    EXPECT_STREQ(name_of(AllocContext::device_reset), "device_reset");
}

// The report keys a fixed table by this, so it has to stay a small dense range.
TEST(AllocContext, FitsTheReportTable)
{
    EXPECT_LT(static_cast<std::uint32_t>(AllocContext::device_reset), lyrium::diag::alloc_context_count);
}

// ---------------------------------------------------------------------------
// Totals that outlive the record buffer.
//
// The detailed records are capped at 256 and a live session filled all of them
// between startup and the first five-second sample. Everything after load-in --
// the churn that actually fragments the space over a session -- was recorded
// nowhere. These totals are counters rather than records, so they cost an atomic
// add and never fill.
// ---------------------------------------------------------------------------

using lyrium::diag::AllocContextTotals;

TEST(AllocContextTotals, StartsAtNothing)
{
    auto totals = AllocContextTotals{};

    EXPECT_EQ(totals.count(AllocContext::d3d_create_texture), 0u);
    EXPECT_EQ(totals.bytes(AllocContext::d3d_create_texture), 0u);
    EXPECT_EQ(totals.largest(AllocContext::d3d_create_texture), 0u);
}

TEST(AllocContextTotals, AccumulatesAgainstTheContext)
{
    auto totals = AllocContextTotals{};

    totals.note(AllocContext::d3d_create_texture, 512u * 1024u);
    totals.note(AllocContext::d3d_create_texture, 1400u * 1024u);

    EXPECT_EQ(totals.count(AllocContext::d3d_create_texture), 2u);
    EXPECT_EQ(totals.bytes(AllocContext::d3d_create_texture), 1912u * 1024u);
}

TEST(AllocContextTotals, KeepsContextsApart)
{
    auto totals = AllocContextTotals{};

    totals.note(AllocContext::d3d_create_texture, 1u * 1024u);
    totals.note(AllocContext::engine_texture, 2u * 1024u);

    EXPECT_EQ(totals.bytes(AllocContext::d3d_create_texture), 1u * 1024u);
    EXPECT_EQ(totals.bytes(AllocContext::engine_texture), 2u * 1024u);
    EXPECT_EQ(totals.bytes(AllocContext::none), 0u);
}

// The largest single request is what separates a context making many small
// allocations from the one making the 1.4 MB request that fails.
TEST(AllocContextTotals, RemembersTheLargestSingleRequest)
{
    auto totals = AllocContextTotals{};

    totals.note(AllocContext::none, 600u * 1024u);
    totals.note(AllocContext::none, 16192u * 1024u);
    totals.note(AllocContext::none, 700u * 1024u);

    EXPECT_EQ(totals.largest(AllocContext::none), 16192u * 1024u);
}

// Unlike the record buffer, this must never stop counting -- the point is the
// part of the session the records could not reach.
TEST(AllocContextTotals, NeverFills)
{
    auto totals = AllocContextTotals{};

    for (auto i = 0; i < 100'000; ++i)
    {
        totals.note(AllocContext::none, 1024u);
    }

    EXPECT_EQ(totals.count(AllocContext::none), 100'000u);
    EXPECT_EQ(totals.bytes(AllocContext::none), 100'000u * 1024u);
}
