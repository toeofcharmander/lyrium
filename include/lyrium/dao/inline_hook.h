#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <tlhelp32.h>
#include <windows.h>

#include "lyrium/dao/targets.h"
#include "lyrium/log.h"
#include "lyrium/sha256.h"
#include "lyrium/utils.h"

namespace lyrium::dao
{

class ThreadFreezer
{
  public:
    ThreadFreezer()
    {
        frozen_.reserve(512u);

        const auto snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            failed_ = true;
            return;
        }

        const auto process_id = ::GetCurrentProcessId();
        const auto self = ::GetCurrentThreadId();

        auto entry = ::THREADENTRY32{};
        entry.dwSize = sizeof(entry);

        if (::Thread32First(snapshot, &entry) != FALSE)
        {
            do
            {
                if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == self)
                {
                    continue;
                }

                if (is_own_thread(static_cast<std::uint32_t>(entry.th32ThreadID)))
                {
                    continue;
                }
                if (frozen_.size() == frozen_.capacity())
                {
                    break;
                }

                const auto thread = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                if (thread == nullptr)
                {
                    ++unreachable_;
                    continue;
                }
                if (::SuspendThread(thread) == static_cast<::DWORD>(-1))
                {
                    ++unreachable_;
                    ::CloseHandle(thread);
                    continue;
                }
                frozen_.push_back(thread);
            }
            while (::Thread32Next(snapshot, &entry) != FALSE);
        }

        ::CloseHandle(snapshot);
    }

    ~ThreadFreezer()
    {
        for (auto *thread : frozen_)
        {
            ::ResumeThread(thread);
            ::CloseHandle(thread);
        }
    }

    ThreadFreezer(const ThreadFreezer &) = delete;
    auto operator=(const ThreadFreezer &) -> ThreadFreezer & = delete;

    auto frozen() const -> std::size_t
    {
        return frozen_.size();
    }

    auto unreachable() const -> std::size_t
    {
        return unreachable_;
    }

    auto failed() const -> bool
    {
        return failed_;
    }

  private:
    std::vector<::HANDLE> frozen_;
    std::size_t unreachable_{0};
    bool failed_{false};
};

inline constexpr auto no_mismatch = std::size_t{0xFFFFFFFFu};

// What verification found. Fixed-size so producing one allocates nothing, which
// matters because this eventually has to run at process attach.
struct VerifyReport
{
    const char *name{""};
    const char *symbol{""};
    std::uintptr_t site{};
    std::size_t patch_len{};
    bool relocated{};

    bool readable{};
    bool prologue_matched{};
    bool body_checked{};
    bool body_matched{};

    std::size_t first_mismatch{no_mismatch};
    std::array<std::uint8_t, prologue_bytes> found{};
    std::array<std::uint8_t, prologue_bytes> expected{};
    std::array<char, 65> computed_hash{};
    const char *table_hash{""};

    [[nodiscard]] auto ok() const -> bool
    {
        return readable && prologue_matched && body_matched;
    }

    [[nodiscard]] auto reason() const -> const char *
    {
        if (!readable)
        {
            return "site not readable";
        }
        if (!prologue_matched)
        {
            return "prologue bytes differ";
        }
        if (!body_matched)
        {
            return "function body hash differs";
        }
        return "verified";
    }
};

class InlineHook
{
  public:
    // Read-only verification producing a report the caller can act on and print.
    // Everything below used to be computed and then discarded by an unadorned
    // `return false`, which is why a version mismatch produced no evidence.
    static auto verify_target(const Target &target, std::intptr_t base_delta) -> VerifyReport
    {
        auto report = VerifyReport{};
        report.name = target.name;
        report.table_hash = target.sha256;
        report.symbol = target.symbol;
        report.site = target.address + static_cast<std::uintptr_t>(base_delta);
        report.relocated = base_delta != 0;
        report.patch_len = target.patch_len;

        const auto *site = reinterpret_cast<const std::uint8_t *>(report.site);
        if (!readable(site, target.size))
        {
            return report;
        }
        report.readable = true;

        for (auto i = std::size_t{0}; i < target.patch_len && i < prologue_bytes; ++i)
        {
            report.found[i] = site[i];
            report.expected[i] = target.prologue[i];

            const auto is_relocatable = (target.reloc_mask & (1u << i)) != 0u;
            if (report.relocated && is_relocatable)
            {
                continue;
            }
            if (site[i] != target.prologue[i] && report.first_mismatch == no_mismatch)
            {
                report.first_mismatch = i;
            }
        }
        report.prologue_matched = report.first_mismatch == no_mismatch;

        // The body hash only means anything at the preferred base, because the
        // bodies contain absolute operands that shift on rebase. It is computed
        // regardless and carried in the report, since a hash to compare against a
        // known dump is the most useful thing to have when a build differs.
        if (!report.relocated)
        {
            const auto hash = Sha256::hex(site, target.size);
            for (std::size_t i = 0u; i < hash.size() && i + 1u < report.computed_hash.size(); ++i)
            {
                report.computed_hash[i] = hash[i];
            }
            report.body_checked = true;
            report.body_matched = hash == target.sha256;
        }
        else
        {
            report.body_matched = true;
        }

        return report;
    }

    enum class Status
    {
        not_installed,
        installed,
        byte_mismatch,
        unreadable,
        no_trampoline_space,
    };

    InlineHook() = default;

    InlineHook(const InlineHook &) = delete;
    auto operator=(const InlineHook &) -> InlineHook & = delete;

