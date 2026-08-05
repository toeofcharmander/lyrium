#include <cstdint>

#include <gtest/gtest.h>

#include "lyrium/diag/engine_health.h"

using lyrium::diag::diagnose_engine_health;
using lyrium::diag::EngineHealth;
using lyrium::diag::EngineHealthConfig;
using lyrium::diag::name_of;
using lyrium::diag::worth_warning;

// Why this exists.
//
// Shrinking the engine's main memory pool with main_pool_mb is the largest
// address-space win available -- 850 MB down to 704 MB moved the largest free
// block below the 2 GB line from 2.4 MB to 78.2 MB. Cut too far and the engine
// starves inside its own pool, and its failure mode is to **skip the asset and
// carry on**. A run at 512 MB reported creates=4622 failures=0 while the world
// was visibly missing geometry: D3D never failed, so nothing lyrium logged could
// see it, and the only detector was a person looking at the screen.
//
// The engine's own create counter has been collected since before this and
// printed nowhere. That is the same trap CLAUDE.md records from the other
// direction -- a counter nobody reads is not a diagnostic.

namespace
{

constexpr auto config = EngineHealthConfig{};

}

// Before there is enough traffic to judge, saying nothing is the honest answer.
// A single failure among the first three creates is not a starving pool.
TEST(EngineHealth, SaysNothingBeforeThereIsEnoughToJudge)
{
    EXPECT_EQ(diagnose_engine_health(0u, 0u, config), EngineHealth::unknown);
    EXPECT_EQ(diagnose_engine_health(config.minimum_creates - 1u, 5u, config), EngineHealth::unknown);
}

TEST(EngineHealth, NoFailuresIsHealthy)
{
    EXPECT_EQ(diagnose_engine_health(5313u, 0u, config), EngineHealth::healthy);
}

// The measured shape of a good session: thousands of engine creates and not one
// failure. Anything above zero is worth naming, because a healthy run does not
// produce them at all.
TEST(EngineHealth, AnySustainedFailureIsWorthSaying)
{
    EXPECT_EQ(diagnose_engine_health(5000u, 1u, config), EngineHealth::degraded);
}

TEST(EngineHealth, ALargeShareOfFailuresIsStarvation)
{
    // 1% of creates failing is not bad luck.
    EXPECT_EQ(diagnose_engine_health(1000u, 10u, config), EngineHealth::starving);
    EXPECT_EQ(diagnose_engine_health(4622u, 900u, config), EngineHealth::starving);
}

TEST(EngineHealth, TheStarvationBoundaryIsWhereItSays)
{
    // starving_permille is per thousand, so 10 means 1%.
    EXPECT_EQ(diagnose_engine_health(1000u, 9u, config), EngineHealth::degraded);
    EXPECT_EQ(diagnose_engine_health(1000u, 10u, config), EngineHealth::starving);
}

// Failures cannot exceed creates, but a torn read across two relaxed atomics
// can make it look that way for one sample. Reporting starvation is right;
// dividing by zero or wrapping is not.
TEST(EngineHealth, SurvivesMoreFailuresThanCreates)
{
    EXPECT_EQ(diagnose_engine_health(100u, 4000u, config), EngineHealth::starving);
}

TEST(EngineHealth, EveryVerdictHasAName)
{
    EXPECT_STREQ(name_of(EngineHealth::unknown), "unknown");
    EXPECT_STREQ(name_of(EngineHealth::healthy), "healthy");
    EXPECT_STREQ(name_of(EngineHealth::degraded), "degraded");
    EXPECT_STREQ(name_of(EngineHealth::starving), "starving");
}

// The warning must fire once, not on every sample. A five-second sampler would
// otherwise print the same line for the rest of the session and bury the log it
// is trying to make readable.
TEST(EngineHealth, WorthWarningOnlyOnTheFirstStarvingVerdict)
{
    EXPECT_TRUE(worth_warning(EngineHealth::starving, EngineHealth::healthy));
    EXPECT_FALSE(worth_warning(EngineHealth::starving, EngineHealth::starving));
}

TEST(EngineHealth, AHealthyVerdictNeverWarns)
{
    EXPECT_FALSE(worth_warning(EngineHealth::healthy, EngineHealth::unknown));
    EXPECT_FALSE(worth_warning(EngineHealth::unknown, EngineHealth::unknown));
    EXPECT_FALSE(worth_warning(EngineHealth::degraded, EngineHealth::healthy));
}

// Degrading further after a warning has already gone out is worth one more line:
// it is the difference between a pool that is slightly tight and one that is
// deleting the world.
TEST(EngineHealth, EscalatingFromDegradedToStarvingWarnsAgain)
{
    EXPECT_TRUE(worth_warning(EngineHealth::starving, EngineHealth::degraded));
}
