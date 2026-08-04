// Pins a real defect in FreeListAllocator and is EXPECTED TO FAIL until it is
// fixed. Labelled known_defect and excluded from the default test preset.
//
// Do not weaken this assertion to make it pass. Fix the allocator.
//
//   ctest --preset known-defects     runs exactly this

#include <cstddef>

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

// DEFECT: the constructor computes the first node's size as `capacity -
// sizeof(Node)` with no guard. When capacity is smaller than sizeof(Node) that
// subtraction wraps, and the free list starts out advertising almost the whole
// address space as available. On the 32-bit build that is a node claiming close
// to four gigabytes inside a page of a few bytes.
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

    EXPECT_LE(advertised, too_small) << "the free list advertises " << advertised << " bytes inside a " << too_small
                                     << " byte page, so capacity - sizeof(Node) wrapped around";
}
