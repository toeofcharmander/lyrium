#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>

#include "lyrium/dao/engine_hooks.h"
#include "lyrium/diag/va_space.h"
#include "lyrium/log.h"
#include "lyrium/utils.h"
#include "lyrium/texture_recycler.h"

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

    // Called after every sample so the DLL can add its own figures to the same
    // line. A plain function pointer rather than a std::function: this fires on
    // the sampler thread and must not allocate.
    using Observer = void (*)(std::string_view reason, const VaStats &stats);

    auto set_observer(Observer observer) -> void
    {
        observer_.store(observer, std::memory_order_relaxed);
    }

    auto sample_now(std::string_view reason) -> void
    {
        const auto stats = sample_va();

        last_largest_free_.store(stats.largest_free, std::memory_order_relaxed);
        last_total_free_.store(stats.total_free, std::memory_order_relaxed);

        // The address-space walk is the whole point of this project, and until
        // now its result reached nothing but two atomics and the overlay. Logging
        // it is what makes a session diagnosable after the fact instead of only
        // while someone is watching the screen. walk_us is included because the
        // cost of this walk on the render thread is otherwise unmeasured.
        lyrium::log(
            "va[{}]: largest_free={} below2g={} total_free={} free_regions={} committed_private={} "
            "available_virtual={} walk_us={}",
            reason,
            stats.largest_free,
            stats.largest_free_below_2g,
            stats.total_free,
            stats.free_regions,
            stats.committed_private,
            stats.available_virtual,
            stats.walk_us);

        if (const auto observer = observer_.load(std::memory_order_relaxed); observer != nullptr)
        {
            observer(reason, stats);
        }
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
        }

        sample_now("shutdown");
    }

    std::mutex mutex_;
    std::condition_variable signal_;
    std::thread thread_;
    std::atomic<std::uint64_t> last_largest_free_{};
    std::atomic<std::uint64_t> last_total_free_{};
    std::atomic<Observer> observer_{nullptr};
    std::int64_t interval_ms_{5000};
    bool running_{false};
};

}
