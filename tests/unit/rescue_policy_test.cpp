#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/policy/rescue_policy.h"

using lyrium::policy::diagnose;
using lyrium::policy::escalation_strength;
using lyrium::policy::EvictAction;
using lyrium::policy::FreeSpaceShape;
using lyrium::policy::RescueConfig;
using lyrium::policy::RescueInputs;
using lyrium::policy::RescuePolicy;

namespace
{

constexpr auto mb = std::uint64_t{1024u * 1024u};

// The largest single texture the game was actually observed creating. The old
// 16 MB watermark sat below this, which is the defect this policy exists to fix.
constexpr auto observed_largest_texture = std::uint64_t{19814400};

constexpr auto healthy() -> RescueInputs
{
    return RescueInputs{
        .largest_free_bytes = 512u * mb,
        .requested_bytes = 4u * mb,
        .now_us = 10'000'000,
        .last_rescue_us = 0,
        .pending_releases = 100,
        .cache_available = true,
        .attempt = 0u,
        .under_pressure = false,
    };
}

constexpr auto policy() -> RescuePolicy
{
    return RescuePolicy{RescueConfig{}};
}

}

// ---------------------------------------------------------------------------
// The safety property. Bounding the preemptive path must never be able to
// disable the net that catches an actual allocation failure.
// ---------------------------------------------------------------------------

TEST(RescuePolicy, AnyFailedCreateAlwaysActs)
{
    // Swept across every input that could plausibly suppress a rescue.
    for (std::uint32_t attempt = 1u; attempt <= 6u; ++attempt)
    {
        for (const auto largest : {std::uint64_t{0}, std::uint64_t{1}, 512u * mb, 4096u * mb})
        {
            for (const auto pending : {-1, 0, 1, 1000})
            {
                for (const auto cache : {true, false})
                {
                    auto inputs = healthy();
                    inputs.attempt = attempt;
                    inputs.largest_free_bytes = largest;
                    inputs.pending_releases = pending;
                    inputs.cache_available = cache;
                    inputs.last_rescue_us = inputs.now_us; // rate limit maximally hostile

                    const auto plan = policy().plan(inputs);
                    EXPECT_TRUE(plan.acts()) << "attempt=" << attempt << " largest=" << largest
                                             << " pending=" << pending << " cache=" << cache << " produced no action";
                }
            }
        }
    }
}

TEST(RescuePolicy, EscalationIsMonotone)
{
    auto previous = escalation_strength(EvictAction::none);

    for (std::uint32_t attempt = 1u; attempt <= 3u; ++attempt)
    {
        auto inputs = healthy();
        inputs.attempt = attempt;

        const auto plan = policy().plan(inputs);
        const auto strength = escalation_strength(plan.action);

        EXPECT_GE(strength, previous) << "attempt " << attempt << " must not be weaker than the one before";
        previous = strength;
    }

    auto last = healthy();
    last.attempt = 3u;
    EXPECT_EQ(policy().plan(last).action, EvictAction::clear_cache_and_managed)
        << "escalation must terminate in a full clear";
}

TEST(RescuePolicy, TheWorstCaseIsStrongerThanTheUnboundedEvictItReplaces)
{
    // clear_cache is never reached by the code being replaced, which only ever
    // called emergency_evict(INT_MAX). Reaching it is what makes bounding safe.
    auto inputs = healthy();
    inputs.attempt = 4u;

    EXPECT_EQ(policy().plan(inputs).action, EvictAction::clear_cache_and_managed);
}

// ---------------------------------------------------------------------------
// The trigger. This is the defect the instrumented session exposed.
// ---------------------------------------------------------------------------

TEST(RescuePolicy, TriggersWhenHeadroomIsSmallerThanTheRequestEvenAboveTheOldWatermark)
{
    // The exact case the old policy missed: 17 MB free, which clears the old
    // 16 MB watermark, while the game asks for the 18.90 MB texture it really
    // does create. The old code fired no rescue and the allocation failed.
    auto inputs = healthy();
    inputs.requested_bytes = observed_largest_texture;
    inputs.largest_free_bytes = 17u * mb;

    const auto plan = policy().plan(inputs);
    EXPECT_TRUE(plan.acts()) << "headroom below the requested size must trigger regardless of any fixed watermark";
    EXPECT_TRUE(plan.enters_pressure);
}

TEST(RescuePolicy, DoesNotTriggerWhenHeadroomComfortablyExceedsTheRequest)
{
    auto inputs = healthy();
    inputs.requested_bytes = 4u * mb;
    inputs.largest_free_bytes = 512u * mb;

    EXPECT_FALSE(policy().plan(inputs).acts());
}

