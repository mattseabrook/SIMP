#include "simpformat.h"
#include "pixel_font.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    constexpr int w = 320, h = 240;
    SimpDocument document;
    document.hasCels = true;
    document.hasLayer = true;
    document.canvasWidthIn = w / 100.0;
    document.canvasHeightIn = h / 100.0;
    document.layerPpi = 100;
    document.gridSizeIn = 0.08;
    document.majorEvery = 4;
    document.showGrid = false;
    document.showRulers = true;
    document.layerName = L"Retro cel study";
    document.stack.cels.clear();
    for (int i = 0; i < 3; ++i)
    {
        Cel cel;
        cel.id = i + 1;
        cel.name = i == 0 ? L"Night sky" : i == 1 ? L"Sun and skyline" : L"Neon ink";
        cel.image = {w, h, {}};
        cel.image.pixels.resize(w * h * 4);
        document.stack.cels.push_back(std::move(cel));
    }
    const auto pixel = [&](int cel, int x, int y, std::uint32_t rgba) {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return;
        std::memcpy(document.stack.cels[cel].image.pixels.data() + (y * w + x) * 4, &rgba, 4);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            pixel(0, x, y,
                  0xff000000u | (std::uint32_t(15 + y / 8) << 16) | (std::uint32_t(20 + y / 12) << 8) |
                      std::uint32_t(48 + y / 5));
    for (int y = 0; y < 160; ++y)
        for (int x = 0; x < w; ++x)
            if ((x - 222) * (x - 222) + (y - 76) * (y - 76) < 44 * 44 && (y < 83 || y % 8 < 5))
                pixel(1, x, y, 0xffff0000u | (std::uint32_t(98 + y / 2) << 8) | 70);
    for (int x = 8; x < w - 8; x += 19)
    {
        const int top = 115 + (x * 13 % 57);
        for (int y = top; y < 193; ++y)
            for (int xx = x; xx < x + 15; ++xx)
            {
                pixel(1, xx, y, 0xff172d50);
                if ((xx - x) % 5 == 2 && (y - top) % 9 < 3)
                    pixel(1, xx, y, 0xff3878a2);
            }
    }
    for (int y = 191; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
            if (y == 191 || y % 12 == 0 || std::abs((x - w / 2) * 20) % (std::max(1, (y - 181) * 8)) < 13)
                pixel(2, x, y, 0xb445dddc);
    }
    constexpr wchar_t title[] = L"CELS";
    int x = 20;
    for (wchar_t c : title)
    {
        if (!c)
            break;
        const auto glyph = pixelGlyph(c);
        for (int yy = 0; yy < 7; ++yy)
            for (int xx = 0; xx < 5; ++xx)
                if (glyph[yy] & (1 << (4 - xx)))
                    for (int dy = 0; dy < 5; ++dy)
                        for (int dx = 0; dx < 5; ++dx)
                            pixel(2, x + xx * 5 + dx, 28 + yy * 5 + dy, 0xfff5df9a);
        x += 30;
    }
    document.stack.activeCel = 2;
    if (!saveSimpDocument(argv[1], document))
        return 1;
    SimpDocument loaded;
    if (!loadSimpDocument(argv[1], loaded) || !loaded.hasCels || loaded.stack.cels.size() != 3)
        return 1;
    std::cout << "Created " << argv[1] << ": 320x240, three editable cels.\n";
}
