#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "lyrium/config_parse.h"

using lyrium::apply_values;
using lyrium::Config;
using lyrium::ConfigValues;
using lyrium::parse_bool;
using lyrium::parse_ini;

namespace
{

auto parse(const std::string &text) -> ConfigValues
{
    auto stream = std::istringstream{text};
    return parse_ini(stream);
}

auto configured(const std::string &text) -> Config
{
    auto config = Config{};
    apply_values(config, parse(text));
    return config;
}

}

TEST(ConfigParse, BooleanFormsAccepted)
{
    EXPECT_TRUE(parse_bool("1", false));
    EXPECT_TRUE(parse_bool("true", false));
    EXPECT_TRUE(parse_bool("yes", false));
    EXPECT_TRUE(parse_bool("on", false));

    EXPECT_FALSE(parse_bool("0", true));
    EXPECT_FALSE(parse_bool("no", true));
}

TEST(ConfigParse, BooleanParsingIsCaseSensitiveAndFallbackOnlyAppliesToEmpty)
{
    // Characterisation, not endorsement. "TRUE" is not accepted, and a value
    // that is present but unrecognised yields false rather than the fallback.
    EXPECT_FALSE(parse_bool("TRUE", true)) << "uppercase is not recognised";
    EXPECT_FALSE(parse_bool("True", true));
    EXPECT_FALSE(parse_bool("maybe", true)) << "an unrecognised value is false, not the fallback";
    EXPECT_TRUE(parse_bool("", true)) << "the fallback applies only to an absent value";
}

TEST(ConfigParse, ReadsKeysAndTrimsWhitespace)
{
    const auto values = parse("  logging  =  1  \noverlay=0\n");

    EXPECT_EQ(values.at("logging"), "1");
    EXPECT_EQ(values.at("overlay"), "0");
}

TEST(ConfigParse, CommentsAreStrippedFromAnywhereInTheLine)
{
    // Including from inside a value, which silently truncates paths that
    // contain a hash. Pinned because it is surprising, not because it is right.
    const auto values = parse("logging=1 ; trailing comment\nlog_dir=C:\\logs#1\n# whole line\n");

    EXPECT_EQ(values.at("logging"), "1");
    EXPECT_EQ(values.at("log_dir"), "C:\\logs") << "a hash inside a value truncates it";
    EXPECT_FALSE(values.contains("# whole line"));
}

TEST(ConfigParse, SectionHeadersAreIgnoredAndDuplicateKeysTakeTheLastValue)
{
    const auto values = parse("[lyrium]\nlogging=0\nlogging=1\n");

    EXPECT_FALSE(values.contains("[lyrium]")) << "section lines survive only by having no equals sign";
    EXPECT_EQ(values.at("logging"), "1");
}

TEST(ConfigParse, MegabyteAndKilobyteSuffixesConvert)
{
    // These conversions produce the memory thresholds the whole project runs on.
    const auto config = configured(
        "texture_pool_min_kb=64\n"
        "rescue_low_watermark_mb=8\n"
        "rescue_large_create_kb=512\n");

    EXPECT_EQ(config.texture_pool.minimum_bytes, 64ull * 1024ull);
    EXPECT_EQ(config.rescue.low_watermark_bytes, 8ull * 1024ull * 1024ull);
    EXPECT_EQ(config.rescue.large_create_bytes, 512ull * 1024ull);
}

TEST(ConfigParse, AbsentKeysLeaveTheStructDefaults)
{
    const auto config = configured("");
    const auto defaults = Config{};

    EXPECT_EQ(config.rescue.low_watermark_bytes, defaults.rescue.low_watermark_bytes);
    EXPECT_EQ(config.texture_pool.minimum_bytes, defaults.texture_pool.minimum_bytes);
    EXPECT_EQ(config.sample_interval_ms, defaults.sample_interval_ms);
}

TEST(ConfigParse, TheSampleIntervalIsClampedToAFloor)
{
    EXPECT_EQ(configured("sample_interval_ms=10\n").sample_interval_ms, 250);
    EXPECT_EQ(configured("sample_interval_ms=0\n").sample_interval_ms, 250);
    EXPECT_EQ(configured("sample_interval_ms=1000\n").sample_interval_ms, 1000);
}

TEST(ConfigParse, GarbageNumbersSilentlyBecomeZero)
{
    // strtoull returns 0 rather than failing, so a typo does not fall back to
    // the default -- it sets a zero threshold. Pinned so the behaviour is known,
    // because the consequence is silent: rescue_low_watermark_mb=abc disarms the
    // watermark rather than leaving it where the struct declares it.
    const auto config = configured("rescue_low_watermark_mb=abc\n");

    EXPECT_EQ(config.rescue.low_watermark_bytes, 0u) << "a malformed number yields zero, not the default";
    EXPECT_NE(config.rescue.low_watermark_bytes, Config{}.rescue.low_watermark_bytes);
}

TEST(ConfigParse, OverlayDefaultDisagreesBetweenTheStructAndTheParser)
{
    // Characterisation of a real inconsistency. Config::overlay is declared
    // true, but apply_values passes a fallback of false, so an absent key gives
    // the opposite of the declared default. Whichever is intended, they should
    // agree; this pins the current answer so a fix is a deliberate change.
    EXPECT_TRUE(Config{}.overlay) << "the struct declares overlay on by default";
    EXPECT_FALSE(configured("").overlay) << "but an absent key turns it off";
}

TEST(ConfigParse, PoolHooksAreOffUnlessAskedFor)
{
    // pool_alloc sits on the entry point every engine allocation goes through, so
    // this one has to be opted into rather than merely not opted out of.
    EXPECT_FALSE(configured("").engine.hook_pool);
    EXPECT_FALSE(configured("pool_hooks=0").engine.hook_pool);
    EXPECT_TRUE(configured("pool_hooks=1").engine.hook_pool);
}
