#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>

#include "lyrium/dao/engine_hooks.h"
#include "lyrium/diag/va_space.h"
#include "lyrium/log.h"
#include "lyrium/never_destroyed.h"
#include "lyrium/texture_recycler.h"
#include "lyrium/utils.h"

namespace lyrium::diag
{

// What the overlay needs from the last walk. Kept small and copied under a try
// lock so the render thread never blocks behind a sample in progress.
struct FreeSpaceSnapshot
{
    std::uint64_t largest_free{};
    std::uint64_t largest_free_below_2g{};
    std::uint64_t total_free{};
    std::uint32_t free_regions{};
    std::int64_t walk_us{};
    // Cumulative counts of free blocks at each size threshold, differenced by
    // the overlay through free_size_classes(). Both sides must read the same
    // thresholds in the same order; see free_size_classes.h.
    FreeBuckets size_buckets{};
};

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
        thread_ = std::thread{[this, interval_ms]
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

        {
            const auto lock = std::scoped_lock{snapshot_mutex_};
            snapshot_.largest_free = stats.largest_free;
            snapshot_.largest_free_below_2g = stats.largest_free_below_2g;
            snapshot_.total_free = stats.total_free;
            snapshot_.free_regions = stats.free_regions;
            snapshot_.walk_us = stats.walk_us;
            snapshot_.size_buckets = stats.buckets;
        }

        last_largest_free_.store(stats.largest_free, std::memory_order_relaxed);
        last_largest_free_below_2g_.store(stats.largest_free_below_2g, std::memory_order_relaxed);
        last_total_free_.store(stats.total_free, std::memory_order_relaxed);
        last_total_free_below_2g_.store(stats.total_free_below_2g, std::memory_order_relaxed);

        // The address-space walk is the whole point of this project, and until
        // now its result reached nothing but two atomics and the overlay. Logging
        // it is what makes a session diagnosable after the fact instead of only
        // while someone is watching the screen. walk_us is included because the
        // cost of this walk on the render thread is otherwise unmeasured.
        lyrium::log(
            "va[{}]: largest_free={} below2g={} low_total={} total_free={} free_regions={} "
            "committed_private={} available_virtual={} walk_us={}",
            reason,
            stats.largest_free,
            stats.largest_free_below_2g,
            stats.total_free_below_2g,
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

    // Declines rather than waits: this is called from the render thread every
    // frame, and dropping one frame's figures costs nothing while a stall costs
    // a visible hitch.
    //
    // Note the walk itself is not what this is guarding against. sample_now()
    // completes sample_va() before taking this lock, so the lock is held only for
    // the length of a struct copy -- which is why a walk with a measured worst
    // case of 205 ms cannot stall the render thread.
    [[nodiscard]] auto try_snapshot(FreeSpaceSnapshot &out) const -> bool
    {
        const auto lock = std::unique_lock{snapshot_mutex_, std::try_to_lock};
        if (!lock.owns_lock())
        {
            return false;
        }
        out = snapshot_;
        return true;
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

    // Free bytes below the 2 GB line, however scattered. Paired with
    // largest_free_below_2g this separates fragmentation from exhaustion.
    auto total_free_below_2g() const -> std::uint64_t
    {
        return last_total_free_below_2g_.load(std::memory_order_relaxed);
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
    mutable std::mutex snapshot_mutex_{};
    FreeSpaceSnapshot snapshot_{};

    std::atomic<std::uint64_t> last_largest_free_{};
    std::atomic<std::uint64_t> last_largest_free_below_2g_{};
    std::atomic<std::uint64_t> last_total_free_{};
    std::atomic<std::uint64_t> last_total_free_below_2g_{};
    std::atomic<Observer> observer_{nullptr};
    // Start-once latch only; nothing ever clears it.
    bool running_{false};
};

}
