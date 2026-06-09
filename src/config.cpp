#include "config.h"
#include "window.h"

#include <algorithm>
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
    return {
        {"renderer", "VULKAN"},
        {"fullscreen", false},
        {"maximized", false},
        {"display", 1},
        {"x", 96},
        {"y", 96},
        {"width", DEFAULT_CLIENT_WIDTH},
        {"height", DEFAULT_CLIENT_HEIGHT}
    };
}

std::filesystem::path getExecutableDirectory()
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return std::filesystem::current_path();
    return std::filesystem::path(buffer).parent_path();
}

template <typename T>
void assignOrDefault(const char* key, const T& defaultValue)
{
    if (!config.contains(key) || config[key].is_null())
        config[key] = defaultValue;
}

void sanitizeConfig()
{
    if (!config.is_object())
        config = makeDefaultConfig();

    assignOrDefault("renderer", std::string("VULKAN"));
    assignOrDefault("fullscreen", false);
    assignOrDefault("maximized", false);
    assignOrDefault("display", 1);
    assignOrDefault("x", 96);
    assignOrDefault("y", 96);
    assignOrDefault("width", DEFAULT_CLIENT_WIDTH);
    assignOrDefault("height", DEFAULT_CLIENT_HEIGHT);

    if (!config["renderer"].is_string())
        config["renderer"] = "VULKAN";
    if (!config["fullscreen"].is_boolean())
        config["fullscreen"] = false;
    if (!config["maximized"].is_boolean())
        config["maximized"] = false;
    if (!config["display"].is_number_integer())
        config["display"] = 1;
    if (!config["x"].is_number_integer())
        config["x"] = 96;
    if (!config["y"].is_number_integer())
        config["y"] = 96;
    if (!config["width"].is_number_integer())
        config["width"] = DEFAULT_CLIENT_WIDTH;
    if (!config["height"].is_number_integer())
        config["height"] = DEFAULT_CLIENT_HEIGHT;

    config["renderer"] = "VULKAN";
    config["display"] = std::max(1, config["display"].get<int>());
    config["width"] = std::max(MIN_CLIENT_WIDTH, config["width"].get<int>());
    config["height"] = std::max(MIN_CLIENT_HEIGHT, config["height"].get<int>());
}
}

std::filesystem::path getConfigPath()
{
    return getExecutableDirectory() / "config.json";
}

void load_config(const std::filesystem::path& filename)
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

void save_config(const std::filesystem::path& filename)
{
    updateConfigFromWindow();
    sanitizeConfig();

    std::ofstream output(filename);
    if (!output.is_open())
        throw std::runtime_error("Failed to save configuration file: " + filename.string());

    output << std::setw(4) << config << '\n';
}