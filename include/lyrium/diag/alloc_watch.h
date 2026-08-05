#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <psapi.h>
#include <windows.h>

#include "lyrium/dao/targets.h"
#include "lyrium/diag/alloc_attribution.h"
#include "lyrium/diag/alloc_context.h"
#include "lyrium/diag/va_region.h"
#include "lyrium/log.h"

namespace lyrium::diag
{

// The hook window currently open on this thread. Thread-local because two render
// threads must not see each other's, and because reading it costs nothing.
//
// Written only by AllocContextScope from lyrium's own hooks, read only by the
// allocation detours, and then only for allocations already past the threshold.
inline auto current_alloc_context() -> AllocContext &
{
    static thread_local auto context = AllocContext::none;
    return context;
}

// Records an allocation's size, address and the stack that asked for it.
//
// The frame capture has been removed once and restored once, and both moves were
// made on a wrong reading. It was removed as the cause of a cutscene stutter --
// but it was already bounded by the same `index < max_alloc_records` guard the
// record itself is, so it ran at most 256 times in a session and cannot have
// stuttered anything. What sits on the hot path is the detour, on every
// NtAllocateVirtualMemory in the process, and that is still here.
//
// It was then replaced by a single __builtin_return_address(0), on the reasoning
// that one frame was enough to name the module. It is not, and cannot be:
// RtlAllocateHeap and VirtualAlloc are what call NtAllocateVirtualMemory, so
// there is always at least one allocator frame between the client and the
// syscall. A live session resolved all 256 records to ntdll.dll and
// KERNEL32.DLL, which is true and useless.
//
// The version before that captured frames and never read them, and filtered them
// to daorigins.exe's image range -- so an allocation made by d3d9 or by the
// display driver could not have been found even in principle.
//
// So: capture a few frames, only for records that are stored, and actually read
// them. requesting_module() in alloc_attribution.h steps over the allocator
// layers to whatever asked.
struct AllocRecord
{
    std::uint64_t size;
    std::uint64_t result;
    std::uint64_t caller;
    std::uint32_t frames[max_alloc_frames];
    std::uint32_t frame_count;
    AllocContext context;
    std::uint32_t type;
    std::uint32_t protect;
    std::int64_t stamp_us;
};

namespace detail
{

// ntdll's own unwinder, resolved once. Bounded to the records that are stored,
// so a whole session pays for at most max_alloc_records walks.
using CaptureBacktraceFn = ::USHORT(NTAPI *)(::ULONG, ::ULONG, void **, ::PULONG);
inline std::atomic<CaptureBacktraceFn> capture_backtrace{nullptr};

inline auto resolve_capture_backtrace() -> void
{
    if (const auto ntdll = ::GetModuleHandleA("ntdll.dll"); ntdll != nullptr)
    {
        capture_backtrace.store(
            reinterpret_cast<CaptureBacktraceFn>(
                reinterpret_cast<void *>(::GetProcAddress(ntdll, "RtlCaptureStackBackTrace"))),
            std::memory_order_relaxed);
    }
}

// Fills a record's frames. `skip` drops our own detour frame so frame 0 is the
// allocator that called the syscall rather than us.
inline auto collect_frames(AllocRecord &record, ::ULONG skip) -> void
{
    record.frame_count = 0u;

    const auto capture = capture_backtrace.load(std::memory_order_relaxed);
    if (capture == nullptr)
    {
        return;
    }

    void *addresses[max_alloc_frames]{};
    const auto captured = capture(skip, static_cast<::ULONG>(max_alloc_frames), addresses, nullptr);

    for (auto i = ::USHORT{0}; i < captured && i < max_alloc_frames; ++i)
    {
        record.frames[i] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(addresses[i]));
    }
    record.frame_count = captured;
}

inline constexpr auto max_alloc_records = std::size_t{256};
inline AllocRecord alloc_records[max_alloc_records]{};
inline std::atomic<std::size_t> alloc_record_count{};
inline std::atomic<std::uint64_t> alloc_records_dropped{};
inline std::atomic<std::uint64_t> alloc_watch_threshold{8ull * 1024ull * 1024ull};

using VirtualAllocFn = ::LPVOID(WINAPI *)(::LPVOID, ::SIZE_T, ::DWORD, ::DWORD);
inline std::atomic<VirtualAllocFn> virtual_alloc_original{nullptr};
inline std::atomic<void **> virtual_alloc_slot{nullptr};

inline auto WINAPI virtual_alloc_detour(::LPVOID address, ::SIZE_T size, ::DWORD allocation_type, ::DWORD protect)
    -> ::LPVOID
{
    const auto original = virtual_alloc_original.load(std::memory_order_relaxed);
    if (original == nullptr)
    {
        return nullptr;
    }

    auto *result = original(address, size, allocation_type, protect);

    if (size >= alloc_watch_threshold.load(std::memory_order_relaxed))
    {
        const auto index = alloc_record_count.fetch_add(1u, std::memory_order_relaxed);
        if (index < max_alloc_records)
        {
            alloc_records[index] = AllocRecord{
                .size = size,
                .result = reinterpret_cast<std::uintptr_t>(result),

                .caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)),
                .frames = {},
                .frame_count = 0u,
                .context = current_alloc_context(),
                .type = allocation_type,
                .protect = protect,
                .stamp_us = 0,
            };
            // Only for records that are kept, so a session pays for at most
            // max_alloc_records walks however badly it goes. skip=1 drops this
            // detour's own frame, leaving frame 0 as the allocator that called
            // the syscall.
            collect_frames(alloc_records[index], 1u);
        }
        else
        {
            alloc_record_count.store(max_alloc_records, std::memory_order_relaxed);
            alloc_records_dropped.fetch_add(1u, std::memory_order_relaxed);
        }
    }

    return result;
}

}

