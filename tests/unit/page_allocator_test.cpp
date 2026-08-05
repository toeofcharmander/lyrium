#include <cstddef>

#include <gtest/gtest.h>

#include "lyrium/allocators/page_allocator.h"

using lyrium::PageAllocator;

// PageAllocator is a concept-checked adapter turning two callables into an
// allocate/deallocate pair. It holds no state of its own, which is what lets
// FreeListAllocator be driven by a test double.

TEST(PageAllocatorTest, ForwardsTheRequestedSizeToTheAllocateCallable)
{
    std::size_t seen = 0u;
    auto allocator = PageAllocator{
        [&seen](std::size_t bytes) -> void *
        {
            seen = bytes;
            return nullptr;
        },
        [](void *) {}};

    (void)allocator.allocate(1234u);

    EXPECT_EQ(seen, 1234u);
}

TEST(PageAllocatorTest, ReturnsWhateverTheAllocateCallableProduced)
{
    int storage = 0;
    auto allocator = PageAllocator{[&storage](std::size_t) -> void * { return &storage; }, [](void *) {}};

    EXPECT_EQ(allocator.allocate(8u), &storage);
}

TEST(PageAllocatorTest, ForwardsThePointerToTheDeallocateCallable)
{
    int storage = 0;
    void *seen = nullptr;
    auto allocator = PageAllocator{[](std::size_t) -> void * { return nullptr; }, [&seen](void *page) { seen = page; }};

    allocator.deallocate(&storage);

    EXPECT_EQ(seen, &storage);
}

TEST(PageAllocatorTest, StaysEmptyForStatelessCallables)
{
    // [[no_unique_address]] on the stored callables is what keeps the adapter
    // free, so a stateless pair should not grow the type.
    const auto allocator = PageAllocator{[](std::size_t) -> void * { return nullptr; }, [](void *) {}};

    EXPECT_LE(sizeof(allocator), sizeof(void *));
}
