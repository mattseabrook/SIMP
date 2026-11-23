#pragma once
#include <cmath>
#include <cstdint>

struct GuideLine
{
    double position = 0;              // Image pixels from the canvas origin.
    float thickness = 1;              // Image pixels; display remains at least one screen pixel.
    std::uint32_t color = 0x00d2a000; // Win32 RGB byte order; cyan.
    bool vertical = true;
    bool operator==(const GuideLine &) const = default;
};
inline bool validGuide(const GuideLine &line)
{
    return std::isfinite(line.position) && std::abs(line.position) <= 1000000000.0 && std::isfinite(line.thickness) &&
           line.thickness >= 1 && line.thickness <= 256 && line.color <= 0xffffff;
}