namespace detail
{

using NtAllocateFn = ::LONG(__attribute__((stdcall)) *)(::HANDLE, void **, ::ULONG_PTR, ::SIZE_T *, ::ULONG, ::ULONG);
inline std::atomic<NtAllocateFn> nt_allocate_original{nullptr};
inline std::atomic<std::uint64_t> nt_hooked_target{0};

inline auto __attribute__((stdcall)) nt_allocate_detour(
    ::HANDLE process,
    void **base_address,
    ::ULONG_PTR zero_bits,
    ::SIZE_T *region_size,
    ::ULONG allocation_type,
    ::ULONG protect) -> ::LONG
{
    const auto original = nt_allocate_original.load(std::memory_order_relaxed);
    if (original == nullptr)
    {
        return -1;
    }

    const auto status = original(process, base_address, zero_bits, region_size, allocation_type, protect);

    const auto size = (region_size != nullptr) ? static_cast<std::uint64_t>(*region_size) : 0u;
    if (status >= 0 && size >= alloc_watch_threshold.load(std::memory_order_relaxed))
    {
        const auto index = alloc_record_count.fetch_add(1u, std::memory_order_relaxed);
        if (index < max_alloc_records)
        {
            alloc_records[index] = AllocRecord{
                .size = size,
                .result = (base_address != nullptr) ? reinterpret_cast<std::uintptr_t>(*base_address) : 0u,
                .caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)),
                .frames = {},
                .frame_count = 0u,
                .context = current_alloc_context(),
                .type = allocation_type,
                .protect = protect,
                .stamp_us = 0,
            };
            // Only for records that are kept, so a session pays for at most
            // max_alloc_records walks however badly it goes. skip=1 drops this
            // detour's own frame, leaving frame 0 as the allocator that called
            // the syscall.
            collect_frames(alloc_records[index], 1u);
        }
        else
        {
            alloc_record_count.store(max_alloc_records, std::memory_order_relaxed);
            alloc_records_dropped.fetch_add(1u, std::memory_order_relaxed);
        }
    }

    return status;
}

