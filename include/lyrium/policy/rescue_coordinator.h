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

    // Frees the DLL's own scratch pools and returns the bytes surrendered.
    [[nodiscard]] virtual auto release_scratch() -> std::uint64_t = 0;
};

class FreeSpaceProbe
{
  public:
    virtual ~FreeSpaceProbe() = default;

    // The constrained space specifically -- largest block and total -- or 0 when
    // not known. Where there is room above the 2 GB line these describe the low
    // half; where there is not they are the same figures as largest_free_bytes.
    //
    // Deliberately separate from largest_free_bytes, which answers "will this
    // allocation be served" and on a large-address-aware process therefore
    // includes 2 GB of untouched reserve. Diagnosing fragmentation against that
    // reports sufficient forever: a live session read sufficient on all 28
    // samples while the low half sat at 15.8 MB largest against 81.1 MB total.
    // The rescue decision wants the space that will be used; the diagnosis wants
    // the space that is degrading. Not the same question.
    [[nodiscard]] virtual auto constrained_largest_free_bytes() const -> std::uint64_t = 0;
    [[nodiscard]] virtual auto constrained_total_free_bytes() const -> std::uint64_t = 0;

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
    std::uint64_t scratch_flushes{};
    std::uint64_t scratch_bytes_released{};
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

    // What the free space looked like at the last decision, measured against the
    // largest request the session has seen rather than against the current one.
    //
    // Measuring against the current request made this useless: nearly every
    // create is a small texture that fits in any hole, so a live session
    // reported "sufficient" on all 28 samples while the largest free block was
    // 16.2 MB against 64.9 MB total. The question worth answering is whether the
    // allocation that fails first would still fit, and that is the largest one.
    //
    // Records the diagnosis rather than acting on it: the policy's behaviour is
    // unchanged and nothing reads this but the log.
    FreeSpaceShape last_shape{FreeSpaceShape::unknown};
    std::uint64_t largest_request_bytes{};

    // The two figures the verdict was reached from, carried alongside it.
    //
    // Without them the verdict invites a false comparison. It is computed when a
    // texture is created, from the sampler's cached figures, while the va[] line
    // beside it in the log comes from the walk that has since run -- so it trails
    // by one sample and reads as though it contradicts its own neighbour. Every
    // apparent mismatch in a live session turned out to be the previous sample's
    // number, which took a careful read to establish and would have taken one
    // again.
    std::uint64_t last_shape_largest_bytes{};
    std::uint64_t last_shape_total_bytes{};
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
            .total_free_bytes = probe_->constrained_total_free_bytes(),
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
        if (requested_bytes > stats_.largest_request_bytes)
        {
            stats_.largest_request_bytes = requested_bytes;
        }
        stats_.last_shape_largest_bytes = probe_->constrained_largest_free_bytes();
        stats_.last_shape_total_bytes = inputs.total_free_bytes;
        stats_.last_shape =
            diagnose(stats_.largest_request_bytes, stats_.last_shape_largest_bytes, stats_.last_shape_total_bytes);
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

        if (plan.flush_scratch)
        {
            ++stats_.scratch_flushes;
            stats_.scratch_bytes_released += backend_->release_scratch();
        }

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
                break;

            case EvictAction::clear_cache_and_managed:
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
