#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <windows.h>

#include "lyrium/diag/size_tally.h"
#include "lyrium/log.h"
#include "lyrium/utils.h"

namespace lyrium::diag
{

// Counting shims on the two imports that can still be carrying the MANAGED
// texture duplicates.
//
// What is established. With relocation off, 140 allocations totalling 245 MB
// happen inside IDirect3DDevice9::CreateTexture, and with it on there is one of
// 532 KB. At the syscall they arrive through ntdll -- 141 records against those
// 140 -- which is RtlAllocateHeap's large path, where a request over the NT
// heap's 508 KB threshold gets its own reservation instead of being
// sub-allocated. That is what turns 140 textures into 140 separate regions.
//
// What is not. d3d9.dll's HeapAlloc import was redirected for the heap arena and
// measured 1877 calls, not one of them 512 KB or larger, so the duplicates do
// not come through it. Its entire remaining memory surface is LocalAlloc from
// KERNEL32 and malloc from msvcrt, and both reach RtlAllocateHeap. One of them
// carries the 245 MB.
//
// These shims answer which. They count and tail-call, changing nothing, because
// the arena's mistake was building against a guessed call rather than a measured
// one -- and because a probe that alters behaviour cannot be trusted about the
// behaviour it altered.
namespace detail
{

inline SizeTally local_alloc_all{};
inline SizeTally local_alloc_large{};
inline SizeTally crt_malloc_all{};
inline SizeTally crt_malloc_large{};

// Matches the NT heap's VirtualMemoryThreshold, so "large" here means exactly
// the requests that become their own reservation.
inline constexpr auto import_probe_threshold = std::uint64_t{512u * 1024u};

using LocalAllocFn = ::HLOCAL(WINAPI *)(::UINT, ::SIZE_T);
using CrtMallocFn = void *(__attribute__((cdecl)) *)(std::size_t);

inline std::atomic<LocalAllocFn> local_alloc_original{nullptr};
inline std::atomic<CrtMallocFn> crt_malloc_original{nullptr};

inline auto WINAPI local_alloc_shim(::UINT flags, ::SIZE_T bytes) -> ::HLOCAL
{
    const auto original = local_alloc_original.load(std::memory_order_relaxed);
    if (original == nullptr)
    {
        return nullptr;
    }

    local_alloc_all.note(bytes);
    if (bytes >= import_probe_threshold)
    {
        local_alloc_large.note(bytes);
    }

    return original(flags, bytes);
}

inline auto __attribute__((cdecl)) crt_malloc_shim(std::size_t bytes) -> void *
{
    const auto original = crt_malloc_original.load(std::memory_order_relaxed);
    if (original == nullptr)
    {
        return nullptr;
    }

    crt_malloc_all.note(bytes);
    if (bytes >= import_probe_threshold)
    {
        crt_malloc_large.note(bytes);
    }

    return original(bytes);
}

// The IAT slot `module` uses to call `function`, or nullptr.
//
// Separate from the patching so a caller can resolve everything it needs before
// writing anything. An earlier interposer installed its hooks one at a time and
// left the first ones live when a later step failed, which is how a process-wide
// free hook outlived the thing it belonged to.
inline auto find_import_slot(::HMODULE module, std::string_view function) -> void **
{
    auto *base = reinterpret_cast<std::uint8_t *>(module);
    if (base == nullptr)
    {
        return nullptr;
    }

    const auto *dos = reinterpret_cast<const ::IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return nullptr;
    }

    const auto *headers = reinterpret_cast<const ::IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE)
    {
        return nullptr;
    }

    const auto directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0)
    {
        return nullptr;
    }

    for (const auto *import = reinterpret_cast<const ::IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
         import->Name != 0;
         ++import)
    {
        const auto names_rva = import->OriginalFirstThunk != 0 ? import->OriginalFirstThunk : import->FirstThunk;
        const auto *names = reinterpret_cast<const ::IMAGE_THUNK_DATA32 *>(base + names_rva);
        auto *addresses = reinterpret_cast<::IMAGE_THUNK_DATA32 *>(base + import->FirstThunk);

        for (; names->u1.AddressOfData != 0; ++names, ++addresses)
        {
            // Ordinal imports carry no name to match, so they are skipped rather
            // than guessed at.
            if ((names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) != 0)
            {
                continue;
            }

            const auto *by_name = reinterpret_cast<const ::IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (function == reinterpret_cast<const char *>(by_name->Name))
            {
                return reinterpret_cast<void **>(&addresses->u1.Function);
            }
        }
    }

    return nullptr;
}

inline auto write_import_slot(void **slot, void *replacement) -> void *
{
    auto protection = ::DWORD{};
    if (::VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection) == 0)
    {
        return nullptr;
    }

    auto *original = *slot;
    *slot = replacement;

    ::VirtualProtect(slot, sizeof(void *), protection, &protection);
    return original;
}

}

struct ImportProbeResult
{
    bool local_alloc_installed;
    bool crt_malloc_installed;
};

// Installs both shims on `d3d9.dll`, or as many as are present.
//
// Each is independent: d3d9 importing one and not the other is a finding rather
// than a failure, and refusing to install either because one is missing would
// throw away the half that works.
inline auto install_import_probe(::HMODULE system_d3d9) -> ImportProbeResult
{
    auto result = ImportProbeResult{};
    if (system_d3d9 == nullptr)
    {
        return result;
    }

    if (auto **slot = detail::find_import_slot(system_d3d9, "LocalAlloc"); slot != nullptr)
    {
        if (auto *original = detail::write_import_slot(slot, reinterpret_cast<void *>(&detail::local_alloc_shim));
            original != nullptr)
        {
            detail::local_alloc_original.store(
                reinterpret_cast<detail::LocalAllocFn>(original), std::memory_order_relaxed);
            result.local_alloc_installed = true;
        }
    }

    if (auto **slot = detail::find_import_slot(system_d3d9, "malloc"); slot != nullptr)
    {
        if (auto *original = detail::write_import_slot(slot, reinterpret_cast<void *>(&detail::crt_malloc_shim));
            original != nullptr)
        {
            detail::crt_malloc_original.store(
                reinterpret_cast<detail::CrtMallocFn>(original), std::memory_order_relaxed);
            result.crt_malloc_installed = true;
        }
    }

    return result;
}

// A shim reporting calls but no large ones is a working probe on the wrong
// function; one reporting nothing at all never ran. The two read identically in
// a total, which is why calls= is printed beside large=.
inline auto report_import_probe(std::string_view reason) -> void
{
    if (detail::local_alloc_all.count() == 0u && detail::crt_malloc_all.count() == 0u)
    {
        return;
    }

    lyrium::log(
        "imports[{}]: LocalAlloc calls={} kb={} large={} large_kb={} largest_kb={} | "
        "malloc calls={} kb={} large={} large_kb={} largest_kb={}",
        reason,
        detail::local_alloc_all.count(),
        detail::local_alloc_all.bytes() / 1024u,
        detail::local_alloc_large.count(),
        detail::local_alloc_large.bytes() / 1024u,
        detail::local_alloc_all.largest() / 1024u,
        detail::crt_malloc_all.count(),
        detail::crt_malloc_all.bytes() / 1024u,
        detail::crt_malloc_large.count(),
        detail::crt_malloc_large.bytes() / 1024u,
        detail::crt_malloc_all.largest() / 1024u);
}

}