TEST(RescuePolicy, TheFloorCatchesASmallRequestInACollapsedAddressSpace)
{
    // A 2 MB request against 20 MB free clears the multiple-of-request test, but
    // the address space is nearly gone. Without a floor a stream of small
    // textures would keep rescue disarmed while things collapsed.
    auto inputs = healthy();
    inputs.requested_bytes = 2u * mb;
    inputs.largest_free_bytes = 20u * mb;

    EXPECT_TRUE(policy().plan(inputs).acts());
}

TEST(RescuePolicy, UnknownHeadroomCountsAsPressureNotAsSafety)
{
    // The live defect: largest_free of zero meant "fine", so rescue was silently
    // disabled until the sampler's first tick, several seconds into the session.
    auto inputs = healthy();
    inputs.largest_free_bytes = 0u;

    EXPECT_TRUE(policy().plan(inputs).acts()) << "an unknown reading must not be read as plenty of room";
}

TEST(RescuePolicy, SmallRequestsDoNotTriggerOnTheirOwnWhileHealthy)
{
    auto inputs = healthy();
    inputs.requested_bytes = 64u;
    inputs.largest_free_bytes = 512u * mb;

    EXPECT_FALSE(policy().plan(inputs).acts()) << "a 64 byte texture is not worth emptying the cache for";
}

// This case previously asserted the opposite, and that assertion was the bug.
// Requiring a large request before even looking at the address space is what let
// three live sessions fall from 35 MB to 16.5 MB of contiguous space with
// "request too small to be worth a rescue" recorded on nearly every call. At a
// megabyte of headroom the size of the request is beside the point.
TEST(RescuePolicy, ASmallRequestStillTriggersWhenHeadroomIsCritical)
{
    auto inputs = healthy();
    inputs.requested_bytes = 64u;
    inputs.largest_free_bytes = 1u * mb;

    EXPECT_TRUE(policy().plan(inputs).acts()) << "one megabyte of contiguous space is an emergency at any size";
}

// ---------------------------------------------------------------------------
// Bounding, rate limiting and hysteresis: the stutter fixes.
// ---------------------------------------------------------------------------

TEST(RescuePolicy, PreemptiveEvictionIsBounded)
{
    auto inputs = healthy();
    inputs.largest_free_bytes = 1u * mb;

    const auto plan = policy().plan(inputs);
    ASSERT_TRUE(plan.acts());
    EXPECT_EQ(plan.max_count, RescueConfig{}.evict_batch);
    EXPECT_LT(plan.max_count, RescueConfig{}.evict_batch_max)
        << "the preemptive path must not empty the cache in one go";
}

TEST(RescuePolicy, TheRateLimitAppliesToThePreemptivePathOnly)
{
    auto inputs = healthy();
    inputs.largest_free_bytes = 1u * mb;
    inputs.last_rescue_us = inputs.now_us;

    EXPECT_FALSE(policy().plan(inputs).acts()) << "back-to-back preemptive rescues are what cause the stutter";

    inputs.attempt = 1u;
    EXPECT_TRUE(policy().plan(inputs).acts()) << "a real failure must never be rate limited away";
}

TEST(RescuePolicy, LeavingPressureNeedsMoreHeadroomThanEnteringIt)
{
    const auto config = RescueConfig{};

    auto inputs = healthy();
    inputs.under_pressure = true;
    inputs.requested_bytes = 4u * mb;

    // Just above the entry threshold but well below the exit threshold: still
    // considered under pressure, so the policy cannot oscillate frame to frame.
    inputs.largest_free_bytes = config.headroom_floor_bytes + 1u;
    EXPECT_FALSE(policy().plan(inputs).leaves_pressure);

    inputs.largest_free_bytes = config.headroom_floor_bytes * config.pressure_exit_multiple + 1u;
    EXPECT_TRUE(policy().plan(inputs).leaves_pressure);
}

TEST(RescuePolicy, DoesNotEvictWhenTheCacheHasNothingPending)
{
    auto inputs = healthy();
    inputs.largest_free_bytes = 1u * mb;
    inputs.pending_releases = 0;

    const auto plan = policy().plan(inputs);
    EXPECT_FALSE(plan.acts()) << "evicting an empty queue costs a call and reclaims nothing";
    EXPECT_TRUE(plan.enters_pressure) << "but the pressure state must still be recorded";
}

TEST(RescuePolicy, WaitsForTheEngineCacheToBeDiscovered)
{
    auto inputs = healthy();
    inputs.largest_free_bytes = 1u * mb;
    inputs.cache_available = false;

    EXPECT_FALSE(policy().plan(inputs).acts());
}

