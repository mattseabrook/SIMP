#pragma once
#include <cstdint>
#include <vector>
struct CanvasViewport
{
    int left = 0, top = 0, right = 0, bottom = 0;
    bool contains(int x, int y) const
    {
        return x >= left && x < right && y >= top && y < bottom;
    }
};
struct CanvasOverlayRect
{
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;
};

struct CanvasRenderData
{
    std::uint32_t surfaceWidth = 0, surfaceHeight = 0;
    std::uint64_t canvasGeneration = 0;
    bool hasImage = false;
    float imageX = 0.0f;
    float imageY = 0.0f;
    float imageWidth = 0.0f;
    float imageHeight = 0.0f;
    CanvasViewport viewport{};
    std::vector<CanvasOverlayRect> overlays;
};