inline std::array<std::uint8_t, 16u> observed_prologue{};
inline std::atomic<bool> prologue_recognised{false};
inline std::atomic<std::uint64_t> hooked_target{0};

inline auto recognised_prologue(const std::uint8_t *bytes) -> bool
{
    static constexpr std::uint8_t hotpatch[] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
    static constexpr std::uint8_t classic[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45};
    return std::memcmp(bytes, hotpatch, sizeof(hotpatch)) == 0 || std::memcmp(bytes, classic, sizeof(classic)) == 0;
}

}

inline auto install_virtual_alloc_hook(std::uint64_t threshold_bytes) -> bool
{
    detail::alloc_watch_threshold.store(threshold_bytes, std::memory_order_relaxed);
    detail::resolve_capture_backtrace();

    auto *target = static_cast<std::uint8_t *>(nullptr);
    if (const auto kernelbase = ::GetModuleHandleA("kernelbase.dll"); kernelbase != nullptr)
    {
        target = reinterpret_cast<std::uint8_t *>(::GetProcAddress(kernelbase, "VirtualAlloc"));
    }

    if (target == nullptr)
    {
        const auto kernel32 = ::GetModuleHandleA("kernel32.dll");
        if (kernel32 == nullptr)
        {
            return false;
        }
        target = reinterpret_cast<std::uint8_t *>(::GetProcAddress(kernel32, "VirtualAlloc"));
    }

    if (target == nullptr)
    {
        return false;
    }

    for (auto hop = 0; hop < 4; ++hop)
    {
        if (target[0] == 0xFF && target[1] == 0x25)
        {
            std::uint8_t **indirect = nullptr;
            std::memcpy(&indirect, target + 2, sizeof(indirect));
            if (indirect == nullptr || *indirect == nullptr)
            {
                break;
            }
            target = *indirect;
        }
        else if (target[0] == 0xE9)
        {
            std::int32_t relative = 0;
            std::memcpy(&relative, target + 1, sizeof(relative));
            target = target + 5 + relative;
        }
        else if (std::memcmp(target, "\x8B\xFF\x55\x8B\xEC\x5D\xFF\x25", 8u) == 0)
        {
            std::uint8_t **indirect = nullptr;
            std::memcpy(&indirect, target + 8, sizeof(indirect));
            if (indirect == nullptr || *indirect == nullptr)
            {
                break;
            }
            target = *indirect;
        }
        else
        {
            break;
        }
    }

    std::memcpy(detail::observed_prologue.data(), target, detail::observed_prologue.size());
    if (!detail::recognised_prologue(target))
    {
        return false;
    }
    detail::prologue_recognised.store(true, std::memory_order_relaxed);

    auto *trampoline =
        static_cast<std::uint8_t *>(::VirtualAlloc(nullptr, 64u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr)
    {
        return false;
    }

    static constexpr auto patch_len = std::size_t{5};
    std::memcpy(trampoline, target, patch_len);
    const auto back = static_cast<std::int32_t>((target + patch_len) - (trampoline + patch_len + 5));
    trampoline[patch_len] = 0xE9;
    std::memcpy(trampoline + patch_len + 1, &back, sizeof(back));

    detail::virtual_alloc_original.store(
        reinterpret_cast<detail::VirtualAllocFn>(trampoline), std::memory_order_relaxed);

    auto protection = ::DWORD{};
    if (::VirtualProtect(target, patch_len, PAGE_EXECUTE_READWRITE, &protection) == 0)
    {
        return false;
    }

    const auto jump = static_cast<std::int32_t>(
        reinterpret_cast<std::uint8_t *>(&detail::virtual_alloc_detour) - (target + patch_len));
    target[0] = 0xE9;
    std::memcpy(target + 1, &jump, sizeof(jump));

    ::VirtualProtect(target, patch_len, protection, &protection);
    ::FlushInstructionCache(::GetCurrentProcess(), target, patch_len);

    detail::hooked_target.store(reinterpret_cast<std::uintptr_t>(target), std::memory_order_relaxed);
    return true;
}

namespace detail
{

using MallocFn = void *(__attribute__((cdecl)) *)(std::size_t);
inline std::atomic<MallocFn> malloc_original{nullptr};
inline std::atomic<std::uint64_t> malloc_hooked_target{0};

inline constexpr auto malloc_record_marker = std::uint32_t{0xFFFFFFFFu};

inline auto __attribute__((cdecl)) malloc_detour(std::size_t size) -> void *
{
    const auto original = malloc_original.load(std::memory_order_relaxed);
    if (original == nullptr)
    {
        return nullptr;
    }

    auto *result = original(size);

    if (size >= alloc_watch_threshold.load(std::memory_order_relaxed))
    {
        const auto index = alloc_record_count.fetch_add(1u, std::memory_order_relaxed);
        if (index < max_alloc_records)
        {
            alloc_records[index] = AllocRecord{
                .size = size,
                .result = reinterpret_cast<std::uintptr_t>(result),

                .caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)),
                .frames = {},
                .frame_count = 0u,
                .context = current_alloc_context(),
                .type = malloc_record_marker,
                .protect = 0u,
                .stamp_us = 0,
            };
            // Only for records that are kept, so a session pays for at most
            // max_alloc_records walks however badly it goes. skip=1 drops this
            // detour's own frame, leaving frame 0 as the allocator that called
            // the syscall.
            collect_frames(alloc_records[index], 1u);
        }
        else
        {
            alloc_record_count.store(max_alloc_records, std::memory_order_relaxed);
            alloc_records_dropped.fetch_add(1u, std::memory_order_relaxed);
        }
    }

    return result;
}

}

