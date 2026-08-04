#include <cstddef>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

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

// sizeof(Node) is deliberately never hardcoded: alignof(std::max_align_t) can
// differ between glibc-i386 and MinGW-w64/i686, so a literal here would make the
// suite measure something different from what ships. See abi_probe_test.
constexpr auto node_size = sizeof(Node);

// Every byte of the page is either a Node header or free payload, so this sum is
// invariant across any sequence of allocate and deallocate calls that returns all
// memory. It is the single strongest statement we can make about the free list.
auto free_bytes_including_headers(Allocator &allocator) -> std::size_t
{
    std::size_t total = 0u;
    for (auto &node : allocator)
    {
        total += node.size + node_size;
    }
    return total;
}

auto node_count(Allocator &allocator) -> std::size_t
{
    std::size_t count = 0u;
    for ([[maybe_unused]] auto &node : allocator)
    {
        ++count;
    }
    return count;
}

// Regression: the allocator used to declare no copy or move operations, so the
// implicitly generated copy constructor duplicated the raw page pointer and both
// objects released the same page. Enforcing this at compile time is what keeps
// the double free unreachable rather than merely untested.
static_assert(!std::is_copy_constructible_v<Allocator>, "copying the allocator would release its page twice");
static_assert(!std::is_copy_assignable_v<Allocator>, "copying the allocator would release its page twice");
static_assert(std::is_nothrow_move_constructible_v<Allocator>);

class FreeList : public ::testing::Test
{
  protected:
    static constexpr auto capacity = std::size_t{4096};

    PageLedger ledger{};
    Allocator allocator{TrackingPages{ledger}, capacity};
};

}

TEST(FreeListOwnership, MoveTransfersThePageExactlyOnce)
{
    PageLedger ledger{};

    {
        Allocator original{TrackingPages{ledger}, 4096u};
        Allocator moved{std::move(original)};
        (void)moved;
    }

    EXPECT_EQ(ledger.allocations(), 1u);
    EXPECT_EQ(ledger.releases(), 1u) << "a moved-from allocator must not release the page it gave away";
}

TEST(FreeListOwnership, MoveAssignmentReleasesBothPagesExactlyOnce)
{
    PageLedger ledger{};

    {
        Allocator first{TrackingPages{ledger}, 4096u};
        Allocator second{TrackingPages{ledger}, 4096u};
        second = std::move(first);
    }

    EXPECT_EQ(ledger.allocations(), 2u);
    EXPECT_EQ(ledger.releases(), 2u) << "the target's own page must be released, and the moved-from one must not be";
}

// Regression: the constructor computed the first node's size as
// `capacity - sizeof(Node)` with no guard, so a capacity below that threshold
// wrapped and the free list advertised close to four gigabytes of free space
// inside a page of a few bytes. An allocator that can never serve a request is an
// illegal state, so it is rejected at construction rather than allowed to exist.
TEST(FreeListOwnership, RejectsACapacityTooSmallForOneNode)
{
    PageLedger ledger{};

    EXPECT_THROW((Allocator{TrackingPages{ledger}, sizeof(Node) - 1u}), std::invalid_argument);
    EXPECT_THROW((Allocator{TrackingPages{ledger}, 0u}), std::invalid_argument);

    EXPECT_EQ(ledger.allocations(), 0u) << "a rejected capacity must not take a page before failing";
}

TEST(FreeListOwnership, AcceptsTheSmallestCapacityThatHoldsANode)
{
    PageLedger ledger{};
    EXPECT_NO_THROW((Allocator{TrackingPages{ledger}, sizeof(Node)}));
}

TEST(FreeListOwnership, AMovedToAllocatorStillOwnsAUsableFreeList)
{
    PageLedger ledger{};
    Allocator original{TrackingPages{ledger}, 4096u};
    Allocator moved{std::move(original)};

    auto *block = moved.allocate(64u);
    ASSERT_NE(block, nullptr);
    moved.deallocate(block);
}

TEST_F(FreeList, StartsAsOneNodeSpanningThePage)
{
    EXPECT_EQ(node_count(allocator), 1u);
    EXPECT_EQ(free_bytes_including_headers(allocator), capacity);
}

