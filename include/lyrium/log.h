#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <windows.h>

namespace lyrium
{

auto register_own_thread() -> void;

namespace detail
{

inline std::atomic<bool> logging_enabled{true};
}

inline auto log_set_enabled(bool enabled) -> void
{
    detail::logging_enabled.store(enabled, std::memory_order_relaxed);
}

inline auto log_is_enabled() -> bool
{
    return detail::logging_enabled.load(std::memory_order_relaxed);
}

namespace detail
{

class LogSink
{
  public:
    static auto instance() -> LogSink &
    {
        static auto sink = LogSink{};
        return sink;
    }

    auto start(const std::filesystem::path &directory, std::string_view stem) -> void
    {
        auto lock = std::unique_lock{mutex_};
        if (running_)
        {
            return;
        }

        auto error = std::error_code{};
        std::filesystem::create_directories(directory, error);

        events_.open(directory / (std::string{stem} + ".jsonl"), std::ios::out | std::ios::trunc);
        text_.open(directory / (std::string{stem} + ".log"), std::ios::out | std::ios::trunc);
        directory_ = directory;
        running_ = true;

        lock.unlock();
        writer_ = std::thread{
            [this]
            {
                register_own_thread();
                run();
            }};
    }

    auto stop() -> void
    {
        {
            auto lock = std::scoped_lock{mutex_};
            if (!running_)
            {
                return;
            }
            running_ = false;
        }
        signal_.notify_all();
        if (writer_.joinable())
        {
            writer_.join();
        }

        auto lock = std::scoped_lock{mutex_};
        drain_locked();
        events_.flush();
        text_.flush();
        events_.close();
        text_.close();
    }

    auto push_event(std::string &&line) -> void
    {
        push(std::move(line), true);
    }

    auto push_text(std::string &&line) -> void
    {
        {
            auto lock = std::scoped_lock{tail_mutex_};
            if (tail_.size() >= tail_limit)
            {
                tail_.erase(tail_.begin());
            }
            tail_.push_back(line);
        }
        push(std::move(line), false);
    }

    auto tail() const -> std::string
    {
        auto lock = std::scoped_lock{tail_mutex_};
        auto out = std::string{};
        for (const auto &line : tail_)
        {
            out += line;
            out.push_back('\n');
        }
        return out;
    }

    auto dropped() const -> std::uint64_t
    {
        return dropped_.load(std::memory_order_relaxed);
    }

    auto directory() const -> std::filesystem::path
    {
        auto lock = std::scoped_lock{mutex_};
        return directory_;
    }

  private:
    struct Line
    {
        std::string text;
        bool is_event;
    };

    LogSink() = default;

    ~LogSink()
    {
        stop();
    }

    auto push(std::string &&line, bool is_event) -> void
    {
        if (!logging_enabled.load(std::memory_order_relaxed))
        {
            return;
        }

        {
            auto lock = std::scoped_lock{mutex_};
            if (pending_.size() >= queue_limit)
            {
                dropped_.fetch_add(1u, std::memory_order_relaxed);
                return;
            }
            pending_.push_back(Line{.text = std::move(line), .is_event = is_event});
        }
        signal_.notify_one();
    }

    auto run() -> void
    {
        auto keep_going = true;
        while (keep_going)
        {
            {
                auto lock = std::unique_lock{mutex_};
                signal_.wait_for(lock, std::chrono::milliseconds{250}, [this] { return !pending_.empty() || !running_; });
                keep_going = running_ || !pending_.empty();
                drain_locked();
            }

            events_.flush();
            text_.flush();
        }
    }

    auto drain_locked() -> void
    {
        if (pending_.empty())
        {
            return;
        }

        std::swap(pending_, draining_);
        for (const auto &line : draining_)
        {
            auto &stream = line.is_event ? events_ : text_;
            if (stream)
            {
                stream.write(line.text.data(), static_cast<std::streamsize>(line.text.size()));
                stream.put('\n');
            }
        }
        draining_.clear();
    }

