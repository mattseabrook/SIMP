#pragma once
#include "image.h"
#include <string>
#include <array>
#include <vector>

inline constexpr std::size_t kMaxCels = 1024;
inline constexpr std::uint64_t kMaxCelPixels = 268435456; // Aggregate document allocation guard (one GiB of pixels).
struct Cel
{
    std::uint64_t id = 1;
    std::wstring name = L"Cel 1";
    bool visible = true;
    DecodedImage image;
};
struct CelStack
{
    std::vector<Cel> cels{Cel{}}; // Bottom to top.
    std::uint32_t activeCel = 0;
    DecodedImage &image()
    {
        return cels.at(activeCel).image;
    }
    const DecodedImage &image() const
    {
        return cels.at(activeCel).image;
    }
};
bool validCelStack(const CelStack &);
DecodedImage compositeCels(const CelStack &);
std::array<std::uint8_t, 4> sampleCels(const CelStack &, std::uint32_t, std::uint32_t);
