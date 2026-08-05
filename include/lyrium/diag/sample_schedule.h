#pragma once

#include <atomic>
#include <cstdint>

namespace lyrium::diag
{

// When the sampler thread should walk the address space.
//
// Portable and free of any Windows type on purpose. sampler.h reaches
// psapi.h through va_space.h and cannot be compiled by the test suite, so
// anything decided in there is untestable by construction -- the same rule that
// moved the free-block size classes out of the walk and into
// free_size_classes.h after a duplicated threshold array shipped a broken
// histogram.
//
// What this exists to fix: a failed texture create used to call
// Sampler::sample_now() inline, which walks. A 2 GB session logged 240
// va[create_failed] reports at about 7 ms each -- roughly 1.7 seconds of walking
// on the render thread, during the cascade it was measuring, on the one path
// CLAUDE.md says the walk must never run. The walk now happens on the sampler
// thread, and a burst of failures collapses into one.
enum class SampleAction : std::uint8_t
{
    wait,
    // Somebody asked. Carries a reason worth naming in the log, which is why it
    // outranks a periodic falling due on the same tick.
    requested,
    periodic,
};

class SampleSchedule
{
  public:
    // poll_ms is how often tick() is called, and so the granularity of a
    // request. It is deliberately much shorter than interval_ms: the cost of a
    // tick is one relaxed exchange, and the delay between a failed create and
    // the walk that describes it should be a few frames rather than seconds.
    SampleSchedule(std::int64_t interval_ms, std::int64_t poll_ms)
        : interval_ms_{interval_ms}
        , poll_ms_{poll_ms}
    {
    }

    // The interval comes from lyrium.ini and so is not known when the Sampler
    // singleton is constructed. Called once, from start(), before the sampler
    // thread exists -- hence plain assignment rather than an atomic.
    auto set_interval_ms(std::int64_t interval_ms) -> void
    {
        interval_ms_ = interval_ms;
    }

    // Callable from any thread, as often as the caller likes. This is the render
    // thread's entire share of the work, and repeated calls before the next tick
    // cost one store each and produce one walk.
    auto request() -> void
    {
        pending_.store(true, std::memory_order_relaxed);
    }

    // One poll interval has passed.
    [[nodiscard]] auto tick() -> SampleAction
    {
        elapsed_ms_ += poll_ms_;

        if (pending_.exchange(false, std::memory_order_relaxed))
        {
            // Restarts the interval, so a cascade that requests every tick does
            // not also fire a periodic on top of each one.
            elapsed_ms_ = 0;
            return SampleAction::requested;
        }

        if (elapsed_ms_ >= interval_ms_)
        {
            elapsed_ms_ = 0;
            return SampleAction::periodic;
        }

        return SampleAction::wait;
    }

  private:
    std::int64_t interval_ms_;
    std::int64_t poll_ms_;
    std::int64_t elapsed_ms_{};
    std::atomic<bool> pending_{false};
};

}