TEST_F(FreeList, SplitPreservesTotalBytes)
{
    (void)allocator.allocate(64u);
    EXPECT_EQ(free_bytes_including_headers(allocator), capacity - (64u + node_size));
}

TEST_F(FreeList, AllocateThenDeallocateReturnsEveryByte)
{
    auto *block = allocator.allocate(64u);
    allocator.deallocate(block);

    EXPECT_EQ(node_count(allocator), 1u);
    EXPECT_EQ(free_bytes_including_headers(allocator), capacity);
}

TEST_F(FreeList, DeallocateNullIsANoOp)
{
    const auto before = free_bytes_including_headers(allocator);
    allocator.deallocate(nullptr);
    EXPECT_EQ(free_bytes_including_headers(allocator), before);
}

TEST_F(FreeList, ThrowsWhenNoNodeIsLargeEnough)
{
    EXPECT_THROW((void)allocator.allocate(capacity * 2u), std::bad_alloc);
}

TEST_F(FreeList, CoalescesForwardWhenTheFollowingBlockIsFree)
{
    auto *first = allocator.allocate(64u);
    auto *second = allocator.allocate(64u);

    allocator.deallocate(second);
    allocator.deallocate(first);

    EXPECT_EQ(node_count(allocator), 1u);
    EXPECT_EQ(free_bytes_including_headers(allocator), capacity);
}

TEST_F(FreeList, CoalescesBackwardWhenThePrecedingBlockIsFree)
{
    auto *first = allocator.allocate(64u);
    auto *second = allocator.allocate(64u);

    allocator.deallocate(first);
    allocator.deallocate(second);

    EXPECT_EQ(node_count(allocator), 1u);
    EXPECT_EQ(free_bytes_including_headers(allocator), capacity);
}

TEST_F(FreeList, CoalescesBothDirectionsWhenFillingAHole)
{
    auto *first = allocator.allocate(64u);
    auto *second = allocator.allocate(64u);
    auto *third = allocator.allocate(64u);

    allocator.deallocate(first);
    allocator.deallocate(third);
    ASSERT_GT(node_count(allocator), 1u) << "freeing the ends should leave a hole around the middle block";

    allocator.deallocate(second);

    EXPECT_EQ(node_count(allocator), 1u) << "freeing the middle block should merge the neighbours on both sides";
    EXPECT_EQ(free_bytes_including_headers(allocator), capacity);
}

TEST_F(FreeList, TotalIsIndependentOfDeallocationOrder)
{
    for (const auto order : {0, 1, 2, 3, 4, 5})
    {
        PageLedger local_ledger{};
        Allocator local{TrackingPages{local_ledger}, capacity};

        std::vector<void *> blocks{local.allocate(64u), local.allocate(64u), local.allocate(64u)};

        // Walk all six permutations of three blocks.
        static constexpr int permutations[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        for (const auto index : permutations[order])
        {
            local.deallocate(blocks[static_cast<std::size_t>(index)]);
        }

        EXPECT_EQ(free_bytes_including_headers(local), capacity) << "permutation index " << order;
        EXPECT_EQ(node_count(local), 1u) << "permutation index " << order;
    }
}

TEST_F(FreeList, ReusesAFreedBlockForALaterRequestOfTheSameSize)
{
    auto *first = allocator.allocate(64u);
    allocator.deallocate(first);
    auto *again = allocator.allocate(64u);

    EXPECT_EQ(first, again) << "first fit should hand back the block that was just released";
}

TEST_F(FreeList, ExhaustsThePageWithoutLosingBytes)
{
    std::vector<void *> blocks{};
    try
    {
        for (;;)
        {
            blocks.push_back(allocator.allocate(64u));
        }
    }
    catch (const std::bad_alloc &)
    {
    }

    ASSERT_FALSE(blocks.empty());
    for (auto *block : blocks)
    {
        allocator.deallocate(block);
    }

    EXPECT_EQ(free_bytes_including_headers(allocator), capacity);
    EXPECT_EQ(node_count(allocator), 1u);
}
