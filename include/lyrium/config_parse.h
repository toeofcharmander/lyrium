#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <istream>
#include <string>
#include <unordered_map>

#include "lyrium/dao/engine_hooks.h"

// The parsing and unit conversion half of the configuration, free of Windows
// headers so it can be tested.
//
// These conversions produce every memory threshold the project uses -- the
// the pool-override minimum, the rescue watermarks -- and none
// of them had any coverage. The remaining half, which needs to know where the
// executable lives and where logs may be written, stays in config.h.

namespace lyrium
{

struct RescueConfig
{

    bool on_failure{true};

    bool preemptive{true};
    std::uint64_t low_watermark_bytes{16ull * 1024ull * 1024ull};
    std::uint64_t large_create_bytes{1ull * 1024ull * 1024ull};

    bool evict_managed{true};

    // Escape hatch: restores the old unbounded eviction from configuration,
    // without a rebuild, for a release whose failure mode is a crash in a long
    // save.
    bool unbounded{false};
};

struct TexturePoolConfig
{
    bool prefer_default{true};

    std::uint64_t minimum_bytes{256ull * 1024ull};

    bool fall_back_to_managed{true};
};

struct Config
{
    dao::EngineConfig engine{};
    RescueConfig rescue{};
    TexturePoolConfig texture_pool{};
    std::int64_t sample_interval_ms{5000};
    bool overlay{true};
    bool allocation_watch{false};

    bool logging{false};

    std::filesystem::path log_directory{};
};

inline auto parse_bool(const std::string &value, bool fallback) -> bool
{
    if (value.empty())
    {
        return fallback;
    }
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

using ConfigValues = std::unordered_map<std::string, std::string>;

// Note the format quirks this pins, all of which are current behaviour rather
// than intent: a comment marker is stripped from anywhere in the line including
// inside a value, section headers survive only by accident because they contain
// no '=', and a duplicate key silently wins.
inline auto parse_ini(std::istream &input) -> ConfigValues
{
    auto values = ConfigValues{};
    auto line = std::string{};

    while (std::getline(input, line))
    {
        const auto hash = line.find_first_of(";#");
        if (hash != std::string::npos)
        {
            line.erase(hash);
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }

        auto key = line.substr(0, equals);
        auto value = line.substr(equals + 1);

        const auto trim = [](std::string &text)
        {
            const auto first = text.find_first_not_of(" \t\r\n");
            const auto last = text.find_last_not_of(" \t\r\n");
            text = first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
        };
        trim(key);
        trim(value);

        if (!key.empty())
        {
            values[key] = value;
        }
    }

    return values;
}

inline auto apply_values(Config &config, const ConfigValues &values) -> void
{
    const auto lookup = [&values](const char *key) -> std::string
    {
        const auto found = values.find(key);
        return found == values.end() ? std::string{} : found->second;
    };

    config.engine.hook_texture_paths = parse_bool(lookup("engine_hooks"), true);
    config.engine.hook_cache = parse_bool(lookup("cache_hooks"), true);
    config.engine.hook_allocator = parse_bool(lookup("allocator_hooks"), false);
    config.engine.hook_pool = parse_bool(lookup("pool_hooks"), false);
    if (const auto value = lookup("side_pool_mb"); !value.empty())
    {
        config.engine.side_pool_bytes = std::strtoull(value.c_str(), nullptr, 10) * 1024ull * 1024ull;
    }
    if (const auto value = lookup("side_pool_threshold_kb"); !value.empty())
    {
        config.engine.side_pool_threshold_bytes = std::strtoull(value.c_str(), nullptr, 10) * 1024ull;
    }

    config.overlay = parse_bool(lookup("overlay"), false);
    config.allocation_watch = parse_bool(lookup("allocation_watch"), false);
    config.logging = parse_bool(lookup("logging"), false);

    config.texture_pool.prefer_default = parse_bool(lookup("texture_pool_default"), true);
    config.texture_pool.fall_back_to_managed = parse_bool(lookup("texture_pool_fallback"), true);
    if (const auto value = lookup("texture_pool_min_kb"); !value.empty())
    {
        config.texture_pool.minimum_bytes = std::strtoull(value.c_str(), nullptr, 10) * 1024ull;
    }

    config.rescue.on_failure = parse_bool(lookup("rescue_on_failure"), true);
    config.rescue.preemptive = parse_bool(lookup("rescue_preemptive"), true);
    config.rescue.evict_managed = parse_bool(lookup("rescue_evict_managed"), true);

    config.rescue.unbounded = parse_bool(lookup("rescue_unbounded"), false);
    if (const auto value = lookup("rescue_low_watermark_mb"); !value.empty())
    {
        config.rescue.low_watermark_bytes = std::strtoull(value.c_str(), nullptr, 10) * 1024ull * 1024ull;
    }
    if (const auto value = lookup("rescue_large_create_kb"); !value.empty())
    {
        config.rescue.large_create_bytes = std::strtoull(value.c_str(), nullptr, 10) * 1024ull;
    }

    if (const auto threshold = lookup("allocation_threshold"); !threshold.empty())
    {
        config.engine.allocation_log_threshold = std::strtoull(threshold.c_str(), nullptr, 10);
    }
    if (const auto interval = lookup("sample_interval_ms"); !interval.empty())
    {
        config.sample_interval_ms = std::strtoll(interval.c_str(), nullptr, 10);
        if (config.sample_interval_ms < 250)
        {
            config.sample_interval_ms = 250;
        }
    }
}
}
