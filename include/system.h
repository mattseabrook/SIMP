#ifndef SIMP_SYSTEM_H
#define SIMP_SYSTEM_H

#include <cstdint>
#include <filesystem>
#include <vector>

#include <windows.h>

struct ThemePalette
{
    std::uint8_t red = 32;
    std::uint8_t green = 32;
    std::uint8_t blue = 32;
    std::uint8_t alpha = 255;
    bool darkMode = true;
};

struct DecodedImage
{
    UINT width = 0;
    UINT height = 0;
    std::vector<std::uint8_t> pixels;
};

bool initializeSystemServices();
void shutdownSystemServices();
void refreshThemePalette();
const ThemePalette& getThemePalette();
COLORREF getThemeColorRef();
void applyWindowTheme(HWND hwnd);
bool decodePngResourceBGRA(int resourceID, DecodedImage& image);
bool decodeImageFileBGRA(const std::filesystem::path& filename, DecodedImage& image);
bool showSplashScreen(HINSTANCE instance, HWND owner, const RECT& monitorBounds);

#endif