inline auto install_game_malloc_hook(std::uint64_t threshold_bytes) -> bool
{
    detail::alloc_watch_threshold.store(threshold_bytes, std::memory_order_relaxed);
    detail::resolve_capture_backtrace();

    const auto &entry = dao::target(dao::TargetId::crt_malloc);
    const auto module = ::GetModuleHandleA(nullptr);
    if (module == nullptr)
    {
        return false;
    }

    const auto delta = reinterpret_cast<std::uintptr_t>(module) - dao::preferred_image_base;
    auto *target = reinterpret_cast<std::uint8_t *>(entry.address + delta);

    static constexpr auto patch_len = std::size_t{5};

    auto info = ::MEMORY_BASIC_INFORMATION{};
    if (::VirtualQuery(target, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY |
                         PAGE_READONLY | PAGE_READWRITE)) == 0 ||
        (info.Protect & PAGE_GUARD) != 0)
    {
        return false;
    }

    const auto *region_start = static_cast<const std::uint8_t *>(info.BaseAddress);
    if (static_cast<std::size_t>((region_start + info.RegionSize) - target) < patch_len)
    {
        return false;
    }

    if (std::memcmp(target, entry.prologue, patch_len) != 0)
    {
        return false;
    }

    auto *trampoline =
        static_cast<std::uint8_t *>(::VirtualAlloc(nullptr, 64u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr)
    {
        return false;
    }

    std::memcpy(trampoline, target, patch_len);
    const auto back = static_cast<std::int32_t>((target + patch_len) - (trampoline + patch_len + 5));
    trampoline[patch_len] = 0xE9;
    std::memcpy(trampoline + patch_len + 1, &back, sizeof(back));
    detail::malloc_original.store(reinterpret_cast<detail::MallocFn>(trampoline), std::memory_order_relaxed);

    auto protection = ::DWORD{};
    if (::VirtualProtect(target, patch_len, PAGE_EXECUTE_READWRITE, &protection) == 0)
    {
        return false;
    }

    const auto jump =
        static_cast<std::int32_t>(reinterpret_cast<std::uint8_t *>(&detail::malloc_detour) - (target + patch_len));
    target[0] = 0xE9;
    std::memcpy(target + 1, &jump, sizeof(jump));

    ::VirtualProtect(target, patch_len, protection, &protection);
    ::FlushInstructionCache(::GetCurrentProcess(), target, patch_len);

    detail::malloc_hooked_target.store(reinterpret_cast<std::uintptr_t>(target), std::memory_order_relaxed);
    return true;
}

