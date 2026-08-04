#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>

#include "lyrium/dao/engine_hooks.h"
#include "lyrium/diag/texture_totals.h"
#include "lyrium/diag/va_space.h"
#include "lyrium/log.h"
#include "lyrium/texture_recycler.h"
#include "lyrium/texture_stager.h"

namespace lyrium::diag
{

class Sampler
{
  public:
    static auto instance() -> Sampler &
    {
        static auto sampler = Sampler{};
        return sampler;
    }

    auto start(std::int64_t interval_ms) -> void
    {
        auto lock = std::unique_lock{mutex_};
        if (running_)
        {
            return;
        }
        running_ = true;
        interval_ms_ = interval_ms;
        lock.unlock();

        thread_ = std::thread{
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
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    auto sample_now([[maybe_unused]] std::string_view reason) -> void
    {
        const auto stats = sample_va();

        last_largest_free_.store(stats.largest_free, std::memory_order_relaxed);
        last_total_free_.store(stats.total_free, std::memory_order_relaxed);
    }

    auto largest_free() const -> std::uint64_t
    {
        return last_largest_free_.load(std::memory_order_relaxed);
    }

    auto total_free() const -> std::uint64_t
    {
        return last_total_free_.load(std::memory_order_relaxed);
    }

  private:
    Sampler() = default;

    ~Sampler()
    {
        stop();
    }

    auto run() -> void
    {
        sample_now("startup");

        while (true)
        {
            auto lock = std::unique_lock{mutex_};
            signal_.wait_for(lock, std::chrono::milliseconds{interval_ms_}, [this] { return !running_; });
            if (!running_)
            {
                break;
            }
            lock.unlock();

            sample_now("periodic");

            if (++ticks_ % 6 == 0)
            {
            }
        }

        sample_now("shutdown");
    }

    std::mutex mutex_;
    std::condition_variable signal_;
    std::thread thread_;
    std::atomic<std::uint64_t> last_largest_free_{};
    std::atomic<std::uint64_t> last_total_free_{};
    std::int64_t interval_ms_{5000};
    std::uint64_t ticks_{0};
    bool running_{false};
};

}
