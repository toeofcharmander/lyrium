#include "lyrium/hooks/prologue.h"
#include <cstdint>
#include <gtest/gtest.h>

using lyrium::hooks::patch_length;

// The three ntdll entry points this project patches, as they actually appear in
// 32-bit ntdll on Windows 11. Read from the binary, not assumed.
TEST(Prologue, HotpatchPrologueIsExactlyFiveBytes)
{
    // mov edi,edi / push ebp / mov ebp,esp -- RtlFreeHeap and RtlSizeHeap
    const std::uint8_t code[] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x6A, 0xFE, 0x68};
    EXPECT_EQ(patch_length(code, sizeof(code)), 5u);
}

// The one that crashed the game. push 0Ch is two bytes and push imm32 is five,
// so the smallest boundary at or past five is seven -- a five byte patch splits
// the immediate and the trampoline runs into garbage.
TEST(Prologue, APrologueWithoutHotpatchPaddingRoundsUpToAnInstructionBoundary)
{
    const std::uint8_t code[] = {0x6A, 0x0C, 0x68, 0x38, 0xE6, 0x39, 0x4B, 0xE8};
    EXPECT_EQ(patch_length(code, sizeof(code)), 7u);
}

// Refusing is the whole point: an unrecognised byte must stop the patch rather
// than let it guess, because guessing wrong executes garbage.
TEST(Prologue, AnUnrecognisedOpcodeIsRefused)
{
    const std::uint8_t code[] = {0x0F, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(patch_length(code, sizeof(code)), 0u);
}

// A relative call or jump cannot be copied to a trampoline unchanged, because
// its displacement is relative to where it sat. Refuse rather than relocate.
TEST(Prologue, ARelativeBranchInThePrologueIsRefused)
{
    const std::uint8_t code[] = {0xE8, 0x08, 0xE5, 0x04, 0x00, 0x8B, 0x75, 0x08};
    EXPECT_EQ(patch_length(code, sizeof(code)), 0u);
}

// Running out of bytes before reaching five must refuse, not read past the end.
TEST(Prologue, ATruncatedPrologueIsRefused)
{
    const std::uint8_t code[] = {0x55, 0x8B, 0xEC};
    EXPECT_EQ(patch_length(code, sizeof(code)), 0u);
}