// ---------------------------------------------------------------------------
// Configuration.
// ---------------------------------------------------------------------------

TEST(RescuePolicy, PreemptiveAndFailurePathsCanBeDisabledIndependently)
{
    auto inputs = healthy();
    inputs.largest_free_bytes = 1u * mb;

    const auto no_preemptive = RescuePolicy{RescueConfig{.preemptive = false}};
    EXPECT_FALSE(no_preemptive.plan(inputs).acts());

    inputs.attempt = 1u;
    EXPECT_TRUE(no_preemptive.plan(inputs).acts()) << "disabling the preemptive path must not disarm the net";

    const auto no_failure = RescuePolicy{RescueConfig{.on_failure = false}};
    EXPECT_FALSE(no_failure.plan(inputs).acts()) << "that one is opt-out on its own";
}

TEST(RescuePolicy, TheUnboundedEscapeHatchRestoresTheOldBehaviour)
{
    auto inputs = healthy();
    inputs.largest_free_bytes = 1u * mb;

    const auto unbounded = RescuePolicy{RescueConfig{.unbounded = true}};
    const auto plan = unbounded.plan(inputs);

    ASSERT_TRUE(plan.acts());
    EXPECT_EQ(plan.max_count, RescueConfig{}.evict_batch_max) << "one switch returns to emptying the cache";
}

TEST(RescuePolicy, EveryOutcomeExplainsItself)
{
    auto inputs = healthy();
    EXPECT_STREQ(policy().plan(inputs).reason, "sufficient headroom");

    inputs.requested_bytes = 64u;
    EXPECT_STREQ(policy().plan(inputs).reason, "request too small to be worth a rescue");

    inputs = healthy();
    inputs.cache_available = false;
    EXPECT_STREQ(policy().plan(inputs).reason, "engine cache not discovered yet");

    inputs = healthy();
    inputs.largest_free_bytes = 1u * mb;
    inputs.last_rescue_us = inputs.now_us;
    EXPECT_STREQ(policy().plan(inputs).reason, "rate limited");

    inputs = healthy();
    inputs.attempt = 3u;
    EXPECT_STREQ(policy().plan(inputs).reason, "create still failing: clearing the whole texture cache");
}

TEST(RescuePolicy, LastRescueMustBeSeededByTheCaller)
{
    // With now_us and last_rescue_us both zero the rate limit reads as "rescued
    // a moment ago" and suppresses the preemptive path. That is correct given
    // the inputs, and it is the coordinator's job to seed last_rescue_us far
    // enough in the past that the first rescue is not swallowed. Pinned so the
    // requirement on the caller is written down rather than discovered.
    auto at_origin = healthy();
    at_origin.largest_free_bytes = 1u * mb;
    at_origin.now_us = 0;
    at_origin.last_rescue_us = 0;

    const auto suppressed = policy().plan(at_origin);
    EXPECT_FALSE(suppressed.acts());
    EXPECT_STREQ(suppressed.reason, "rate limited");

    at_origin.last_rescue_us = -RescueConfig{}.min_interval_us;
    EXPECT_TRUE(policy().plan(at_origin).acts()) << "seeded properly, the first rescue fires";
}

// The decision is constexpr, so it costs nothing on the create path.
static_assert(!policy().plan(healthy()).acts());
static_assert(policy()
                  .plan(
                      RescueInputs{
                          .largest_free_bytes = 0u,
                          .requested_bytes = 4u * mb,
                          .now_us = 10'000'000,
                          .last_rescue_us = 0,
                          .pending_releases = 10,
                          .cache_available = true})
                  .acts());

// ---------------------------------------------------------------------------
// Diagnosing why a request cannot be served.
//
// The pressure test compares the largest block against a threshold, which is a
// proxy. The actual question is whether the request would fit in the free space
// if the free space were in one piece. A live session measured 101.6 MB free
// below the 2 GB line with the largest block at 9.2 MB across 339 regions: the
// bytes present, in fragments, unusable for anything large. That is a different
// condition from being genuinely out of memory, and eviction is worth trying for
// one and pointless for the other.
// ---------------------------------------------------------------------------

TEST(FreeSpaceShape, ScatteredSpaceThatWouldOtherwiseFitIsFragmentation)
{
    // 20 MB wanted, 9.2 MB largest, 101.6 MB total: it would fit if it were whole.
    EXPECT_EQ(diagnose(20u * mb, 9u * mb + 200u * 1024u, 101u * mb), FreeSpaceShape::fragmented);
}

TEST(FreeSpaceShape, NotEnoughInTotalIsExhaustionRatherThanFragmentation)
{
    // Compacting every byte still would not serve this. Evicting the cache might
    // free real bytes, but no rearrangement can.
    EXPECT_EQ(diagnose(20u * mb, 9u * mb, 15u * mb), FreeSpaceShape::exhausted);
}

