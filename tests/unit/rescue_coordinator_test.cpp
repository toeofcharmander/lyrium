#include <cstdint>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "lyrium/policy/rescue_coordinator.h"

using lyrium::policy::Clock;
using lyrium::policy::EvictionBackend;
using lyrium::policy::FreeSpaceProbe;
using lyrium::policy::RescueConfig;
using lyrium::policy::RescueCoordinator;

namespace
{

constexpr auto mb = std::uint64_t{1024u * 1024u};

class ManualClock final : public Clock
{
  public:
    [[nodiscard]] auto now_us() const -> std::int64_t override
    {
        return now_;
    }

    auto advance(std::int64_t us) -> void
    {
        now_ += us;
    }

  private:
    std::int64_t now_{1'000'000};
};

class FakeProbe final : public FreeSpaceProbe
{
  public:
    [[nodiscard]] auto largest_free_bytes() const -> std::uint64_t override
    {
        return largest_;
    }

    auto set(std::uint64_t bytes) -> void
    {
        largest_ = bytes;
    }

  private:
    std::uint64_t largest_{512u * mb};
};

// Models an engine cache that frees a fixed amount of contiguous space per
// evicted entry, so the coordinator's effect on headroom can be observed.
class FakeBackend final : public EvictionBackend
{
  public:
    [[nodiscard]] auto cache_available() const -> bool override
    {
        return available;
    }

    [[nodiscard]] auto pending_releases() const -> std::int32_t override
    {
        return pending;
    }

    [[nodiscard]] auto evict(std::int32_t max_count) -> std::int32_t override
    {
        evict_calls.push_back(max_count);
        if (evict_reclaims_nothing)
        {
            return 0;
        }
        const auto taken = max_count < pending ? max_count : pending;
        pending -= taken;
        return taken;
    }

    [[nodiscard]] auto clear_cache() -> bool override
    {
        ++clear_calls;
        pending = 0;
        return true;
    }

    auto evict_managed_resources() -> void override
    {
        ++managed_calls;
    }

    bool available{true};
    std::int32_t pending{1000};
    bool evict_reclaims_nothing{false};
    std::vector<std::int32_t> evict_calls{};
    int clear_calls{};
    int managed_calls{};
};

struct Fixture
{
    ManualClock clock{};
    FakeProbe probe{};
    FakeBackend backend{};

    auto make(const RescueConfig &config = RescueConfig{}) -> RescueCoordinator
    {
        return RescueCoordinator{config, backend, probe, clock};
    }
};

}

TEST(RescueCoordinator, DoesNothingWhenThereIsPlentyOfHeadroom)
{
    Fixture f{};
    auto coordinator = f.make();

    const auto outcome = coordinator.consider(4u * mb, 0u);

    EXPECT_FALSE(outcome.acted);
    EXPECT_TRUE(f.backend.evict_calls.empty());
}

TEST(RescueCoordinator, TheFirstRescueIsNotSwallowedByTheRateLimit)
{
    // The coordinator must seed last_rescue_us far enough back, because the
    // policy cannot tell "never rescued" from "rescued a moment ago".
    Fixture f{};
    auto coordinator = f.make();
    f.probe.set(1u * mb);

    EXPECT_TRUE(coordinator.consider(4u * mb, 0u).acted);
}

TEST(RescueCoordinator, PreemptiveEvictionIsBoundedAndRateLimited)
{
    Fixture f{};
    auto coordinator = f.make();
    f.probe.set(1u * mb);

    ASSERT_TRUE(coordinator.consider(4u * mb, 0u).acted);
    ASSERT_EQ(f.backend.evict_calls.size(), 1u);
    EXPECT_EQ(f.backend.evict_calls[0], RescueConfig{}.evict_batch);

    // Immediately again: suppressed, which is what stops the per-create eviction
    // storm that produced the stutter.
    EXPECT_FALSE(coordinator.consider(4u * mb, 0u).acted);
    EXPECT_EQ(f.backend.evict_calls.size(), 1u);

    f.clock.advance(RescueConfig{}.min_interval_us + 1);
    EXPECT_TRUE(coordinator.consider(4u * mb, 0u).acted);
    EXPECT_EQ(f.backend.evict_calls.size(), 2u);
}

