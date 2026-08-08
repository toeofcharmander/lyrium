#pragma once

#include <cstdint>
#include <cstring>

#include <windows.h>

// Redirect an imported function by rewriting the caller's import address table.
//
// This is what lets lyrium reach Direct3DCreate9 without being d3d9.dll. Only one
// file in the game's folder can carry that name, and ReShade wants it, so the
// dinput8 build takes a different proxy slot and patches the game's import entry
// instead of owning the filename.
//
// The entry is a pointer in the loaded image, not code, so nothing here is
// architecture-specific and there is no prologue to preserve. Whatever the entry
// pointed at is returned so the caller can chain to it -- which on a ReShade
// install is ReShade rather than the system DLL, and that is the point.

namespace lyrium::hooks
{

inline auto patch_import(::HMODULE target, const char *module_name, const char *function_name, void *replacement)
    -> void *
{
    if (target == nullptr || module_name == nullptr || function_name == nullptr)
    {
        return nullptr;
    }

    auto *base = reinterpret_cast<std::uint8_t *>(target);
    const auto *dos = reinterpret_cast<::IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return nullptr;
    }

    const auto *nt = reinterpret_cast<::IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return nullptr;
    }

    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0u)
    {
        return nullptr;
    }

    auto *descriptor = reinterpret_cast<::IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0u; ++descriptor)
    {
        const auto *name = reinterpret_cast<const char *>(base + descriptor->Name);
        if (::lstrcmpiA(name, module_name) != 0)
        {
            continue;
        }

        // The names live in OriginalFirstThunk; FirstThunk is what the loader
        // overwrote with real addresses and is therefore what has to be patched.
        if (descriptor->OriginalFirstThunk == 0u)
        {
            continue;
        }

        auto *named = reinterpret_cast<::IMAGE_THUNK_DATA *>(base + descriptor->OriginalFirstThunk);
        auto *live = reinterpret_cast<::IMAGE_THUNK_DATA *>(base + descriptor->FirstThunk);

        for (; named->u1.AddressOfData != 0u; ++named, ++live)
        {
            if ((named->u1.Ordinal & IMAGE_ORDINAL_FLAG) != 0u)
            {
                continue;
            }

            const auto *imported = reinterpret_cast<::IMAGE_IMPORT_BY_NAME *>(base + named->u1.AddressOfData);
            if (std::strcmp(imported->Name, function_name) != 0)
            {
                continue;
            }

            auto protection = ::DWORD{};
            if (::VirtualProtect(&live->u1.Function, sizeof(void *), PAGE_READWRITE, &protection) == 0)
            {
                return nullptr;
            }

            auto *previous = reinterpret_cast<void *>(live->u1.Function);
            live->u1.Function = reinterpret_cast<::ULONG_PTR>(replacement);

            auto restored = ::DWORD{};
            ::VirtualProtect(&live->u1.Function, sizeof(void *), protection, &restored);
            return previous;
        }
    }

    return nullptr;
}

}
