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
