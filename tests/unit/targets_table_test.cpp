#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>

#include <gtest/gtest.h>

#include "lyrium/dao/targets.h"

using lyrium::dao::prologue_bytes;
using lyrium::dao::TargetId;
using lyrium::dao::targets;

namespace
{

constexpr auto target_count = static_cast<std::size_t>(TargetId::count);

// A near jmp rel32 is 5 bytes. Anything shorter than that cannot be replaced by
// the detour jump without writing past the bytes InlineHook saved for restore.
constexpr auto jump_length = std::size_t{5};

auto is_hex_lower(char c) -> bool
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// A conservative scan for opcodes whose operand is relative to the instruction
// address. The trampoline copies prologue bytes verbatim to a different address
// with no relocation, so any such instruction inside the patched range would
// silently retarget. This inspects raw bytes rather than decoding, so it can only
// ever be a screen -- it is not a substitute for a length decoder.
auto looks_like_relative_branch(std::uint8_t byte) -> bool
{
    return byte == 0xE8u || byte == 0xE9u || byte == 0xEBu || byte == 0xE3u ||
           (byte >= 0xE0u && byte <= 0xE2u) || (byte >= 0x70u && byte <= 0x7Fu);
}

}

TEST(TargetsTable, TableAndEnumAgreeOnSize)
{
    EXPECT_EQ(sizeof(targets) / sizeof(targets[0]), target_count);
}

TEST(TargetsTable, EveryTargetHasTextFields)
{
    for (const auto &entry : targets)
    {
        ASSERT_NE(entry.name, nullptr);
        ASSERT_NE(entry.symbol, nullptr);
        ASSERT_NE(entry.comment, nullptr);
        EXPECT_FALSE(std::string_view{entry.name}.empty());
        EXPECT_FALSE(std::string_view{entry.symbol}.empty());
    }
}

TEST(TargetsTable, NamesSymbolsAndAddressesAreUnique)
{
    std::set<std::string_view> names{};
    std::set<std::string_view> symbols{};
    std::set<std::uintptr_t> addresses{};

    for (const auto &entry : targets)
    {
        EXPECT_TRUE(names.insert(entry.name).second) << "duplicate name " << entry.name;
        EXPECT_TRUE(symbols.insert(entry.symbol).second) << "duplicate symbol " << entry.symbol;
        EXPECT_TRUE(addresses.insert(entry.address).second) << "duplicate address for " << entry.name;
    }
}

TEST(TargetsTable, PatchLengthCanHoldAJumpAndFitsTheSavedPrologue)
{
    for (const auto &entry : targets)
    {
        EXPECT_GE(entry.patch_len, jump_length)
            << entry.name << " has patch_len " << entry.patch_len
            << ", too short for a 5 byte jump; write_jump would overrun the saved originals";
        EXPECT_LE(entry.patch_len, prologue_bytes)
            << entry.name << " has patch_len larger than the " << prologue_bytes << " bytes recorded in prologue[]";
        EXPECT_GE(entry.size, entry.patch_len) << entry.name << " claims a body smaller than the bytes it patches";
    }
}

TEST(TargetsTable, RelocationMaskOnlyDescribesBytesInsideThePatch)
{
    // verify() in inline_hook.h walks [0, patch_len) and consults reloc_mask bit
    // i for each byte. A bit at or above patch_len can never be read, so it is
    // dead data that silently fails to describe anything.
    for (const auto &entry : targets)
    {
        const auto outside = static_cast<std::uint32_t>(entry.reloc_mask) >> entry.patch_len;
        EXPECT_EQ(outside, 0u) << entry.name << " has reloc_mask 0x" << std::hex << entry.reloc_mask << std::dec
                               << " with patch_len " << entry.patch_len
                               << ", so the bits above bit " << (entry.patch_len - 1u)
                               << " are never consulted by verify()";
    }
}

TEST(TargetsTable, DigestsAreSixtyFourLowercaseHexCharacters)
{
    for (const auto &entry : targets)
    {
        ASSERT_NE(entry.sha256, nullptr);
        const auto digest = std::string_view{entry.sha256};

        EXPECT_EQ(digest.size(), 64u) << entry.name << " has a digest of " << digest.size() << " characters";
        for (const auto character : digest)
        {
            EXPECT_TRUE(is_hex_lower(character))
                << entry.name << " digest contains '" << character << "', which is not lowercase hex";
        }
    }
}

TEST(TargetsTable, NoRelativeBranchStartsInsideThePatchedRange)
{
    // The trampoline copies these bytes to a freshly allocated page and does not
    // relocate them, so a relative branch inside the range would jump somewhere
    // unrelated. crt_free and crt_realloc both carry E8 at offset 7 with
    // patch_len 7, which is correct by exactly one byte -- this pins that margin.
    for (const auto &entry : targets)
    {
        for (std::size_t i = 0u; i < entry.patch_len; ++i)
        {
            EXPECT_FALSE(looks_like_relative_branch(entry.prologue[i]))
                << entry.name << " has a byte that looks like a relative branch (0x" << std::hex
                << static_cast<unsigned>(entry.prologue[i]) << std::dec << ") at offset " << i
                << ", inside the " << entry.patch_len << " bytes that get replaced";
        }
    }
}

TEST(TargetsTable, EachTargetIdResolvesToItsOwnEntry)
{
    for (std::size_t i = 0u; i < target_count; ++i)
    {
        const auto &by_id = lyrium::dao::target(static_cast<TargetId>(i));
        EXPECT_EQ(&by_id, &targets[i]) << "target() does not map ordinal " << i << " to table row " << i;
    }
}
