#pragma once

#include <cstdint>

namespace lyrium::policy
{

// Decides when to ask the engine to evict textures, and how hard.
//
// Two things this replaces, both of which the first instrumented session showed
// to be wrong rather than merely inelegant.
//
// It used to evict without bound: reclaim() called emergency_evict(INT_MAX), so
// every rescue emptied the engine's entire texture cache and then evicted all
// managed resources on top. Everything afterwards had to be re-read from disk
// and re-decoded, which is what makes cutscenes and area transitions stutter.
//
// It also triggered on a fixed 16 MB of free space, while the game was observed
// creating a single 18.90 MB texture. With 17 MB free, no rescue fired and that
// allocation failed anyway: the watermark sat below the largest thing it was
// supposed to protect. The trigger here scales with the request instead, because
// what actually matters is whether one contiguous block is big enough for the
// allocation about to be made.

enum class EvictAction : std::uint8_t
{
    // Ordered by strength. escalation_strength() depends on this order.
    none,
    evict_cache,
    evict_cache_and_managed,
    clear_cache,
    // The failure path's terminal rung: everything clear_cache does, plus
    // EvictManagedResources. Distinct because the preemptive ladder must never
    // touch managed resources -- that frees video memory while the starving
    // resource is address space, and stalls the scene -- but a create that has
    // already failed may genuinely be short of VRAM.
    clear_cache_and_managed,
};

[[nodiscard]] constexpr auto escalation_strength(EvictAction action) -> std::uint8_t
{
    return static_cast<std::uint8_t>(action);
}

struct RescueConfig
{
    bool preemptive{true};
    bool on_failure{true};

    // A rescue fires before a create when the largest contiguous free block is
    // not comfortably larger than the request. Expressed as a multiple of the
    // request rather than a constant, so it stays correct on both a 2 GB and a
    // 4 GB process and cannot end up below the largest texture the game makes.
    std::uint32_t headroom_multiple{2u};

    // A floor for small requests, so a stream of 64 byte textures does not keep
    // rescue permanently disarmed while the address space collapses. Set above
    // the largest single create observed (18.90 MB).
    std::uint64_t headroom_floor_bytes{32ull * 1024ull * 1024ull};

    // Requests below this are never worth a preemptive rescue on their own.
    std::uint64_t large_create_bytes{1ull * 1024ull * 1024ull};

    // Hysteresis: once under pressure, stay there until there is this much more
    // than the entry condition, so the policy cannot oscillate every frame.
    std::uint32_t pressure_exit_multiple{4u};

    // How many preemptive rescues may run in one pressure episode before the
    // action strengthens, and before it becomes a full clear. Escalation used to
    // require attempt > 0, meaning a texture create had to fail first. A live
    // session died at 31 MB of headroom with all 2596 of its creates succeeding,
    // so that signal never arrived and the policy trimmed once and stopped.
    // Pressure that survives repeated trims is itself the evidence that trimming
    // is not reclaiming enough.
    std::uint32_t preemptive_escalate_after{2u};

    // Bounded eviction. The preemptive path takes a batch; the failure path
    // escalates from here and is never bounded away from acting.
    std::int32_t evict_batch{32};
    std::int32_t evict_batch_max{512};

    // Rate limit for the preemptive path only. Never applied after a failure.
    std::int64_t min_interval_us{50'000};

    // Also evict managed resources once escalation reaches that level.
    bool evict_managed{true};

    // Escape hatch. Restores the old unbounded behaviour without a rebuild, for
    // a first release where the failure mode is a crash in a long save.
    bool unbounded{false};
};

// Why a request cannot be served, which is not the same question as whether
// there is pressure.
//
// The pressure test compares the largest block against a threshold, which is a
// proxy for "about to fail". This is the direct form: a request that exceeds the
// largest block but not the total would fit if the free space were in one piece,
// and that is fragmentation. One that exceeds the total would not fit however
// the space were arranged, and that is exhaustion. A live session measured
// 101.6 MB free below the 2 GB line with a largest block of 9.2 MB across 339
// regions, which is the first condition in its purest form.
//
// The distinction decides whether eviction is worth attempting. Releasing a
// texture adjacent to an existing hole enlarges that hole, so fragmentation is
// worth a try. Under genuine exhaustion eviction reclaims real bytes too, but
// nothing can be gained by rearrangement alone, and a caller may reasonably want
// to know which it is looking at.
enum class FreeSpaceShape
{
    // Either figure unknown. Deliberately not folded into exhausted: reading an
    // unmeasured space as empty would report exhaustion on every call before the
    // sampler's first walk.
    unknown,
    sufficient,
    fragmented,
    exhausted,
};

// needed_bytes is the size the space is being judged against, which is not
// necessarily the request in hand: see RescueStats::largest_request_bytes for
// why the caller passes the largest request of the session instead.
[[nodiscard]] constexpr auto diagnose(
    std::uint64_t needed_bytes,
    std::uint64_t largest_free_bytes,
    std::uint64_t total_free_bytes) -> FreeSpaceShape
{
    if (largest_free_bytes == 0u || total_free_bytes == 0u)
    {
        return FreeSpaceShape::unknown;
    }
    if (needed_bytes <= largest_free_bytes)
    {
        return FreeSpaceShape::sufficient;
    }
    return needed_bytes <= total_free_bytes ? FreeSpaceShape::fragmented : FreeSpaceShape::exhausted;
}

[[nodiscard]] constexpr auto name_of(FreeSpaceShape shape) -> const char *
{
    switch (shape)
    {
        case FreeSpaceShape::sufficient: return "sufficient";
        case FreeSpaceShape::fragmented: return "fragmented";
        case FreeSpaceShape::exhausted: return "exhausted";
        case FreeSpaceShape::unknown: break;
    }
    return "unknown";
}

struct RescueInputs
{
    // Largest contiguous free block. Zero means UNKNOWN, not "plenty": the old
    // code treated zero as fine and so disabled rescue entirely until the
    // sampler's first tick.
    std::uint64_t largest_free_bytes{};

