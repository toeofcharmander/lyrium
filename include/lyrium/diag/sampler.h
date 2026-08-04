#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>

#include "lyrium/dao/engine_hooks.h"
#include "lyrium/never_destroyed.h"
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
        // Never destroyed, for the same reason as LogSink. See
        // never_destroyed.h.
        static auto sampler = NeverDestroyed<Sampler>{};
        return sampler.get();
    }

    auto start(std::int64_t interval_ms) -> void
    {
        auto lock = std::unique_lock{mutex_};
        if (running_)
        {
            return;
        }
        running_ = true;
        lock.unlock();

        // The interval is captured by value rather than read from a member, so
        // the loop shares no mutable state with anyone.
        thread_ = std::thread{
            [this, interval_ms]
            {
                register_own_thread();
                run(interval_ms);
            }};
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
        last_largest_free_below_2g_.store(stats.largest_free_below_2g, std::memory_order_relaxed);
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

    // The largest contiguous block below the 2 GB line. On a large-address-aware
    // process largest_free() stays pinned at roughly 2 GB because the space
    // above that line is untouched, which makes it useless as a pressure signal;
    // this is the number that actually moves as the heap fragments.
    auto largest_free_below_2g() const -> std::uint64_t
    {
        return last_largest_free_below_2g_.load(std::memory_order_relaxed);
    }

    auto total_free() const -> std::uint64_t
    {
        return last_total_free_.load(std::memory_order_relaxed);
    }

  private:
    Sampler() = default;

    template <class>
    friend class lyrium::NeverDestroyed;

    // Runs until the process ends. Deliberately takes no lock: a thread killed
    // by ExitProcess while holding one holds it forever, and anything that later
    // waits on it deadlocks inside the loader. The interval is read once, in
    // start(), so the loop touches no shared mutable state at all.
    //
    // The trailing sample_now("shutdown") that used to follow this loop was
    // unreachable -- nothing ever called stop() -- which is exactly why no live
    // log has ever contained a shutdown sample. It now happens at detach.
    auto run(std::int64_t interval_ms) -> void
    {
        sample_now("startup");

        while (true)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{interval_ms});
            sample_now("periodic");
        }
    }

    std::mutex mutex_;
    std::thread thread_;
    std::atomic<std::uint64_t> last_largest_free_{};
    std::atomic<std::uint64_t> last_largest_free_below_2g_{};
    std::atomic<std::uint64_t> last_total_free_{};
    std::atomic<Observer> observer_{nullptr};
    // Start-once latch only; nothing ever clears it.
    bool running_{false};
};

}
