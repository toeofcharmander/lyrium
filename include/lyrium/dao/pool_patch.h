#pragma once

#include <cstdint>
#include <cstring>

#include <windows.h>

#include "lyrium/log.h"

namespace lyrium::dao
{

inline constexpr auto main_pool_site = std::uintptr_t{0x004B8F30};
inline constexpr std::uint8_t main_pool_opcode[] = {0xC7, 0x44, 0x24, 0x18};
inline constexpr auto main_pool_default = std::uint32_t{0x35200000};
inline constexpr auto main_pool_immediate_offset = std::size_t{4};

struct PoolPatchResult
{
    bool applied;
    std::uint32_t original_bytes;
    std::uint32_t patched_bytes;
    const char *reason;
};

inline auto patch_main_pool(std::uint32_t new_size_bytes) -> PoolPatchResult
{
    auto result = PoolPatchResult{
        .applied = false, .original_bytes = 0, .patched_bytes = 0, .reason = "not attempted"};

    if (new_size_bytes == 0u)
    {
        result.reason = "disabled";
        return result;
    }

    const auto module = ::GetModuleHandleA(nullptr);
    if (module == nullptr)
    {
        result.reason = "no main module";
        return result;
    }

    const auto delta = reinterpret_cast<std::uintptr_t>(module) - 0x00400000u;
    auto *site = reinterpret_cast<std::uint8_t *>(main_pool_site + delta);

    auto info = ::MEMORY_BASIC_INFORMATION{};
    if (::VirtualQuery(site, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
    {
        result.reason = "site not committed";
        return result;
    }
    const auto *region_start = static_cast<const std::uint8_t *>(info.BaseAddress);
    if (static_cast<std::size_t>((region_start + info.RegionSize) - site) < 8u)
    {
        result.reason = "site too close to the end of its region";
        return result;
    }

    if (std::memcmp(site, main_pool_opcode, sizeof(main_pool_opcode)) != 0)
    {
        result.reason = "opcode mismatch";
        return result;
    }

    auto existing = std::uint32_t{};
    std::memcpy(&existing, site + main_pool_immediate_offset, sizeof(existing));
    result.original_bytes = existing;

    if (existing != main_pool_default)
    {

        result.reason = "unexpected pool size";
        return result;
    }

    auto protection = ::DWORD{};
    if (::VirtualProtect(site, 8u, PAGE_EXECUTE_READWRITE, &protection) == 0)
    {
        result.reason = "could not make the site writable";
        return result;
    }

    std::memcpy(site + main_pool_immediate_offset, &new_size_bytes, sizeof(new_size_bytes));
    ::VirtualProtect(site, 8u, protection, &protection);
    ::FlushInstructionCache(::GetCurrentProcess(), site, 8u);

    result.applied = true;
    result.patched_bytes = new_size_bytes;
    result.reason = "applied";
    return result;
}

}
