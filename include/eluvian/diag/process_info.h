#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <psapi.h>
#include <windows.h>

#include "eluvian/log.h"
#include "eluvian/sha256.h"
#include "eluvian/utils.h"

namespace eluvian::diag
{

struct ImageFlags
{
    bool valid;
    bool large_address_aware;
    bool dynamic_base;
    bool nx_compatible;
    std::uintptr_t preferred_base;
    std::uintptr_t actual_base;
    std::intptr_t base_delta;
    std::uint32_t size_of_image;
    std::uint32_t checksum;
    std::uint32_t timestamp;
};

inline auto read_image_flags() -> ImageFlags
{
    auto flags = ImageFlags{};

    const auto module = ::GetModuleHandleA(nullptr);
    if (module == nullptr)
    {
        return flags;
    }

    const auto *base = reinterpret_cast<const std::uint8_t *>(module);
    const auto *dos = reinterpret_cast<const ::IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return flags;
    }

    const auto *headers = reinterpret_cast<const ::IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE)
    {
        return flags;
    }

    flags.valid = true;
    flags.large_address_aware = (headers->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    flags.dynamic_base = (headers->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) != 0;
    flags.nx_compatible = (headers->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_NX_COMPAT) != 0;
    flags.preferred_base = headers->OptionalHeader.ImageBase;
    flags.actual_base = reinterpret_cast<std::uintptr_t>(module);
    flags.base_delta = static_cast<std::intptr_t>(flags.actual_base - flags.preferred_base);
    flags.size_of_image = headers->OptionalHeader.SizeOfImage;
    flags.checksum = headers->OptionalHeader.CheckSum;
    flags.timestamp = headers->FileHeader.TimeDateStamp;

    return flags;
}

inline auto windows_version() -> std::string
{

    using RtlGetVersionFn = ::LONG(WINAPI *)(::PRTL_OSVERSIONINFOW);

    const auto ntdll = ::GetModuleHandleA("ntdll.dll");
    if (ntdll != nullptr)
    {
        const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtl_get_version != nullptr)
        {
            auto info = ::RTL_OSVERSIONINFOW{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (rtl_get_version(&info) == 0)
            {
                return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." +
                       std::to_string(info.dwBuildNumber);
            }
        }
    }

    return "unknown";
}

inline auto hash_file(const std::filesystem::path &path, std::uint64_t &size_out) -> std::string
{
    size_out = 0u;

    auto file = std::ifstream{path, std::ios::binary};
    if (!file)
    {
        return {};
    }

    auto hasher = Sha256{};
    auto buffer = std::vector<char>(64u * 1024u);
    while (file)
    {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = static_cast<std::size_t>(file.gcount());
        if (read == 0u)
        {
            break;
        }
        hasher.update(buffer.data(), read);
        size_out += read;
    }

    const auto digest = hasher.finish();
    static constexpr auto digits = "0123456789abcdef";
    auto out = std::string{};
    out.reserve(64u);
    for (const auto byte : digest)
    {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0Fu]);
    }
    return out;
}

}