TEST(RescueCoordinator, EscalatesThroughToAFullClearWhenEvictionReclaimsNothing)
{
    // The decisive safety case. A backend whose eviction frees nothing must not
    // leave the coordinator retrying the same bounded batch forever: it has to
    // reach the strongest action available.
    Fixture f{};
    auto coordinator = f.make();
    f.probe.set(1u * mb);
    f.backend.evict_reclaims_nothing = true;

    for (std::uint32_t attempt = 1u; attempt <= 3u; ++attempt)
    {
        EXPECT_TRUE(coordinator.consider(4u * mb, attempt).acted) << "attempt " << attempt;
    }

    EXPECT_EQ(f.backend.clear_calls, 1) << "escalation must terminate in clearing the cache";
    EXPECT_GT(f.backend.managed_calls, 0) << "and in evicting managed resources";
}

TEST(RescueCoordinator, AFailedCreateActsEvenWhenTheRateLimitWouldSuppressIt)
{
    Fixture f{};
    auto coordinator = f.make();
    f.probe.set(1u * mb);

    ASSERT_TRUE(coordinator.consider(4u * mb, 0u).acted);
    const auto calls_after_preemptive = f.backend.evict_calls.size();

    // No clock advance: a preemptive check here would be rate limited.
    EXPECT_TRUE(coordinator.consider(4u * mb, 1u).acted) << "a real failure must never be rate limited away";
    EXPECT_GT(f.backend.evict_calls.size(), calls_after_preemptive);
}

TEST(RescueCoordinator, HoldsThePressureLatchAcrossCalls)
{
    Fixture f{};
    auto coordinator = f.make();

    f.probe.set(1u * mb);
    ASSERT_TRUE(coordinator.consider(4u * mb, 0u).acted);
    EXPECT_TRUE(coordinator.stats().under_pressure);

    // Recovering a little is not enough to leave pressure.
    f.probe.set(RescueConfig{}.headroom_floor_bytes + 1u);
    f.clock.advance(RescueConfig{}.min_interval_us + 1);
    (void)coordinator.consider(4u * mb, 0u);
    EXPECT_TRUE(coordinator.stats().under_pressure) << "hysteresis: a small recovery must not clear the latch";

    // Recovering properly does.
    f.probe.set(4096u * mb);
    f.clock.advance(RescueConfig{}.min_interval_us + 1);
    (void)coordinator.consider(4u * mb, 0u);
    EXPECT_FALSE(coordinator.stats().under_pressure);
}

TEST(RescueCoordinator, DoesNotReEnterWhileAlreadyRescuing)
{
    // Eviction runs engine code that can create textures, which would call
    // straight back in. Without the guard that recurses until the stack ends.
    struct ReentrantBackend final : public EvictionBackend
    {
        RescueCoordinator *coordinator{};
        int depth{};

        [[nodiscard]] auto cache_available() const -> bool override
        {
            return true;
        }
        [[nodiscard]] auto pending_releases() const -> std::int32_t override
        {
            return 1000;
        }
        [[nodiscard]] auto evict(std::int32_t) -> std::int32_t override
        {
            ++depth;
            // The engine calls back into a create while we are inside evict.
            (void)coordinator->consider(4u * mb, 0u);
            return 1;
        }
        [[nodiscard]] auto clear_cache() -> bool override
        {
            return true;
        }
        auto evict_managed_resources() -> void override
        {
        }
    };

    ManualClock clock{};
    FakeProbe probe{};
    ReentrantBackend backend{};
    probe.set(1u * mb);

    RescueCoordinator coordinator{RescueConfig{}, backend, probe, clock};
    backend.coordinator = &coordinator;

    EXPECT_TRUE(coordinator.consider(4u * mb, 0u).acted);
    EXPECT_EQ(backend.depth, 1) << "the nested call must be refused, not recursed into";
    EXPECT_EQ(coordinator.stats().suppressed, 1u);
}

