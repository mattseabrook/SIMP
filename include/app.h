#ifndef SIMP_APP_H
#define SIMP_APP_H

#include <cstdint>
#include <vector>

#include <windows.h>

struct CanvasOverlayRect
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;
};

struct CanvasRenderData
{
    bool hasImage = false;
    float imageX = 0.0f;
    float imageY = 0.0f;
    float imageWidth = 0.0f;
    float imageHeight = 0.0f;
    std::vector<CanvasOverlayRect> overlays;
};

void initializeCanvasCaliper(HWND hwnd);
void shutdownCanvasCaliper();
void layoutCanvasCaliperChrome();
void loadCanvasCaliperConfig();
void saveCanvasCaliperConfig();
void openCanvasCaliperPngDialog(HWND owner);
bool loadCanvasCaliperPngPath(HWND owner, const wchar_t* path);
void clearCanvasCaliperImage();
bool handleCanvasCaliperCommand(HWND hwnd, WPARAM wParam, LPARAM lParam);
bool handleCanvasCaliperWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
void updateCanvasCaliperMenu(HMENU menu);
CanvasRenderData buildCanvasRenderData(std::uint32_t width, std::uint32_t height);
bool canvasCaliperGridVisible();
bool canvasCaliperRulersVisible();
void setCanvasCaliperGridVisible(bool visible);
void setCanvasCaliperRulersVisible(bool visible);

#endif
