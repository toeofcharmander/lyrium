#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/dao/pool_layout.h"

using lyrium::dao::main_pool_id;
using lyrium::dao::main_pool_reading_is_plausible;
using lyrium::dao::manager_pool_array_offset;
using lyrium::dao::manager_pool_slots;
using lyrium::dao::pool_base_offset;
using lyrium::dao::pool_id_offset;
using lyrium::dao::pool_size_offset;
using lyrium::dao::pool_usable_offset;

namespace
{

constexpr auto mb = std::uint64_t{1024u * 1024u};

// A base somewhere plausible for a large heap block in a 32-bit process.
constexpr auto sane_base = std::uint32_t{0x20000000};

}

TEST(PoolLayout, OffsetsMatchWhatTheEngineWrites)
{
    // Characterisation of two functions, so that a mistyped offset fails here
    // rather than by reading a neighbouring field out of engine memory.
    //
    // FUN_004b93f0 stores param_1[0x35]=id, [0x36]=base, [0x38]=size.
    // FUN_004ba1d0 stores param_1[0x39]=usable, and FUN_004b9100 walks the four
    // pointers at manager+0x4c0 comparing +0xd4.
    EXPECT_EQ(pool_id_offset, 0x35u * 4u);
    EXPECT_EQ(pool_base_offset, 0x36u * 4u);
    EXPECT_EQ(pool_size_offset, 0x38u * 4u);
    EXPECT_EQ(pool_usable_offset, 0x39u * 4u);
    EXPECT_EQ(manager_pool_array_offset, 0x4c0u);
    EXPECT_EQ(manager_pool_slots, 4u);
    EXPECT_EQ(main_pool_id, 0);
}

TEST(PoolLayout, ARealisticReadingIsPlausible)
{
    // 713 MB, what main_pool_mb=768 asks for once the strings pool is taken out.
    EXPECT_TRUE(main_pool_reading_is_plausible(sane_base, 713u * mb));
}

TEST(PoolLayout, ANullBaseIsNotAPool)
{
    EXPECT_FALSE(main_pool_reading_is_plausible(0u, 713u * mb));
}

TEST(PoolLayout, AZeroSizeIsNotAPool)
{
    EXPECT_FALSE(main_pool_reading_is_plausible(sane_base, 0u));
}

TEST(PoolLayout, ASizeBelowTheEnginesOwnFloorIsNotAPool)
{
    // The back-off loop stops rather than going below 1 MB, so the engine itself
    // never produces a pool smaller than that. A smaller reading is uninitialised
    // memory or the wrong offset, not a very small pool.
    EXPECT_FALSE(main_pool_reading_is_plausible(sane_base, 512u * 1024u));
}

TEST(PoolLayout, ASizeLargerThanTheAddressSpaceIsNotAPool)
{
    EXPECT_FALSE(main_pool_reading_is_plausible(sane_base, 3u * 1024u * mb));
}