inline auto install_nt_alloc_hook(std::uint64_t threshold_bytes) -> bool
{
    detail::alloc_watch_threshold.store(threshold_bytes, std::memory_order_relaxed);
    detail::resolve_capture_backtrace();

    const auto ntdll = ::GetModuleHandleA("ntdll.dll");
    if (ntdll == nullptr)
    {
        return false;
    }

    auto *target = reinterpret_cast<std::uint8_t *>(::GetProcAddress(ntdll, "NtAllocateVirtualMemory"));
    if (target == nullptr)
    {
        return false;
    }

    std::memcpy(detail::observed_prologue.data(), target, detail::observed_prologue.size());
    if (target[0] != 0xB8)
    {
        return false;
    }
    detail::prologue_recognised.store(true, std::memory_order_relaxed);

    auto *trampoline =
        static_cast<std::uint8_t *>(::VirtualAlloc(nullptr, 64u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr)
    {
        return false;
    }

    static constexpr auto patch_len = std::size_t{5};
    std::memcpy(trampoline, target, patch_len);
    const auto back = static_cast<std::int32_t>((target + patch_len) - (trampoline + patch_len + 5));
    trampoline[patch_len] = 0xE9;
    std::memcpy(trampoline + patch_len + 1, &back, sizeof(back));

    detail::nt_allocate_original.store(reinterpret_cast<detail::NtAllocateFn>(trampoline), std::memory_order_relaxed);

    auto protection = ::DWORD{};
    if (::VirtualProtect(target, patch_len, PAGE_EXECUTE_READWRITE, &protection) == 0)
    {
        return false;
    }

    const auto jump =
        static_cast<std::int32_t>(reinterpret_cast<std::uint8_t *>(&detail::nt_allocate_detour) - (target + patch_len));
    target[0] = 0xE9;
    std::memcpy(target + 1, &jump, sizeof(jump));

    ::VirtualProtect(target, patch_len, protection, &protection);
    ::FlushInstructionCache(::GetCurrentProcess(), target, patch_len);

    detail::nt_hooked_target.store(reinterpret_cast<std::uintptr_t>(target), std::memory_order_relaxed);
    return true;
}

inline auto install_alloc_watch(std::uint64_t threshold_bytes) -> bool
{
    detail::alloc_watch_threshold.store(threshold_bytes, std::memory_order_relaxed);
    detail::resolve_capture_backtrace();

    auto *base = reinterpret_cast<std::uint8_t *>(::GetModuleHandleA(nullptr));
    if (base == nullptr)
    {
        return false;
    }

    const auto *dos = reinterpret_cast<const ::IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return false;
    }
    const auto *headers = reinterpret_cast<const ::IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE)
    {
        return false;
    }

    const auto directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0)
    {
        return false;
    }

    const auto *import = reinterpret_cast<const ::IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
    for (; import->Name != 0; ++import)
    {
        const auto *names = reinterpret_cast<const ::IMAGE_THUNK_DATA32 *>(base + import->OriginalFirstThunk);
        auto *addresses = reinterpret_cast<::IMAGE_THUNK_DATA32 *>(base + import->FirstThunk);
        if (import->OriginalFirstThunk == 0)
        {
            names = reinterpret_cast<const ::IMAGE_THUNK_DATA32 *>(base + import->FirstThunk);
        }

        for (; names->u1.AddressOfData != 0; ++names, ++addresses)
        {
            if ((names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) != 0)
            {
                continue;
            }

            const auto *by_name = reinterpret_cast<const ::IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(by_name->Name), "VirtualAlloc") != 0)
            {
                continue;
            }

            auto **slot = reinterpret_cast<void **>(&addresses->u1.Function);
            auto protection = ::DWORD{};
            if (::VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection) == 0)
            {
                return false;
            }

            detail::virtual_alloc_original.store(
                reinterpret_cast<detail::VirtualAllocFn>(*slot), std::memory_order_relaxed);
            *slot = reinterpret_cast<void *>(&detail::virtual_alloc_detour);
            detail::virtual_alloc_slot.store(slot, std::memory_order_relaxed);

            ::VirtualProtect(slot, sizeof(void *), protection, &protection);
            return true;
        }
    }

    return false;
}