    // Free bytes in the space the request must come from, however scattered.
    // Zero means unknown. Diagnostic only: it decides which shape diagnose()
    // reports, and no eviction decision reads it, so a stale or absent reading
    // cannot change what the policy does.
    std::uint64_t total_free_bytes{};

    std::uint64_t requested_bytes{};

    std::int64_t now_us{};
    std::int64_t last_rescue_us{};

    // What the engine's cache reports. A pending queue with nothing in it means
    // there is nothing for a bounded evict to reclaim.
    std::int32_t pending_releases{};
    bool cache_available{};

    // 0 for a preemptive check, 1 and up for successive retries after a create
    // has already failed.
    std::uint32_t attempt{};

    // Whether the coordinator currently considers the process under pressure.
    bool under_pressure{};

    // Preemptive rescues already run in the current pressure episode. Reset when
    // pressure is relieved, so a fresh episode starts at the gentlest rung.
    std::uint32_t consecutive_preemptive{};
};

struct RescuePlan
{
    EvictAction action{EvictAction::none};
    std::int32_t max_count{};
    // Also surrender the DLL's own scratch pools (staging textures and the
    // recycler). Orthogonal to the action because it reclaims from us, not from
    // the engine: it is the cheapest address space there is to give back.
    bool flush_scratch{};
    bool enters_pressure{};
    bool leaves_pressure{};
    const char *reason{"idle"};

    [[nodiscard]] constexpr auto acts() const -> bool
    {
        return action != EvictAction::none;
    }
};

// Pure and stateless. All the state lives in the coordinator that executes these
// plans, which is what lets the whole decision table be tested exhaustively
// without fakes.
class RescuePolicy
{
  public:
    explicit constexpr RescuePolicy(const RescueConfig &config)
        : config_{config}
    {
    }

    [[nodiscard]] constexpr auto plan(const RescueInputs &inputs) const -> RescuePlan
    {
        // A create has already failed. This path must always act, and must
        // escalate, or bounding the preemptive path could quietly disable the
        // safety net that stops the game running out of address space.
        if (inputs.attempt > 0u)
        {
            return escalate(inputs);
        }

        if (!config_.preemptive)
        {
            return idle("preemptive rescue disabled");
        }

        if (!inputs.cache_available)
        {
            return idle("engine cache not discovered yet");
        }

        // The size gate applies only while the address space is healthy. It used
        // to apply unconditionally, and that is what let three sessions die:
        // "request too small to be worth a rescue" was the recorded reason on
        // nearly every call while the largest low block fell from 35 MB to
        // 16.5 MB, because the policy stopped thinking before it ever looked at
        // the address space. Small creates are precisely what drains it, so once
        // pressure is established, or headroom is already under the floor, every
        // request has to keep the ladder turning.
        const auto below_floor =
            inputs.largest_free_bytes != 0u && inputs.largest_free_bytes < config_.headroom_floor_bytes;
        if (!inputs.under_pressure && !below_floor && inputs.requested_bytes < config_.large_create_bytes)
        {
            return idle("request too small to be worth a rescue");
        }

        if (inputs.now_us - inputs.last_rescue_us < config_.min_interval_us)
        {
            return idle("rate limited");
        }

        const auto pressured = is_pressured(inputs);

        if (!pressured)
        {
            // Leaving pressure needs more headroom than entering it did.
            if (inputs.under_pressure && has_exit_headroom(inputs))
            {
                auto plan = idle("pressure relieved");
                plan.leaves_pressure = true;
                return plan;
            }
            return idle("sufficient headroom");
        }

        if (inputs.consecutive_preemptive >= config_.preemptive_escalate_after)
        {
            // Deliberately ahead of the pending_releases gate: with nothing
            // queued a bounded evict is a no-op, but the scratch pools reclaim
            // regardless, and that is exactly the state where trimming has
            // already failed to help.
            //
            // The preemptive path stops here. It does not clear the engine's
            // cache and it does not evict managed resources, for two separate
            // reasons that both come from live sessions.
            //
            // Managed eviction frees video memory while the starving resource is
            // the address space; two of them left 12 MB of headroom completely
            // unmoved.
            //
            // A full clear destroys hundreds of engine textures at once, and
            // every ~D3DResetable installs the abstract vtable -- _purecall in
            // the reset slot -- before unregistering, which is the game's own
            // race. Six sessions across both install types recorded zero failed
            // allocations, including a 2 GB run that reached 12 MB of headroom
            // and escalated to two clears and still died of that race. So the
            // clear has never been observed preventing anything, while being the
            // most disruptive thing available to the engine's object graph.
            //
            // Both remain on the failure path, where a create has actually
            // failed and there is nothing left to lose.
            return RescuePlan{
                .action = EvictAction::evict_cache,
                .max_count = batch_for(1u),
                .flush_scratch = true,
                .enters_pressure = true,
                .leaves_pressure = false,
                .reason = "preemptive: pressure sustained, trimming and flushing scratch pools",
            };
        }

        if (inputs.pending_releases <= 0)
        {
            // Nothing queued for the engine to trickle out. Evicting would be a
            // no-op that still costs a call, and the failure path will escalate
            // properly if the create actually fails.
            auto plan = idle("under pressure but the cache has nothing pending");
            plan.enters_pressure = true;
            return plan;
        }

        auto plan = RescuePlan{
            .action = EvictAction::evict_cache,
            .max_count = batch_for(0u),
            .enters_pressure = true,
            .leaves_pressure = false,
            .reason = "preemptive: headroom below the requested size",
        };
        return plan;
    }

