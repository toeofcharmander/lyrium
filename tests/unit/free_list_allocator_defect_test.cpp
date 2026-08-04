// These tests pin real defects in FreeListAllocator and are EXPECTED TO FAIL
// until the allocator is fixed. They are labelled known_defect and excluded from
// the default test preset, so the green loop stays green.
//
// Do not weaken these assertions to make them pass. Fix the allocator.
//
//   ctest --preset known-defects     runs exactly these

#include <cstddef>
#include <limits>

#include <gtest/gtest.h>

#include "lyrium/allocators/free_list_allocator.h"
#include "support/tracking_pages.h"

using lyrium::FreeListAllocator;
using lyrium::test::PageLedger;
using lyrium::test::TrackingPages;

namespace
{

using Allocator = FreeListAllocator<TrackingPages>;
using Node = Allocator::Node;

}

// DEFECT: FreeListAllocator allocates its page in the constructor and releases it
// in the destructor, but declares no copy or move operations. The implicitly
// generated copy constructor copies the raw page_ pointer, so two objects own the
// same page and both release it. That is a double free, and it is reachable by
// writing `auto copy = allocator;`.
//
// Fix: delete the copy operations and add a noexcept move that nulls the source's
// page_, per the rule of five.
TEST(FreeListAllocatorDefect, CopyingDoesNotReleaseThePageTwice)
{
    PageLedger ledger{};

    {
        Allocator original{TrackingPages{ledger}, 4096u};
        auto copy = original;
        (void)copy;
    }

    EXPECT_EQ(ledger.allocations(), 1u) << "one allocator constructed, so exactly one page should be taken";
    EXPECT_EQ(ledger.releases(), 1u) << "the page was released more than once: copying the allocator double frees it";
}

// DEFECT: the constructor computes the first node's size as `capacity -
// sizeof(Node)` with no guard. When capacity is smaller than sizeof(Node) that
// subtraction wraps, and the free list starts out advertising almost the whole
// address space as available. On the 32-bit build that is a node claiming roughly
// four gigabytes inside a page of a few bytes.
//
// Fix: reject a capacity that cannot hold at least one Node.
TEST(FreeListAllocatorDefect, CapacitySmallerThanANodeIsRejectedRatherThanWrapping)
{
    PageLedger ledger{};
    const auto too_small = sizeof(Node) - 1u;

    Allocator allocator{TrackingPages{ledger}, too_small};

    std::size_t advertised = 0u;
    for (auto &node : allocator)
    {
        advertised += node.size;
    }

    EXPECT_LE(advertised, too_small)
        << "the free list advertises " << advertised << " bytes inside a " << too_small
        << " byte page, so capacity - sizeof(Node) wrapped around";
    EXPECT_NE(advertised, std::numeric_limits<std::size_t>::max() - sizeof(Node) + 1u)
        << "advertised size is exactly the wrapped value";
}
