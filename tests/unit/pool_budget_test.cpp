#include <cstdint>
#include <optional>

#include <gtest/gtest.h>

#include "lyrium/dao/pool_budget.h"

using lyrium::dao::evaluate_main_pool;
using lyrium::dao::expected_main_pool_bytes;
using lyrium::dao::stock_budget_bytes;
using lyrium::dao::strings_pool_bytes;

namespace
{

constexpr auto mb = std::uint64_t{1024u * 1024u};

}

TEST(PoolBudget, StringsPoolIsFiftyFiveMegabytes)
{
    // 0x3700000, the first record's size in FUN_004b8da0. Everything else here is
    // derived from it, so it is worth stating in megabytes once.
    EXPECT_EQ(strings_pool_bytes, 55u * mb);
}

TEST(PoolBudget, StockBudgetIsEightHundredAndFiftyMegabytes)
{
    EXPECT_EQ(stock_budget_bytes, 850u * mb);
}

TEST(PoolBudget, TheStockBudgetAsksForSevenHundredAndNinetyFiveMegabytes)
{
    // The number that matters and that the documentation used to get wrong: the
    // main pool is the budget minus the strings pool, not the budget.
    EXPECT_EQ(expected_main_pool_bytes(stock_budget_bytes), 795u * mb);
}

TEST(PoolBudget, SettingSevenHundredAndSixtyEightAsksForSevenHundredAndThirteen)
{
    EXPECT_EQ(expected_main_pool_bytes(768u * mb), 713u * mb);
}

TEST(PoolBudget, ABudgetSmallerThanTheStringsPoolExpectsNothingRatherThanUnderflowing)
{
    EXPECT_EQ(expected_main_pool_bytes(16u * mb), 0u);
}

TEST(PoolBudget, APoolThatGotWhatItAskedForDidNotBackOff)
{
    const auto outcome = evaluate_main_pool(stock_budget_bytes, 795u * mb);

    EXPECT_FALSE(outcome.backed_off);
    EXPECT_EQ(outcome.shortfall_bytes, 0u);
}

TEST(PoolBudget, APoolThatCameUpShortReportsHowFar)
{
    // The back-off loop retries 1 MB smaller until the request fits, so a short
    // pool is the only evidence that the address space could not satisfy it.
    const auto outcome = evaluate_main_pool(stock_budget_bytes, 700u * mb);

    EXPECT_TRUE(outcome.backed_off);
    EXPECT_EQ(outcome.shortfall_bytes, 95u * mb);
}

TEST(PoolBudget, APoolLargerThanExpectedReportsNoShortfall)
{
    // Cannot happen from the engine's own loop, but the figure arrives from a
    // detour reading engine memory and must not wrap when it surprises us.
    const auto outcome = evaluate_main_pool(stock_budget_bytes, 900u * mb);

    EXPECT_FALSE(outcome.backed_off);
    EXPECT_EQ(outcome.shortfall_bytes, 0u);
}

TEST(PoolBudget, AFailedPoolIsShortByEverythingItAskedFor)
{
    const auto outcome = evaluate_main_pool(stock_budget_bytes, 0u);

    EXPECT_TRUE(outcome.backed_off);
    EXPECT_EQ(outcome.shortfall_bytes, 795u * mb);
}

TEST(PoolBudget, AnUnobservedPoolIsNotReportedAsBackedOff)
{
    // The registrar runs at engine startup, long before the mod's hooks exist, so
    // "we never saw it" is the normal case and must not read as "it got nothing".
    // Reporting a 713 MB shortfall for a pool that is almost certainly fine is
    // worse than reporting nothing at all.
    const auto outcome = evaluate_main_pool(stock_budget_bytes, std::nullopt);

    EXPECT_FALSE(outcome.observed);
    EXPECT_FALSE(outcome.backed_off);
    EXPECT_EQ(outcome.shortfall_bytes, 0u);
}

TEST(PoolBudget, AnObservedPoolSaysSo)
{
    const auto outcome = evaluate_main_pool(stock_budget_bytes, 795u * mb);

    EXPECT_TRUE(outcome.observed);
}

TEST(PoolBudget, WithoutASidePoolTheBudgetIsUntouched)
{
    const auto split = lyrium::dao::plan_pool_split(768u * mb, 0u, false);

    EXPECT_EQ(split.budget_bytes, 768u * mb);
    EXPECT_EQ(split.side_bytes, 0u);
}

TEST(PoolBudget, OnATwoGigImageTheSidePoolComesOutOfTheBudget)
{
    // The failure this prevents is a hard freeze on map load. Leaving the budget
    // at 768 and adding 192 put 905 MB of pools in a 2 GB address space and the
    // game stopped during a level load with every counter reading healthy.
    const auto split = lyrium::dao::plan_pool_split(768u * mb, 192u * mb, false);

    EXPECT_EQ(split.budget_bytes, 576u * mb);
    EXPECT_EQ(split.side_bytes, 192u * mb);
    EXPECT_EQ(split.budget_bytes + split.side_bytes, 768u * mb);
}

TEST(PoolBudget, OnAFourGigImageTheSidePoolIsAdditive)
{
    // With 2.1 GB never touched above the line there is nothing to pay for, and
    // shrinking the main pool would be a cost with no benefit.
    const auto split = lyrium::dao::plan_pool_split(768u * mb, 192u * mb, true);

    EXPECT_EQ(split.budget_bytes, 768u * mb);
    EXPECT_EQ(split.side_bytes, 192u * mb);
}

TEST(PoolBudget, ASubtractionThatWouldStarveTheMainPoolDropsTheSidePoolInstead)
{
    // Degrading to today's behaviour is always available and always safe. Running
    // with a main pool too small to hold the engine's working set is not.
    const auto split = lyrium::dao::plan_pool_split(400u * mb, 320u * mb, false);

    EXPECT_EQ(split.side_bytes, 0u);
    EXPECT_EQ(split.budget_bytes, 400u * mb);
}
