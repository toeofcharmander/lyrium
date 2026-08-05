#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "lyrium/allocators/arena.h"

using lyrium::Arena;

namespace
{

constexpr auto kb = std::size_t{1024};
constexpr auto mb = kb * 1024;

// Stands in for one contiguous VirtualAlloc(MEM_RESERVE) region. The arena never
// grows it and never returns it: the whole point is that the process sees one
// reservation for the lifetime, however the inside is carved up.
class FixedRegion
{
  public:
    explicit FixedRegion(std::size_t size)
        : storage_(size)
    {
    }

    [[nodiscard]] auto base() -> std::byte *
    {
        return storage_.data();
    }

    [[nodiscard]] auto size() const -> std::size_t
    {
        return storage_.size();
    }

  private:
    std::vector<std::byte> storage_;
};

}

// Containment: an address inside the reservation is ours and an address outside
// is not, decided by two comparisons. This is what makes interposing on another
// module's frees safe -- nothing else can live in a range we reserved, so the
// test has no false positives.
TEST(Arena, OwnsExactlyItsOwnRange)
{
    auto region = FixedRegion{1 * mb};
    auto arena = Arena{region.base(), region.size()};

    auto *inside = arena.allocate(4 * kb);
    ASSERT_NE(inside, nullptr);

    EXPECT_TRUE(arena.owns(inside));
    EXPECT_TRUE(arena.owns(region.base()));
    EXPECT_TRUE(arena.owns(region.base() + region.size() - 1));

    EXPECT_FALSE(arena.owns(region.base() - 1));
    EXPECT_FALSE(arena.owns(region.base() + region.size()));
    EXPECT_FALSE(arena.owns(nullptr));
}

TEST(Arena, ServesAnAllocationFromInsideTheReservation)
{
    auto region = FixedRegion{1 * mb};
    auto arena = Arena{region.base(), region.size()};

    auto *p = static_cast<std::byte *>(arena.allocate(64 * kb));

    ASSERT_NE(p, nullptr);
    EXPECT_GE(p, region.base());
    EXPECT_LE(p + 64 * kb, region.base() + region.size());
}

// Refusing is the fallback contract: the caller passes the request to the real
// allocator, so a full arena degrades to today's behaviour rather than failing.
TEST(Arena, RefusesWhatItCannotServe)
{
    auto region = FixedRegion{64 * kb};
    auto arena = Arena{region.base(), region.size()};

    EXPECT_EQ(arena.allocate(1 * mb), nullptr);
}

// The healing property. Two neighbours freed must become one block big enough
// for their combined size, or the arena fragments internally exactly the way the
// address space it replaces does.
TEST(Arena, AdjacentFreesCoalesceIntoOneUsableBlock)
{
    // Sized so the two allocations genuinely commit the region: with slack left
    // over the 350 KB request would succeed anyway and the test would pass
    // without coalescing having happened at all.
    auto region = FixedRegion{600 * kb};
    auto arena = Arena{region.base(), region.size()};

    auto *a = arena.allocate(200 * kb);
    auto *b = arena.allocate(200 * kb);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_EQ(arena.allocate(350 * kb), nullptr)
        << "the space must genuinely be committed before the test means anything";

    arena.deallocate(a);
    arena.deallocate(b);

    EXPECT_NE(arena.allocate(350 * kb), nullptr) << "two freed neighbours did not merge";
}

// Repeated churn of varying sizes is what shreds the real address space. Inside
// the arena it must not accumulate, because coalescing runs on every free.
TEST(Arena, ChurnOfVaryingSizesDoesNotAccumulate)
{
    auto region = FixedRegion{1 * mb};
    auto arena = Arena{region.base(), region.size()};

    for (auto round = 0; round < 200; ++round)
    {
        const auto size = static_cast<std::size_t>(16 + (round % 7) * 24) * kb;
        auto *p = arena.allocate(size);
        ASSERT_NE(p, nullptr) << "arena exhausted after " << round << " rounds of allocate-and-free";
        arena.deallocate(p);
    }

    EXPECT_NE(arena.allocate(700 * kb), nullptr) << "churn left the arena unable to serve a large request";
}

// Freeing out of order still has to coalesce, since the engine releases textures
// in whatever order it likes.
TEST(Arena, OutOfOrderFreesStillCoalesce)
{
    auto region = FixedRegion{1 * mb};
    auto arena = Arena{region.base(), region.size()};

    auto *a = arena.allocate(150 * kb);
    auto *b = arena.allocate(150 * kb);
    auto *c = arena.allocate(150 * kb);
    ASSERT_NE(c, nullptr);

    arena.deallocate(b);
    arena.deallocate(a);
    arena.deallocate(c);

    EXPECT_NE(arena.allocate(430 * kb), nullptr);
}

// A pointer the arena does not own must never be freed through it: that is the
// interposition's safety property, and getting it wrong corrupts rather than
// crashes.
TEST(Arena, DeallocatingAForeignPointerIsRefused)
{
    auto region = FixedRegion{1 * mb};
    auto arena = Arena{region.base(), region.size()};
    auto foreign = std::byte{};

    EXPECT_FALSE(arena.deallocate(&foreign));
    EXPECT_FALSE(arena.deallocate(nullptr));
}

TEST(Arena, ReportsWhatItIsHolding)
{
    auto region = FixedRegion{1 * mb};
    auto arena = Arena{region.base(), region.size()};
    EXPECT_EQ(arena.live_allocations(), 0u);

    auto *a = arena.allocate(32 * kb);
    auto *b = arena.allocate(32 * kb);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(arena.live_allocations(), 2u);

    arena.deallocate(a);
    EXPECT_EQ(arena.live_allocations(), 1u);
    arena.deallocate(b);
    EXPECT_EQ(arena.live_allocations(), 0u);
}