    static constexpr auto queue_limit = std::size_t{16384};
    static constexpr auto tail_limit = std::size_t{256};

    mutable std::mutex mutex_;
    mutable std::mutex tail_mutex_;
    std::condition_variable signal_;
    std::thread writer_;
    std::vector<Line> pending_;
    std::vector<Line> draining_;
    std::vector<std::string> tail_;
    std::ofstream events_;
    std::ofstream text_;
    std::filesystem::path directory_;
    std::atomic<std::uint64_t> dropped_{};
    bool running_{false};
};

inline auto performance_frequency() -> std::int64_t
{
    static const auto frequency = []
    {
        auto value = ::LARGE_INTEGER{};
        ::QueryPerformanceFrequency(&value);
        return value.QuadPart != 0 ? value.QuadPart : std::int64_t{1};
    }();
    return frequency;
}

inline auto performance_origin() -> std::int64_t
{
    static const auto origin = []
    {
        auto value = ::LARGE_INTEGER{};
        ::QueryPerformanceCounter(&value);
        return value.QuadPart;
    }();
    return origin;
}

inline auto append_escaped(std::string &out, std::string_view value) -> void
{
    for (const auto c : value)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u)
                {
                    static constexpr auto digits = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(digits[(static_cast<unsigned char>(c) >> 4) & 0x0Fu]);
                    out.push_back(digits[static_cast<unsigned char>(c) & 0x0Fu]);
                }
                else
                {
                    out.push_back(c);
                }
                break;
        }
    }
}

}

inline auto now_us() -> std::int64_t
{
    auto value = ::LARGE_INTEGER{};
    ::QueryPerformanceCounter(&value);
    return ((value.QuadPart - detail::performance_origin()) * 1'000'000) / detail::performance_frequency();
}

inline auto wall_clock(const char *format = "%Y-%m-%dT%H:%M:%S") -> std::string
{
    auto local = ::SYSTEMTIME{};
    ::GetLocalTime(&local);

    auto tm_value = std::tm{};
    tm_value.tm_year = local.wYear - 1900;
    tm_value.tm_mon = local.wMonth - 1;
    tm_value.tm_mday = local.wDay;
    tm_value.tm_hour = local.wHour;
    tm_value.tm_min = local.wMinute;
    tm_value.tm_sec = local.wSecond;

    char buffer[64]{};
    std::strftime(buffer, sizeof(buffer), format, &tm_value);
    return std::string{buffer};
}

class Event
{
  public:
    explicit Event(std::string_view type)
        : buffer_{}
        , silent_{!log_is_enabled()}
    {

        if (silent_)
        {
            return;
        }

        buffer_.reserve(256u);
        buffer_ += R"({"t":)";
        buffer_ += std::to_string(now_us());
        buffer_ += R"(,"ev":")";
        detail::append_escaped(buffer_, type);
        buffer_ += '"';
    }

    auto add(std::string_view key, std::string_view value) -> Event &
    {
        if (silent_)
        {
            return *this;
        }
        key_prefix(key);
        buffer_ += '"';
        detail::append_escaped(buffer_, value);
        buffer_ += '"';
        return *this;
    }

    auto add(std::string_view key, const char *value) -> Event &
    {
        return add(key, std::string_view{value != nullptr ? value : ""});
    }

    auto add(std::string_view key, bool value) -> Event &
    {
        if (silent_)
        {
            return *this;
        }
        key_prefix(key);
        buffer_ += value ? "true" : "false";
        return *this;
    }

    template <class T>
        requires std::is_integral_v<T> && (!std::is_same_v<std::remove_cv_t<T>, bool>)
    auto add(std::string_view key, T value) -> Event &
    {
        if (silent_)
        {
            return *this;
        }
        key_prefix(key);
        if constexpr (std::is_signed_v<T>)
        {
            buffer_ += std::to_string(static_cast<std::int64_t>(value));
        }
        else
        {
            buffer_ += std::to_string(static_cast<std::uint64_t>(value));
        }
        return *this;
    }

    template <class T>
        requires std::is_floating_point_v<T>
    auto add(std::string_view key, T value) -> Event &
    {
        if (silent_)
        {
            return *this;
        }
        key_prefix(key);
        char buffer[40]{};
        std::snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(value));
        buffer_ += buffer;
        return *this;
    }

    auto add_hex(std::string_view key, std::uint64_t value) -> Event &
    {
        if (silent_)
        {
            return *this;
        }
        key_prefix(key);
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "\"0x%08llX\"", static_cast<unsigned long long>(value));
        buffer_ += buffer;
        return *this;
    }

    auto add_ptr(std::string_view key, const void *value) -> Event &
    {
        return add_hex(key, reinterpret_cast<std::uintptr_t>(value));
    }

    auto emit() -> void
    {
        if (silent_)
        {
            return;
        }
        buffer_ += '}';
        detail::LogSink::instance().push_event(std::move(buffer_));
    }

  private:
    auto key_prefix(std::string_view key) -> void
    {
        buffer_ += ",\"";
        detail::append_escaped(buffer_, key);
        buffer_ += "\":";
    }

    std::string buffer_;
    bool silent_;
};

