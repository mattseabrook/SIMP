// config.h

#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <nlohmann/json.hpp>

//=============================================================================

extern nlohmann::json config;
extern std::string windowTitle;
extern const int MIN_CLIENT_WIDTH;
extern const int MIN_CLIENT_HEIGHT;
extern const int CONTENT_WIDTH;   // PVX video content resolution
extern const int CONTENT_HEIGHT;

//=============================================================================

constexpr const char *default_config = R"({
    "fullscreen": false,
    "width": 1920,
    "renderer": "DirectX",
    "display": 1,
    "x": 100,
    "y": 100,
    "pcmEnabled": true,
    "pcmVolume": 100,
    "midiEnabled": true,
    "midiMode": "opl3",
    "midiBank": 0,
    "midiVolume": 100,
    "mlookSensitivity": 50,
    "raycastFov": 90,
    "raycastSupersample": 4,
    "devMode": false
})";

//=============================================================================

void load_config(const std::string &filename);
void save_config(const std::string &filename);

#endif // CONFIG_H