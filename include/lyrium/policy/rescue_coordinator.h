#pragma once

#include <cstdint>

#include "lyrium/policy/rescue_policy.h"

namespace lyrium::policy
{

// The seams the coordinator acts through. Abstract so the whole execution path
// can be driven by fakes on Linux; the D3D and engine adapters live on the
// Windows side and contain no decisions of their own.

class EvictionBackend
{
  public:
    virtual ~EvictionBackend() = default;

    [[nodiscard]] virtual auto cache_available() const -> bool = 0;
    [[nodiscard]] virtual auto pending_releases() const -> std::int32_t = 0;

    // Returns how many the engine actually released, or 0 if unknowable.
    [[nodiscard]] virtual auto evict(std::int32_t max_count) -> std::int32_t = 0;
    [[nodiscard]] virtual auto clear_cache() -> bool = 0;
    virtual auto evict_managed_resources() -> void = 0;
};

class FreeSpaceProbe
{
  public:
    virtual ~FreeSpaceProbe() = default;

    // Largest contiguous free block, or 0 when not yet known. Must be cheap:
    // a full address-space walk was measured at about 6.5 ms, which is 39
    // percent of a frame at 60 fps and cannot run on the create path.
    [[nodiscard]] virtual auto largest_free_bytes() const -> std::uint64_t = 0;
};

class Clock
{
  public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual auto now_us() const -> std::int64_t = 0;
};

struct RescueStats
{
    std::uint64_t preemptive{};
    std::uint64_t on_failure{};
    std::uint64_t evictions{};
    std::uint64_t cache_clears{};
    std::uint64_t managed_evictions{};
    std::uint64_t released_total{};
    std::uint64_t suppressed{};
    bool under_pressure{};

    // The inputs and verdict of the most recent decision, so a log line can say
    // why nothing happened rather than only that nothing happened. A live
    // session sat below the headroom floor without the pressure latch ever
    // setting, and the log recorded outcomes only, so which gate stopped it was
    // unknowable after the fact. Both are scalars: RescueCoordinator must stay
    // trivially destructible, and last_reason only ever points at a string
    // literal with static storage duration.
    std::uint64_t last_largest_free_bytes{};
    const char *last_reason{""};

    // The reason of the last decision that actually acted. Kept apart from
    // last_reason because the call right after a rescue is nearly always a small
    // create idling out, which overwrites it before anything can read it.
    const char *last_action_reason{""};
};

// Holds the state a rescue decision needs and executes the resulting plan.
//
// Split from RescuePolicy deliberately: the policy is pure and gets an
// exhaustive decision table test, while this gets a handful of interaction
// tests. Keeping the pressure latch, the clock reading and the reentrancy guard
// out of the decision is what makes that decision testable at all.
class RescueCoordinator
{
  public:
    RescueCoordinator(const RescueConfig &config, EvictionBackend &backend, FreeSpaceProbe &probe, const Clock &clock)
        : policy_{config}
        , backend_{&backend}
        , probe_{&probe}
        , clock_{&clock} // Seeded far enough back that the first rescue is never swallowed by the
                         // rate limit. The policy has no way to distinguish "never rescued" from
                         // "rescued just now", so the caller must establish it.
        , last_rescue_us_{clock.now_us() - config.min_interval_us - 1}
    {
    }

    RescueCoordinator(const RescueCoordinator &) = delete;
    auto operator=(const RescueCoordinator &) -> RescueCoordinator & = delete;

    struct Outcome
    {
        bool acted{};
        std::int32_t released{};
        const char *reason{"idle"};
    };

    // attempt 0 is the check before a create; 1 and up are retries after one has
    // already failed.
    auto consider(std::uint64_t requested_bytes, std::uint32_t attempt) -> Outcome
    {
        // Evicting runs engine code that can itself create textures, so a rescue
        // must not re-enter. Per-thread rather than global: two render threads
        // are independent, and a shared flag would let one suppress the other.
        static thread_local auto in_rescue = false;
        if (in_rescue)
        {
            ++stats_.suppressed;
            return Outcome{.reason = "already inside a rescue on this thread"};
        }

        const auto inputs = RescueInputs{
            .largest_free_bytes = probe_->largest_free_bytes(),
            .requested_bytes = requested_bytes,
            .now_us = clock_->now_us(),
            .last_rescue_us = last_rescue_us_,
            .pending_releases = backend_->pending_releases(),
            .cache_available = backend_->cache_available(),
            .attempt = attempt,
            .under_pressure = under_pressure_,
            .consecutive_preemptive = consecutive_preemptive_,
        };

        const auto plan = policy_.plan(inputs);

        if (plan.enters_pressure)
        {
            under_pressure_ = true;
            if (attempt == 0u)
            {
                // Counts pressured decisions, not acted rescues. A live session
                // froze one rung below managed eviction because its first rescue
                // emptied the pending queue and "nothing pending" never acts --
                // pressure the policy keeps seeing must climb regardless.
                // Rate-limited calls carry no pressure verdict, so pacing between
                // rungs is preserved.
                ++consecutive_preemptive_;
            }
        }
        if (plan.leaves_pressure)
        {
            under_pressure_ = false;
            consecutive_preemptive_ = 0u;
        }
        stats_.under_pressure = under_pressure_;
        stats_.last_largest_free_bytes = inputs.largest_free_bytes;
        stats_.last_reason = plan.reason;

        if (!plan.acts())
        {
            return Outcome{.reason = plan.reason};
        }

        in_rescue = true;
        const auto released = execute(plan);
        in_rescue = false;

        last_rescue_us_ = inputs.now_us;
        stats_.last_action_reason = plan.reason;

        if (attempt == 0u)
        {
            ++stats_.preemptive;
        }
        else
        {
            ++stats_.on_failure;
        }

        return Outcome{.acted = true, .released = released, .reason = plan.reason};
    }

    [[nodiscard]] auto stats() const -> RescueStats
    {
        return stats_;
    }

  private:
    auto execute(const RescuePlan &plan) -> std::int32_t
    {
        auto released = std::int32_t{};

        switch (plan.action)
        {
            case EvictAction::none: break;

            case EvictAction::evict_cache:
                released = backend_->evict(plan.max_count);
                ++stats_.evictions;
                break;

            case EvictAction::evict_cache_and_managed:
                released = backend_->evict(plan.max_count);
                ++stats_.evictions;
                backend_->evict_managed_resources();
                ++stats_.managed_evictions;
                break;

            case EvictAction::clear_cache:
                if (backend_->clear_cache())
                {
                    ++stats_.cache_clears;
                }
                backend_->evict_managed_resources();
                ++stats_.managed_evictions;
                break;
        }

        if (released > 0)
        {
            stats_.released_total += static_cast<std::uint64_t>(released);
        }
        return released;
    }

    RescuePolicy policy_;
    EvictionBackend *backend_;
    FreeSpaceProbe *probe_;
    const Clock *clock_;

    std::int64_t last_rescue_us_{};
    bool under_pressure_{};
    std::uint32_t consecutive_preemptive_{};
    RescueStats stats_{};
};

}