TEST(FreeSpaceShape, ARequestThatAlreadyFitsIsNeither)
{
    EXPECT_EQ(diagnose(4u * mb, 9u * mb, 101u * mb), FreeSpaceShape::sufficient);
}

// The boundary matters: a request exactly the size of the largest block fits.
TEST(FreeSpaceShape, ARequestExactlyTheSizeOfTheLargestBlockFits)
{
    EXPECT_EQ(diagnose(9u * mb, 9u * mb, 101u * mb), FreeSpaceShape::sufficient);
}

// An unknown total cannot be used to rule fragmentation in or out, and must not
// be read as zero -- that would report exhaustion on every call before the
// sampler's first walk.
TEST(FreeSpaceShape, AnUnknownTotalIsNotReadAsExhaustion)
{
    EXPECT_EQ(diagnose(20u * mb, 9u * mb, 0u), FreeSpaceShape::unknown);
}

TEST(FreeSpaceShape, AnUnknownLargestBlockIsUnknownToo)
{
    EXPECT_EQ(diagnose(20u * mb, 0u, 101u * mb), FreeSpaceShape::unknown);
}

// ---------------------------------------------------------------------------
// Giving up. A live session on a 2 GB install logged on_failure=182 with
// evictions=122, managed=121, clears=0 and released=0, running the failure
// ladder to its terminal rung sixty times in a row while the largest free block
// sat unchanged at 106,496 bytes and the shape read exhausted throughout.
//
// The terminal rung is the most destructive thing available to the engine's
// object graph -- a full cache clear plus EvictManagedResources -- and every
// ~D3DResetable installs the abstract vtable before unregistering, which is the
// game's own reset race. Running it sixty times while the address space does not
// move is harm with nothing on the other side of the ledger.
// ---------------------------------------------------------------------------

TEST(RescuePolicy, TheFailurePathStopsOnceItsLadderHasRunTheLimit)
{
    auto inputs = healthy();
    inputs.attempt = 3u;
    inputs.failure_ladders = RescueConfig{}.failure_ladder_limit;

    EXPECT_FALSE(policy().plan(inputs).acts());
}

TEST(RescuePolicy, TheFailurePathStillActsOnTheLastLadderBeforeTheLimit)
{
    auto inputs = healthy();
    inputs.attempt = 3u;
    inputs.failure_ladders = RescueConfig{}.failure_ladder_limit - 1u;

    EXPECT_EQ(policy().plan(inputs).action, EvictAction::clear_cache_and_managed);
}

// Every rung, not just the terminal one. Leaving the gentler rungs armed would
// keep two thirds of the work running for the same nothing.
TEST(RescuePolicy, ReachingTheLimitStopsEveryRungOfTheFailurePath)
{
    for (std::uint32_t attempt = 1u; attempt <= 6u; ++attempt)
    {
        auto inputs = healthy();
        inputs.attempt = attempt;
        inputs.failure_ladders = RescueConfig{}.failure_ladder_limit;

        EXPECT_FALSE(policy().plan(inputs).acts()) << "attempt " << attempt << " kept acting past the limit";
    }
}

TEST(RescuePolicy, GivingUpSaysSoRatherThanReportingIdle)
{
    auto inputs = healthy();
    inputs.attempt = 3u;
    inputs.failure_ladders = RescueConfig{}.failure_ladder_limit;

    EXPECT_STRNE(policy().plan(inputs).reason, "idle");
}

// The escape hatch, matching RescueConfig::unbounded in spirit: a limit of zero
// restores the behaviour that shipped, without a rebuild.
TEST(RescuePolicy, ALimitOfZeroNeverGivesUp)
{
    auto config = RescueConfig{};
    config.failure_ladder_limit = 0u;

    auto inputs = healthy();
    inputs.attempt = 3u;
    inputs.failure_ladders = 1000u;

    EXPECT_EQ(RescuePolicy{config}.plan(inputs).action, EvictAction::clear_cache_and_managed);
}

// The limit belongs to the failure path alone. The preemptive ladder is bounded
// by consecutive_preemptive and must not be disarmed by a count it does not own.
TEST(RescuePolicy, ASpentFailureLadderDoesNotDisarmThePreemptivePath)
{
    auto inputs = healthy();
    inputs.attempt = 0u;
    inputs.largest_free_bytes = 8u * mb;
    inputs.requested_bytes = 8u * mb;
    inputs.failure_ladders = RescueConfig{}.failure_ladder_limit;

    EXPECT_TRUE(policy().plan(inputs).acts());
}
