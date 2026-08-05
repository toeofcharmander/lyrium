#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>

#include "lyrium/config_parse.h"
#include "lyrium/dao/engine_hooks.h"
#include "lyrium/log.h"
#include "lyrium/utils.h"

namespace lyrium
{

// The structs, parse_bool, parse_ini and apply_values live in config_parse.h so
// they can be tested without a Windows toolchain. Included here so every
// existing call site is unchanged.

inline auto load_config() -> Config
{
    auto config = Config{};

    const auto directory = executable_directory();
    const auto path = directory / "lyrium.ini";

    auto values = ConfigValues{};
    if (auto file = std::ifstream{path}; file)
    {
        values = parse_ini(file);
    }
    apply_values(config, values);

    const auto lookup = [&values](const char *key) -> std::string
    {
        const auto found = values.find(key);
        return found == values.end() ? std::string{} : found->second;
    };

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
