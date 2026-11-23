#ifndef SIMP_CONFIG_H
#define SIMP_CONFIG_H

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "app_version.h"

extern nlohmann::json config;
extern std::string windowTitle;
extern const int DEFAULT_CLIENT_WIDTH;
extern const int DEFAULT_CLIENT_HEIGHT;
extern const int MIN_CLIENT_WIDTH;
extern const int MIN_CLIENT_HEIGHT;

std::filesystem::path getConfigPath();
void load_config(const std::filesystem::path& filename);
void save_config(const std::filesystem::path& filename);

#endif