    auto install(const Target &target, void *detour, std::intptr_t base_delta) -> bool
    {
        target_ = &target;
        auto *site = reinterpret_cast<std::uint8_t *>(target.address + static_cast<std::uintptr_t>(base_delta));

        if (!readable(site, target.size))
        {
            status_ = Status::unreadable;
            return false;
        }

        if (!verify(site, base_delta))
        {
            status_ = Status::byte_mismatch;
            return false;
        }

        auto *trampoline = allocate_trampoline();
        if (trampoline == nullptr)
        {
            status_ = Status::no_trampoline_space;
            return false;
        }

        std::memcpy(trampoline, site, target.patch_len);
        write_jump(trampoline + target.patch_len, site + target.patch_len);

        std::memcpy(original_.data(), site, target.patch_len);

        auto protection = ::DWORD{};
        if (::VirtualProtect(site, target.patch_len, PAGE_EXECUTE_READWRITE, &protection) == 0)
        {
            status_ = Status::unreadable;
            return false;
        }

        {
            const auto freezer = ThreadFreezer{};

            write_jump(site, static_cast<std::uint8_t *>(detour));

            for (auto i = std::size_t{5}; i < target.patch_len; ++i)
            {
                site[i] = 0x90u;
            }

            ::FlushInstructionCache(::GetCurrentProcess(), site, target.patch_len);
        }

        ::VirtualProtect(site, target.patch_len, protection, &protection);

        site_ = site;
        trampoline_ = trampoline;
        status_ = Status::installed;

        return true;
    }

    auto remove() -> void
    {
        if (status_ != Status::installed || site_ == nullptr || target_ == nullptr)
        {
            return;
        }

        auto protection = ::DWORD{};
        if (::VirtualProtect(site_, target_->patch_len, PAGE_EXECUTE_READWRITE, &protection) != 0)
        {
            {

                const auto freezer = ThreadFreezer{};
                std::memcpy(site_, original_.data(), target_->patch_len);
                ::FlushInstructionCache(::GetCurrentProcess(), site_, target_->patch_len);
            }
            ::VirtualProtect(site_, target_->patch_len, protection, &protection);
        }

        status_ = Status::not_installed;
    }

    auto trampoline() const -> void *
    {
        return trampoline_;
    }

    auto status() const -> Status
    {
        return status_;
    }

    auto installed() const -> bool
    {
        return status_ == Status::installed;
    }

    static auto status_name(Status status) -> const char *
    {
        switch (status)
        {
            case Status::not_installed: return "not_installed";
            case Status::installed: return "installed";
            case Status::byte_mismatch: return "byte_mismatch";
            case Status::unreadable: return "unreadable";
            case Status::no_trampoline_space: return "no_trampoline_space";
        }
        return "unknown";
    }

  private:
    static auto readable(const void *address, std::size_t size) -> bool
    {
        auto info = ::MEMORY_BASIC_INFORMATION{};
        if (::VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
        {
            return false;
        }

        const auto *start = static_cast<const std::uint8_t *>(info.BaseAddress);
        const auto available = info.RegionSize - static_cast<std::size_t>(
                                                     static_cast<const std::uint8_t *>(address) - start);
        return available >= size;
    }

    auto verify(const std::uint8_t *site, std::intptr_t base_delta) -> bool
    {
        const auto relocated = base_delta != 0;
        auto mismatches = std::vector<std::size_t>{};

        for (auto i = std::size_t{0}; i < target_->patch_len; ++i)
        {
            const auto is_relocatable = (target_->reloc_mask & (1u << i)) != 0u;
            if (relocated && is_relocatable)
            {
                continue;
            }
            if (site[i] != target_->prologue[i])
            {
                mismatches.push_back(i);
            }
        }

        auto body_hash = std::string{};
        auto body_matches = true;
        if (!relocated)
        {
            body_hash = Sha256::hex(site, target_->size);
            body_matches = body_hash == target_->sha256;
        }

        if (!mismatches.empty() || !body_matches)
        {
            auto found = std::string{};
            static constexpr auto digits = "0123456789ABCDEF";
            for (auto i = std::size_t{0}; i < target_->patch_len; ++i)
            {
                found.push_back(digits[site[i] >> 4]);
                found.push_back(digits[site[i] & 0x0Fu]);
                found.push_back(' ');
            }

            auto expected = std::string{};
            for (auto i = std::size_t{0}; i < target_->patch_len; ++i)
            {
                expected.push_back(digits[target_->prologue[i] >> 4]);
                expected.push_back(digits[target_->prologue[i] & 0x0Fu]);
                expected.push_back(' ');
            }

            return false;
        }

        return true;
    }

    static auto write_jump(std::uint8_t *at, const std::uint8_t *destination) -> void
    {

        const auto relative = static_cast<std::int32_t>(destination - (at + 5));
        at[0] = 0xE9u;
        std::memcpy(at + 1, &relative, sizeof(relative));
    }

    static auto allocate_trampoline() -> std::uint8_t *
    {
        static constexpr auto slot_size = std::size_t{32};
        static auto *page = static_cast<std::uint8_t *>(nullptr);
        static auto used = std::size_t{0};
        static constexpr auto page_size = std::size_t{4096};

        if (page == nullptr || used + slot_size > page_size)
        {
            page = static_cast<std::uint8_t *>(
                ::VirtualAlloc(nullptr, page_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            used = 0u;
            if (page == nullptr)
            {
                return nullptr;
            }
        }

        auto *slot = page + used;
        used += slot_size;
        return slot;
    }

    const Target *target_{nullptr};
    std::uint8_t *site_{nullptr};
    std::uint8_t *trampoline_{nullptr};
    std::array<std::uint8_t, prologue_bytes> original_{};
    Status status_{Status::not_installed};
};

inline auto image_base_delta() -> std::intptr_t
{
    static const auto delta = []
    {
        const auto module = ::GetModuleHandleA(nullptr);
        return static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(module) - preferred_image_base);
    }();
    return delta;
}

}
