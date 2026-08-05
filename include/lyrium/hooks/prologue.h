#pragma once

#include <cstddef>
#include <cstdint>

namespace lyrium::hooks
{

// How many bytes an inline hook must copy before it can write a 5-byte jump.
//
// Not a disassembler. It recognises the handful of instructions that actually
// begin the functions this project patches, and refuses everything else --
// because the alternative to refusing is guessing, and guessing wrong writes a
// jump into the middle of an instruction.
//
// That is not hypothetical. A blind 5-byte patch was applied to
// ntdll!RtlReAllocateHeap, whose 32-bit prologue on Windows 11 is
//
//     6A 0C              push 0Ch          2 bytes
//     68 38 E6 39 4B     push 4B39E638h    5 bytes
//
// Five bytes lands three bytes into the second push, so the trampoline ended
// `68 38 E6` followed by the jump home -- a push whose immediate was assembled
// out of jump bytes, then execution into nothing. The game died with "the
// instruction at X referenced memory at X", the signature of executing an
// address that cannot be read.
//
// RtlFreeHeap and RtlSizeHeap begin `8B FF 55 8B EC`, the hotpatch prologue,
// which is exactly five bytes of three whole instructions. That is why patching
// those two worked and the third did not, and why validating rather than
// assuming is the difference.
//
// Returns 0 when the prologue cannot be copied safely. Callers must treat that
// as a refusal to install, not as a length.

namespace detail
{

// Length of one instruction at `code`, or 0 if it is not one of the forms this
// deliberately narrow decoder understands.
[[nodiscard]] constexpr auto instruction_length(const std::uint8_t *code, std::size_t available) -> std::size_t
{
    if (available == 0u)
    {
        return 0u;
    }

    switch (code[0])
    {
        // push ebp / push ecx / push ebx / push esi / push edi / push eax
        case 0x50:
        case 0x51:
        case 0x53:
        case 0x55:
        case 0x56:
        case 0x57: return 1u;

        // push imm8
        case 0x6A: return available >= 2u ? 2u : 0u;

        // push imm32
        case 0x68: return available >= 5u ? 5u : 0u;

        // mov r32, r/m32 with a register operand: mov edi,edi and mov ebp,esp
        // both appear in the hotpatch prologue. Only the register-direct form
        // (mod == 11) is understood; anything with a displacement is refused.
        case 0x8B:
        {
            if (available < 2u)
            {
                return 0u;
            }
            const auto modrm = code[1];
            if ((modrm & 0xC0u) == 0xC0u)
            {
                return 2u;
            }
            // mov r32, [ebp+disp8] -- mod 01, rm 101
            if ((modrm & 0xC0u) == 0x40u && (modrm & 0x07u) != 0x04u)
            {
                return available >= 3u ? 3u : 0u;
            }
            return 0u;
        }

        // and r/m32, imm8 -- the `and esp, -8` that begins RtlSizeHeap's body
        case 0x83:
        {
            if (available < 3u)
            {
                return 0u;
            }
            return (code[1] & 0xC0u) == 0xC0u ? 3u : 0u;
        }

        // Relative call and jump. Understood, and deliberately refused: their
        // displacement is relative to where they sit, so copying one to a
        // trampoline silently retargets it.
        case 0xE8:
        case 0xE9: return 0u;

        default: return 0u;
    }
}

}

inline constexpr auto jump_length = std::size_t{5};

[[nodiscard]] constexpr auto patch_length(const std::uint8_t *code, std::size_t available) -> std::size_t
{
    if (code == nullptr)
    {
        return 0u;
    }

    auto covered = std::size_t{0};
    while (covered < jump_length)
    {
        const auto step = detail::instruction_length(code + covered, available - covered);
        if (step == 0u)
        {
            return 0u;
        }
        covered += step;
    }
    return covered;
}

}
