#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/diag/sample_schedule.h"

using lyrium::diag::SampleAction;
using lyrium::diag::SampleSchedule;

namespace
{

constexpr auto interval_ms = std::int64_t{5000};
constexpr auto poll_ms = std::int64_t{100};

// Ticks the schedule `count` times with nothing requested and reports what came
// back, so a test can say "nothing happens for four seconds" in one line.
auto quiet_ticks(SampleSchedule &schedule, int count) -> int
{
    auto samples = 0;
    for (auto tick = 0; tick < count; ++tick)
    {
        if (schedule.tick() != SampleAction::wait)
        {
            ++samples;
        }
    }
    return samples;
}

}

// The periodic behaviour that existed before requests did: a walk every
// interval, and nothing in between.
TEST(SampleSchedule, FiresPeriodicallyWhenNobodyAsks)
{
    auto schedule = SampleSchedule{interval_ms, poll_ms};

    EXPECT_EQ(quiet_ticks(schedule, static_cast<int>(interval_ms / poll_ms) - 1), 0);
    EXPECT_EQ(schedule.tick(), SampleAction::periodic);
}

TEST(SampleSchedule, APeriodicSampleRestartsTheInterval)
{
    auto schedule = SampleSchedule{interval_ms, poll_ms};

    ASSERT_EQ(quiet_ticks(schedule, static_cast<int>(interval_ms / poll_ms)), 1);

    EXPECT_EQ(quiet_ticks(schedule, static_cast<int>(interval_ms / poll_ms) - 1), 0);
    EXPECT_EQ(schedule.tick(), SampleAction::periodic);
}

// A request must not wait out the remaining interval: the state at a failed
// texture create is worth having promptly, which is the whole reason the walk
// used to be done inline on the create path.
TEST(SampleSchedule, ServesARequestOnTheVeryNextTick)
{
    auto schedule = SampleSchedule{interval_ms, poll_ms};

    schedule.request();

    EXPECT_EQ(schedule.tick(), SampleAction::requested);
}

// This is the defect the whole change exists for. A live session logged 240
// va[create_failed] reports, each carrying a full address-space walk of about
// 7 ms, all of them on the render thread during a failure cascade -- roughly
// 1.7 seconds of walking while the game was already dying. Requests arriving
// between two ticks must collapse into one walk, so the cost is bounded by time
// rather than by how badly the session is going.
TEST(SampleSchedule, ManyRequestsBetweenTicksCollapseIntoOneSample)
{
    auto schedule = SampleSchedule{interval_ms, poll_ms};

    for (auto failure = 0; failure < 240; ++failure)
    {
        schedule.request();
    }

    EXPECT_EQ(schedule.tick(), SampleAction::requested);
    EXPECT_EQ(schedule.tick(), SampleAction::wait) << "the request was already served";
}

TEST(SampleSchedule, DoesNotSampleWhenNothingWasAsked)
{
    auto schedule = SampleSchedule{interval_ms, poll_ms};

    EXPECT_EQ(schedule.tick(), SampleAction::wait);
}

// A cascade requests constantly. If a served request left the periodic clock
// running, every tick during the cascade would fire twice.
TEST(SampleSchedule, AServedRequestAlsoRestartsTheInterval)
{
    auto schedule = SampleSchedule{interval_ms, poll_ms};

    ASSERT_EQ(quiet_ticks(schedule, static_cast<int>(interval_ms / poll_ms) - 1), 0);

    schedule.request();
    ASSERT_EQ(schedule.tick(), SampleAction::requested);

    EXPECT_EQ(quiet_ticks(schedule, static_cast<int>(interval_ms / poll_ms) - 1), 0)
        << "the periodic that was one tick away must have been pushed out a full interval";
    EXPECT_EQ(schedule.tick(), SampleAction::periodic);
}

// A request outranks a periodic that comes due on the same tick, because it
// carries a reason worth naming in the log and the periodic does not.
TEST(SampleSchedule, ARequestWinsATickItSharesWithAPeriodic)
{
    auto schedule = SampleSchedule{interval_ms, poll_ms};

    ASSERT_EQ(quiet_ticks(schedule, static_cast<int>(interval_ms / poll_ms) - 1), 0);
    schedule.request();

    EXPECT_EQ(schedule.tick(), SampleAction::requested);
}

// An interval shorter than one poll must still fire rather than never coming
// due, since sample_interval_ms is user-supplied through lyrium.ini.
TEST(SampleSchedule, AnIntervalShorterThanOnePollFiresEveryTick)
{
    auto schedule = SampleSchedule{poll_ms / 2, poll_ms};

    EXPECT_EQ(schedule.tick(), SampleAction::periodic);
    EXPECT_EQ(schedule.tick(), SampleAction::periodic);
}

// The interval arrives from lyrium.ini, after the Sampler singleton exists.
TEST(SampleSchedule, TakesItsIntervalAfterConstruction)
{
    auto schedule = SampleSchedule{interval_ms, poll_ms};
    schedule.set_interval_ms(poll_ms * 2);

    EXPECT_EQ(schedule.tick(), SampleAction::wait);
    EXPECT_EQ(schedule.tick(), SampleAction::periodic);
}