TEST(RescueCoordinator, WaitsForTheEngineCacheToBeDiscovered)
{
    Fixture f{};
    auto coordinator = f.make();
    f.probe.set(1u * mb);
    f.backend.available = false;

    EXPECT_FALSE(coordinator.consider(4u * mb, 0u).acted);
    EXPECT_TRUE(f.backend.evict_calls.empty());
}

TEST(RescueCoordinator, BoundedEvictionReclaimsLessThanUnboundedForTheSamePressure)
{
    // The precise claim behind the change: bounded does strictly less work.
    // That it is not also weaker is asserted by the policy's escalation tests.
    Fixture bounded{};
    auto bounded_coordinator = bounded.make();
    bounded.probe.set(1u * mb);
    (void)bounded_coordinator.consider(4u * mb, 0u);

    Fixture unbounded{};
    auto unbounded_coordinator = unbounded.make(RescueConfig{.unbounded = true});
    unbounded.probe.set(1u * mb);
    (void)unbounded_coordinator.consider(4u * mb, 0u);

    ASSERT_EQ(bounded.backend.evict_calls.size(), 1u);
    ASSERT_EQ(unbounded.backend.evict_calls.size(), 1u);
    EXPECT_LT(bounded.backend.evict_calls[0], unbounded.backend.evict_calls[0])
        << "the bounded run must ask for fewer evictions";

    // pending is what remains queued, so draining less leaves more behind. That
    // surviving cache is precisely the work the old behaviour threw away and had
    // to re-read from disk, which is the stutter.
    EXPECT_GT(bounded.backend.pending, unbounded.backend.pending)
        << "the bounded run should leave far more of the cache intact";
}

// RescueCoordinator holds raw pointers to objects it does not own. That is only
// safe because it is trivially destructible and so never runs at exit -- adding
// one std::string member would register a destructor that reads those pointers
// after the objects are gone. This fails here, on Linux, in seconds, rather than
// as an unexplained shutdown crash in the game months later.
static_assert(
    std::is_trivially_destructible_v<lyrium::policy::RescueCoordinator>,
    "RescueCoordinator must stay trivially destructible; see the composition in src/d3d9.cpp");

// A live session showed the pressure latch never setting while the address space
// dipped below the floor, and the log could not say which gate stopped it: the
// rescue line recorded outcomes only. These two record the decision itself.
TEST(RescueCoordinator, RecordsTheHeadroomItActuallySaw)
{
    Fixture f{};
    auto coordinator = f.make();
    f.probe.set(22u * mb);

    coordinator.consider(4u * mb, 0u);

    EXPECT_EQ(coordinator.stats().last_largest_free_bytes, 22u * mb);
}

TEST(RescueCoordinator, RecordsWhyItDeclinedToAct)
{
    Fixture f{};
    auto coordinator = f.make();

    // Healthy headroom and a small request: the policy declines, and the reason
    // has to say which of the several ways it declined. (A small request while
    // headroom is under the floor no longer declines at all -- see
    // ASmallCreateBelowTheFloorStillEntersPressure.)
    f.probe.set(512u * mb);
    coordinator.consider(64u * 1024u, 0u);

    ASSERT_NE(coordinator.stats().last_reason, nullptr);
    EXPECT_STRNE(coordinator.stats().last_reason, "");
    EXPECT_FALSE(coordinator.stats().under_pressure);
}