namespace detail
{
inline auto breadcrumb_path() -> std::filesystem::path &
{
    static auto path = std::filesystem::path{};
    return path;
}

inline constexpr auto max_own_threads = std::size_t{16};
inline std::atomic<std::uint32_t> own_thread_ids[max_own_threads]{};
}

inline auto register_own_thread() -> void
{
    const auto id = static_cast<std::uint32_t>(::GetCurrentThreadId());
    for (auto &slot : detail::own_thread_ids)
    {
        auto expected = std::uint32_t{0};
        if (slot.compare_exchange_strong(expected, id, std::memory_order_relaxed))
        {
            return;
        }
    }
}

inline auto is_own_thread(std::uint32_t id) -> bool
{
    for (const auto &slot : detail::own_thread_ids)
    {
        if (slot.load(std::memory_order_relaxed) == id)
        {
            return true;
        }
    }
    return false;
}

inline auto breadcrumb(std::string_view step) -> void
{
    if (!log_is_enabled())
    {
        return;
    }

    auto &path = detail::breadcrumb_path();
    if (path.empty())
    {
        char temp[MAX_PATH]{};
        ::GetEnvironmentVariableA("TEMP", temp, MAX_PATH);
        path = std::filesystem::path{temp} / "lyrium_breadcrumbs.txt";
    }

    if (auto file = std::ofstream{path, std::ios::app}; file)
    {
        file << now_us() << "us tid=" << ::GetCurrentThreadId() << "  " << step << std::endl;
    }
}

inline auto breadcrumb_reset(const std::filesystem::path &directory) -> void
{
    if (!log_is_enabled())
    {
        return;
    }

    auto error = std::error_code{};
    std::filesystem::create_directories(directory, error);
    detail::breadcrumb_path() = directory / "lyrium_breadcrumbs.txt";

    if (auto file = std::ofstream{detail::breadcrumb_path(), std::ios::trunc}; file)
    {
        file << "lyrium startup breadcrumbs, " << ::GetCurrentProcessId() << std::endl;
    }
}

inline auto log_start(const std::filesystem::path &directory, std::string_view stem) -> void
{
    detail::LogSink::instance().start(directory, stem);
}

inline auto log_stop() -> void
{
    detail::LogSink::instance().stop();
}

inline auto log_tail() -> std::string
{
    return detail::LogSink::instance().tail();
}

inline auto log_dropped() -> std::uint64_t
{
    return detail::LogSink::instance().dropped();
}

inline auto log_directory() -> std::filesystem::path
{
    return detail::LogSink::instance().directory();
}

}
