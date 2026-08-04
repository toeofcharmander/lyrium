#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

// This suite exists to exercise production arithmetic at the pointer width the
// DLL actually ships with. If these ever disagree with the 32-bit Windows build,
// every allocator and address-space assertion in the suite is measuring
// something other than what runs in the game.

TEST(AbiProbe, PointersAreThirtyTwoBit)
{
    EXPECT_EQ(sizeof(void *), 4u);
    EXPECT_EQ(sizeof(std::size_t), 4u);
    EXPECT_EQ(sizeof(std::uintptr_t), 4u);
}

TEST(AbiProbe, RecordsLayoutFactsThatMayDivergeFromMinGW)
{
    // Deliberately not equality assertions. glibc-i386 and MinGW-w64/i686 derive
    // alignof(std::max_align_t) differently because of long double, and
    // FreeListAllocator::Node is alignas(std::max_align_t). Recording the values
    // makes a divergence visible instead of silent, and is why no test in this
    // suite may hardcode sizeof(Node).
    RecordProperty("alignof_max_align_t", static_cast<int>(alignof(std::max_align_t)));
    RecordProperty("sizeof_max_align_t", static_cast<int>(sizeof(std::max_align_t)));
    RecordProperty("sizeof_long_double", static_cast<int>(sizeof(long double)));
    RecordProperty("sizeof_long_long", static_cast<int>(sizeof(long long)));
    SUCCEED();
}
