#pragma once

#include <filesystem>
#include <format>
#include <iostream>
#include <ostream>
#include <print>
#include <string>

#include <windows.h>

#include "lyrium/containers/string.h"
#include "lyrium/log.h"

namespace lyrium
{

inline auto get_temp(StringView filename) -> std::filesystem::path
{
    char tempPath[MAX_PATH]{};
    ::GetEnvironmentVariableA("TEMP", tempPath, MAX_PATH);
    return std::filesystem::path{tempPath} / filename;
}

inline auto executable_path() -> std::filesystem::path
{
    char buffer[MAX_PATH]{};
    const auto length = ::GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    return length != 0 ? std::filesystem::path{std::string{buffer, length}} : std::filesystem::path{};
}

inline auto executable_directory() -> std::filesystem::path
{
    const auto path = executable_path();
    return path.empty() ? std::filesystem::path{} : path.parent_path();
}

template <class... T>
auto log(std::format_string<T...> fmt, T &&...args) -> void
{

    if (!log_is_enabled())
    {
        return;
    }

    auto line = std::string{};
    std::format_to(std::back_inserter(line), fmt, std::forward<T>(args)...);
    detail::LogSink::instance().push_text(std::move(line));
}

template <class... T>
auto die(std::format_string<T...> fmt, T &&...args) -> void
{

    auto message = std::string{};
    std::format_to(std::back_inserter(message), fmt, std::forward<T>(args)...);

    std::println("{}", message);
    std::cout << std::flush;

    std::_Exit(1);
}

template <class... T>
auto ensure(bool cond, std::format_string<T...> fmt, T &&...args) -> void
{
    if (!cond)
    {
        die(fmt, std::forward<T>(args)...);
    }
}

inline auto safe_read_string(const void *address, std::size_t limit = 260u) -> std::string
{
    if (address == nullptr)
    {
        return {};
    }

    auto out = std::string{};
    const auto *cursor = static_cast<const char *>(address);

    while (out.size() < limit)
    {
        auto info = ::MEMORY_BASIC_INFORMATION{};
        if (::VirtualQuery(cursor, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
        {
            break;
        }

        static constexpr auto readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((info.Protect & readable) == 0 || (info.Protect & PAGE_GUARD) != 0)
        {
            break;
        }

        const auto *region_end =
            static_cast<const char *>(info.BaseAddress) + info.RegionSize;
        while (cursor < region_end && out.size() < limit)
        {
            const auto c = *cursor++;
            if (c == '\0')
            {
                return out;
            }
            out.push_back(c);
        }
    }

    return out;
}

}