// A live session died at 31 MB of headroom having run exactly one preemptive
// eviction that released one texture. The ladder never escalated, because
// escalation is gated on attempt > 0 and attempt only rises when a texture
// create fails -- and no create failed. The process was killed by a different
// allocation starving while every texture succeeded. Pressure that survives
// repeated trims has to escalate on its own.
TEST(RescueCoordinator, SustainedPressureEscalatesWithoutWaitingForACreateToFail)
{
    Fixture f{};
    auto coordinator = f.make();
    f.probe.set(20u * mb);
    f.backend.evict_reclaims_nothing = true;

    for (auto i = 0; i < 6; ++i)
    {
        coordinator.consider(4u * mb, 0u);
        f.clock.advance(100'000);
    }

    EXPECT_GT(f.backend.managed_calls + f.backend.clear_calls, 0)
        << "trimming alone never escalated while pressure persisted";
}

TEST(RescueCoordinator, EscalationResetsAfterPressureIsRelieved)
{
    Fixture f{};
    auto coordinator = f.make();
    f.probe.set(20u * mb);
    f.backend.evict_reclaims_nothing = true;

    for (auto i = 0; i < 6; ++i)
    {
        coordinator.consider(4u * mb, 0u);
        f.clock.advance(100'000);
    }
    const auto escalated_before = f.backend.clear_calls;

    // Headroom recovers well past the exit multiple, then collapses again.
    f.probe.set(1024u * mb);
    coordinator.consider(4u * mb, 0u);
    f.clock.advance(100'000);
    ASSERT_FALSE(coordinator.stats().under_pressure);

    f.backend.evict_reclaims_nothing = false;
    f.probe.set(20u * mb);
    coordinator.consider(4u * mb, 0u);

    EXPECT_EQ(f.backend.clear_calls, escalated_before) << "a fresh pressure episode must start at the gentlest rung";
}

// last_reason is overwritten by every call, and the call after a rescue is
// almost always a small create idling, so the acting decision vanishes from the
// log immediately. Keep the acting one separately.
TEST(RescueCoordinator, RemembersTheLastActingReasonAcrossLaterIdleCalls)
{
    Fixture f{};
    auto coordinator = f.make();
    f.probe.set(20u * mb);

    ASSERT_TRUE(coordinator.consider(4u * mb, 0u).acted);
    const auto acting = coordinator.stats().last_action_reason;
    ASSERT_NE(acting, nullptr);
    EXPECT_STRNE(acting, "");

    // A small create idles out before the pressure test and overwrites last_reason.
    coordinator.consider(16u * 1024u, 0u);

    EXPECT_STREQ(coordinator.stats().last_action_reason, acting);
}

// The defect behind three crashed sessions. "request too small to be worth a
// rescue" was the reason on nearly every call while the largest low block fell
// from 35 MB to 16.5 MB, because the size gate ran before the policy ever looked
// at the address space. Small creates are exactly what drains this game's space,
// so once pressure is established every create has to keep the ladder turning.
TEST(RescueCoordinator, ASmallCreateUnderEstablishedPressureStillDrivesTheLadder)
{
    Fixture f{};
    auto coordinator = f.make();
    f.probe.set(20u * mb);
    f.backend.evict_reclaims_nothing = true;

    ASSERT_TRUE(coordinator.consider(4u * mb, 0u).acted);
    ASSERT_TRUE(coordinator.stats().under_pressure);

    // Nothing but small creates from here, as in the live session.
    for (auto i = 0; i < 6; ++i)
    {
        f.clock.advance(100'000);
        coordinator.consider(64u * 1024u, 0u);
    }

    EXPECT_GT(f.backend.managed_calls + f.backend.clear_calls, 0)
        << "small creates under pressure left the ladder stuck on its first rung";
}

// And a collapse that never happens to include a large create must still arm.
TEST(RescueCoordinator, ASmallCreateBelowTheFloorStillEntersPressure)
{
    Fixture f{};
    auto coordinator = f.make();
    f.probe.set(16u * mb);

    coordinator.consider(64u * 1024u, 0u);

    EXPECT_TRUE(coordinator.stats().under_pressure);
}

// The gate still applies in health, so ordinary small creates cost nothing.
TEST(RescueCoordinator, ASmallCreateWithHealthyHeadroomStillIdlesOutEarly)
{
    Fixture f{};
    auto coordinator = f.make();
    f.probe.set(512u * mb);

    EXPECT_FALSE(coordinator.consider(64u * 1024u, 0u).acted);
    EXPECT_FALSE(coordinator.stats().under_pressure);
}