  private:
    [[nodiscard]] static constexpr auto idle(const char *reason) -> RescuePlan
    {
        return RescuePlan{.reason = reason};
    }

    // Unknown headroom counts as pressure rather than as safety. Treating it as
    // safety is what silently disabled rescue before the sampler's first tick.
    [[nodiscard]] constexpr auto is_pressured(const RescueInputs &inputs) const -> bool
    {
        if (inputs.largest_free_bytes == 0u)
        {
            return true;
        }

        const auto wanted = inputs.requested_bytes * config_.headroom_multiple;
        return inputs.largest_free_bytes < wanted || inputs.largest_free_bytes < config_.headroom_floor_bytes;
    }

    [[nodiscard]] constexpr auto has_exit_headroom(const RescueInputs &inputs) const -> bool
    {
        if (inputs.largest_free_bytes == 0u)
        {
            return false;
        }

        const auto wanted = inputs.requested_bytes * config_.headroom_multiple * config_.pressure_exit_multiple;
        const auto floor = config_.headroom_floor_bytes * config_.pressure_exit_multiple;
        return inputs.largest_free_bytes >= wanted && inputs.largest_free_bytes >= floor;
    }

    [[nodiscard]] constexpr auto batch_for(std::uint32_t attempt) const -> std::int32_t
    {
        if (config_.unbounded)
        {
            return config_.evict_batch_max;
        }

        auto batch = config_.evict_batch;
        for (std::uint32_t step = 0u; step < attempt; ++step)
        {
            batch = batch > config_.evict_batch_max / 4 ? config_.evict_batch_max : batch * 4;
        }
        return batch > config_.evict_batch_max ? config_.evict_batch_max : batch;
    }

    // Monotonically stronger with each failed retry, terminating in a full cache
    // clear. clear_cache is never reached by the current code at all, so the new
    // worst case is strictly more aggressive than the unbounded evict it
    // replaces, not less.
    [[nodiscard]] constexpr auto escalate(const RescueInputs &inputs) const -> RescuePlan
    {
        if (!config_.on_failure)
        {
            return idle("failure rescue disabled");
        }

        if (inputs.attempt == 1u)
        {
            return RescuePlan{
                .action = EvictAction::evict_cache,
                .max_count = batch_for(1u),
                .enters_pressure = true,
                .leaves_pressure = false,
                .reason = "create failed: bounded cache eviction",
            };
        }

        if (inputs.attempt == 2u)
        {
            return RescuePlan{
                .action = config_.evict_managed ? EvictAction::evict_cache_and_managed : EvictAction::evict_cache,
                .max_count = batch_for(2u),
                .enters_pressure = true,
                .leaves_pressure = false,
                .reason = "create failed again: wider eviction including managed resources",
            };
        }

        return RescuePlan{
            .action = EvictAction::clear_cache_and_managed,
            .max_count = config_.evict_batch_max,
            .enters_pressure = true,
            .leaves_pressure = false,
            .reason = "create still failing: clearing the whole texture cache",
        };
    }

    RescueConfig config_;
};

}