inline auto remove_alloc_watch() -> void
{
    auto **slot = detail::virtual_alloc_slot.exchange(nullptr, std::memory_order_relaxed);
    const auto original = detail::virtual_alloc_original.load(std::memory_order_relaxed);
    if (slot == nullptr || original == nullptr)
    {
        return;
    }

    auto protection = ::DWORD{};
    if (::VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection) != 0)
    {
        *slot = reinterpret_cast<void *>(original);
        ::VirtualProtect(slot, sizeof(void *), protection, &protection);
    }
}

// Where the large allocations actually land.
//
// The records above were collected and never read, which is why this hook has
// never answered anything. The question it exists for: on a large-address-aware
// process there is a ~2 GB region above the 2 GB line that every session shows
// completely untouched -- largest_free reads 2,146,115,584 in every sample ever
// captured -- while the space below grinds down to slivers. If the allocations
// doing the grinding can be seen, and their caller identified, they can
// potentially be steered up there instead, which would let textures stay in
// D3DPOOL_MANAGED and remove the relocation machinery entirely.
//
// Read without synchronisation from the sampler thread while the render thread
// may be appending. A torn record costs one wrong line in a diagnostic; taking a
// lock on the allocation path would cost far more.
namespace detail
{

// The module a return address belongs to, or 0 if it belongs to none.
//
// GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT so this cannot keep a module
// alive, and this walks the loader list under the loader lock -- which is
// exactly why it runs here, at report time on the sampler thread, and never on
// the allocation path. Doing it inside a hook on NtAllocateVirtualMemory would
// take the loader lock from inside the allocator the loader itself calls.
inline auto module_of(std::uint64_t address) -> std::uint64_t
{
    if (address == 0u)
    {
        return 0u;
    }

    auto module = ::HMODULE{};
    if (::GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<::LPCSTR>(static_cast<std::uintptr_t>(address)),
            &module) == 0)
    {
        return 0u;
    }
    return reinterpret_cast<std::uintptr_t>(module);
}

inline auto module_name_of(std::uint64_t base) -> std::string
{
    if (base == 0u)
    {
        return "unattributed";
    }

    char path[MAX_PATH]{};
    const auto length = ::GetModuleFileNameA(
        reinterpret_cast<::HMODULE>(static_cast<std::uintptr_t>(base)), path, static_cast<::DWORD>(sizeof(path)));
    if (length == 0u)
    {
        return "unknown";
    }

    auto name = std::string{path, length};
    const auto slash = name.find_last_of("\\/");
    return slash == std::string::npos ? name : name.substr(slash + 1u);
}

// The allocator layers a client's request passes through before reaching the
// syscall. Resolved once at report time, not held across the session, because a
// module could in principle be unloaded and reloaded elsewhere.
inline auto allocator_modules() -> AllocatorModules
{
    const auto base_of = [](const char *name) -> std::uint64_t
    { return reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(name)); };

    return AllocatorModules{base_of("ntdll.dll"), base_of("kernel32.dll"), base_of("kernelbase.dll"), 0u};
}

// The module that asked for this allocation, stepping over the allocators.
// Falls back to the immediate caller when the walk produced nothing, so a record
// captured before RtlCaptureStackBackTrace resolved still says something.
inline auto requester_of(const AllocRecord &record, const AllocatorModules &allocators) -> std::uint64_t
{
    auto modules = FrameModules{};
    const auto usable =
        record.frame_count < max_alloc_frames ? record.frame_count : static_cast<std::uint32_t>(max_alloc_frames);
    for (auto i = std::uint32_t{0}; i < usable; ++i)
    {
        modules[i] = module_of(record.frames[i]);
    }

    if (const auto module = requesting_module(modules, usable, allocators); module != 0u)
    {
        return module;
    }
    return module_of(record.caller);
}

}

