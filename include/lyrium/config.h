#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>

#include "lyrium/dao/engine_hooks.h"
#include "lyrium/log.h"
#include "lyrium/utils.h"

namespace lyrium
{

struct RescueConfig
{

    bool on_failure{true};

    bool preemptive{true};
    std::uint64_t low_watermark_bytes{16ull * 1024ull * 1024ull};
    std::uint64_t large_create_bytes{1ull * 1024ull * 1024ull};

    bool evict_managed{true};
};

struct TexturePoolConfig
{
    bool prefer_default{true};

    std::uint64_t minimum_bytes{256ull * 1024ull};

    bool fall_back_to_managed{true};
};

struct RecyclerConfig
{
    bool enabled{false};

    std::uint64_t budget_bytes{96ull * 1024ull * 1024ull};
    std::uint32_t max_per_key{8};
};

struct Config
{
    dao::EngineConfig engine{};
    RescueConfig rescue{};
    TexturePoolConfig texture_pool{};
    RecyclerConfig recycler{};
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

inline auto load_config() -> Config
{
    auto config = Config{};

    const auto directory = executable_directory();
    const auto path = directory / "lyrium.ini";

    auto values = std::unordered_map<std::string, std::string>{};
    if (auto file = std::ifstream{path}; file)
    {
        auto line = std::string{};
        while (std::getline(file, line))
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
    }

    const auto lookup = [&values](const char *key) -> std::string
    {
        const auto found = values.find(key);
        return found == values.end() ? std::string{} : found->second;
    };

    config.engine.hook_texture_paths = parse_bool(lookup("engine_hooks"), true);
    config.engine.hook_cache = parse_bool(lookup("cache_hooks"), true);
    config.engine.hook_allocator = parse_bool(lookup("allocator_hooks"), false);

    config.overlay = parse_bool(lookup("overlay"), false);
    config.allocation_watch = parse_bool(lookup("allocation_watch"), false);
    config.logging = parse_bool(lookup("logging"), false);

    config.recycler.enabled = parse_bool(lookup("recycler"), false);
    if (const auto value = lookup("recycler_budget_mb"); !value.empty())
    {
        config.recycler.budget_bytes = std::strtoull(value.c_str(), nullptr, 10) * 1024ull * 1024ull;
    }
    if (const auto value = lookup("recycler_max_per_key"); !value.empty())
    {
        config.recycler.max_per_key = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    }

    config.texture_pool.prefer_default = parse_bool(lookup("texture_pool_default"), true);
    config.texture_pool.fall_back_to_managed = parse_bool(lookup("texture_pool_fallback"), true);
    if (const auto value = lookup("texture_pool_min_kb"); !value.empty())
    {
        config.texture_pool.minimum_bytes = std::strtoull(value.c_str(), nullptr, 10) * 1024ull;
    }

    config.rescue.on_failure = parse_bool(lookup("rescue_on_failure"), true);
    config.rescue.preemptive = parse_bool(lookup("rescue_preemptive"), true);
    config.rescue.evict_managed = parse_bool(lookup("rescue_evict_managed"), true);
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

    if (const auto log_dir = lookup("log_dir"); !log_dir.empty())
    {
        config.log_directory = std::filesystem::path{log_dir};
    }
    else
    {
        config.log_directory = directory / "lyrium_logs";
    }

    if (config.logging)
    {
        auto error = std::error_code{};
        std::filesystem::create_directories(config.log_directory, error);
        if (error)
        {
            config.log_directory = get_temp("lyrium_logs");
            std::filesystem::create_directories(config.log_directory, error);
        }
    }

    return config;
}

}
