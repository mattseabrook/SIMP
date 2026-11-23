#include "config.h"
#include "window.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <stdexcept>

#include <windows.h>

nlohmann::json config;
std::string windowTitle = "SIMP";
const int DEFAULT_CLIENT_WIDTH = 1600;
const int DEFAULT_CLIENT_HEIGHT = 900;
const int MIN_CLIENT_WIDTH = 1280;
const int MIN_CLIENT_HEIGHT = 720;

namespace
{
nlohmann::json makeDefaultConfig()
{
    return {{"renderer", "VULKAN"},
            {"lastOpenFile", ""},
            {"fullscreen", false},
            {"maximized", false},
            {"display", 1},
            {"x", 96},
            {"y", 96},
            {"width", DEFAULT_CLIENT_WIDTH},
            {"height", DEFAULT_CLIENT_HEIGHT},
            {"dpi", 96}};
}

std::filesystem::path getExecutableDirectory()
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return std::filesystem::current_path();
    return std::filesystem::path(buffer).parent_path();
}

void sanitizeConfig()
{
    const auto defaults = makeDefaultConfig();
    if (!config.is_object())
        config = defaults;
    for (const auto &[key, fallback] : defaults.items())
    {
        auto &value = config[key];
        if (fallback.is_number_integer() ? !value.is_number_integer() : value.type() != fallback.type())
            value = fallback;
    }

    const struct
    {
        const char *key;
        int low, high;
    } limits[] = {{"display", 1, 64}, {"width", MIN_CLIENT_WIDTH, 16384}, {"height", MIN_CLIENT_HEIGHT, 16384},
                  {"x", -65536, 65536}, {"y", -65536, 65536}, {"dpi", 48, 768}};
    for (const auto &[key, low, high] : limits)
    {
        auto &value = config[key];
        const auto number = value.is_number_unsigned()
                                ? std::int64_t(std::min(value.get<std::uint64_t>(), std::uint64_t(high)))
                                : value.get<std::int64_t>();
        value = int(std::clamp(number, std::int64_t(low), std::int64_t(high)));
    }
    config["renderer"] = "VULKAN";
}
} // namespace

std::filesystem::path getConfigPath()
{
    return getExecutableDirectory() / "config.json";
}

void load_config(const std::filesystem::path &filename)
{
    config = makeDefaultConfig();

    bool shouldRewrite = false;
    if (std::filesystem::exists(filename))
    {
        try
        {
            std::ifstream input(filename);
            if (!input.is_open())
                throw std::runtime_error("failed to open config file");

            nlohmann::json loaded;
            input >> loaded;
            if (loaded.is_object())
                config.merge_patch(loaded);
            else
                shouldRewrite = true;
        }
        catch (...)
        {
            shouldRewrite = true;
        }
    }
    else
    {
        shouldRewrite = true;
    }

    sanitizeConfig();

    if (shouldRewrite)
        save_config(filename);
}

void save_config(const std::filesystem::path &filename)
{
    updateConfigFromWindow();
    sanitizeConfig();

    std::filesystem::path temporary = filename;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output.is_open())
            throw std::runtime_error("Failed to open temporary configuration file: " + temporary.string());
        output << std::setw(4) << config << '\n';
        output.close();
        if (!output)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw std::runtime_error("Failed to write configuration file: " + temporary.string());
        }
    }

    if (!MoveFileExW(temporary.c_str(), filename.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("Failed to replace configuration file: " + filename.string());
    }
}
