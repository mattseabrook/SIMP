#pragma once
#include "image.h"
#include "cels.h"
#include <vector>

struct PaletteColor
{
    std::uint8_t red = 0, green = 0, blue = 0;
    bool operator==(const PaletteColor &) const = default;
};
// Every RGB value is counted, including RGB stored under transparent pixels.
// Alpha is preserved in the image but does not create duplicate paint colors.
std::vector<PaletteColor> collectImageColors(const DecodedImage &image);
std::vector<PaletteColor> collectCelColors(const CelStack &);