inline auto report_alloc_records(std::string_view reason) -> void
{
    const auto recorded = detail::alloc_record_count.load(std::memory_order_relaxed);
    if (recorded == 0u)
    {
        return;
    }

    const auto count = recorded < detail::max_alloc_records ? recorded : detail::max_alloc_records;
    auto below = std::uint32_t{};
    auto above = std::uint32_t{};
    auto below_bytes = std::uint64_t{};
    auto above_bytes = std::uint64_t{};
    auto largest = std::uint64_t{};
    auto largest_at = std::uint64_t{};
    auto by_module = AllocAttribution{};
    auto unattributable = std::uint32_t{};
    const auto allocators = detail::allocator_modules();

    for (auto i = std::size_t{0}; i < count; ++i)
    {
        const auto &record = detail::alloc_records[i];
        if (record.result == 0u)
        {
            continue;
        }
        if (!attribute(by_module, detail::requester_of(record, allocators), record.size))
        {
            ++unattributable;
        }
        if (record.result < two_gigabytes)
        {
            ++below;
            below_bytes += record.size;
        }
        else
        {
            ++above;
            above_bytes += record.size;
        }
        if (record.size > largest)
        {
            largest = record.size;
            largest_at = record.result;
        }
    }

    lyrium::log(
        // Deliberately not "below2g", which va[] already uses for a byte count. The
        // two lines sharing a field name that means different things made a
        // grep over the logs silently mix counts with bytes.
        "alloc[{}]: recorded={} low_count={} low_kb={} high_count={} high_kb={} largest_kb={} at={:#x} "
        "modules={} unattributable={}",
        reason,
        recorded,
        below,
        below_bytes / 1024u,
        above,
        above_bytes / 1024u,
        largest / 1024u,
        largest_at,
        attributed_modules(by_module),
        unattributable);

    // One line per module. Kept even though two sessions showed it can only ever
    // name ntdll, kernel32 or kernelbase here: it is how that limit stays
    // visible rather than becoming folklore, and it costs one line.
    for (auto used = attributed_modules(by_module), i = std::size_t{0}; i < used; ++i)
    {
        const auto &row = by_module[i];
        lyrium::log(
            "alloc_by[{}]: module={} base={:#x} count={} kb={} largest_kb={}",
            reason,
            detail::module_name_of(row.module_base),
            row.module_base,
            row.count,
            row.bytes / 1024u,
            row.largest / 1024u);
    }

    // One line per hook window, which is the question the module lines cannot
    // reach: was this allocation caused by a texture create? A d3d_create row
    // whose largest_kb matches the texture being made is the MANAGED duplicate,
    // caught without unwinding anything.
    auto by_context = std::array<ModuleAllocations, alloc_context_count>{};
    for (auto i = std::size_t{0}; i < count; ++i)
    {
        const auto &record = detail::alloc_records[i];
        if (record.result == 0u)
        {
            continue;
        }
        auto &row = by_context[static_cast<std::uint32_t>(record.context) % alloc_context_count];
        ++row.count;
        row.bytes += record.size;
        row.largest = record.size > row.largest ? record.size : row.largest;
    }

    for (auto i = std::uint32_t{0}; i < alloc_context_count; ++i)
    {
        if (by_context[i].count == 0u)
        {
            continue;
        }
        lyrium::log(
            "alloc_in[{}]: hook={} count={} kb={} largest_kb={}",
            reason,
            name_of(static_cast<AllocContext>(i)),
            by_context[i].count,
            by_context[i].bytes / 1024u,
            by_context[i].largest / 1024u);
    }
}

}
