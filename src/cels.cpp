#include "cels.h"
#include "cpu_work.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_set>

namespace
{
bool validName(const std::wstring &name)
{
    bool high = false;
    for (auto value : name)
    {
        const auto c = std::uint32_t(value);
        if (!c || c > 0x10ffff)
            return false;
        if constexpr (sizeof(wchar_t) == 2)
        {
            if (c >= 0xd800 && c <= 0xdbff)
            {
                if (high)
                    return false;
                high = true;
            }
            else if (c >= 0xdc00 && c <= 0xdfff)
            {
                if (!high)
                    return false;
                high = false;
            }
            else if (high)
                return false;
        }
        else if (c >= 0xd800 && c <= 0xdfff)
            return false;
    }
    return !high;
}
// Keep the probe and exported composite on the same straight-alpha math.
struct Blend
{
    std::array<unsigned, 3> channels{};
    unsigned alpha = 0;

    void add(const std::uint8_t *pixel)
    {
        const unsigned a = pixel[3], inverse = 255 - a;
        if (!a)
            return;
        for (int c = 0; c < 3; ++c)
            channels[c] = pixel[c] * a + (channels[c] * inverse + 127) / 255;
        alpha = a + (alpha * inverse + 127) / 255;
    }

    void store(std::uint8_t *out) const
    {
        for (int c = 0; c < 3; ++c)
            out[c] = alpha ? std::uint8_t(std::min(255u, (channels[c] + alpha / 2) / alpha)) : 0;
        out[3] = std::uint8_t(alpha);
    }
};
} // namespace
bool validCelStack(const CelStack &stack)
{
    if (stack.cels.empty() || stack.cels.size() > kMaxCels || stack.activeCel >= stack.cels.size())
        return false;
    std::unordered_set<std::uint64_t> ids;
    const auto width = stack.cels.front().image.width, height = stack.cels.front().image.height;
    if (!width || !height || width > 16384 || height > 16384 || std::uint64_t(width) * height > 64000000 ||
        std::uint64_t(width) * height * stack.cels.size() > kMaxCelPixels)
        return false;
    for (const auto &cel : stack.cels)
        if (!cel.id || !ids.insert(cel.id).second || cel.name.empty() || cel.name.size() > 32768 ||
            !validName(cel.name) || cel.image.width != width || cel.image.height != height ||
            cel.image.pixels.size() != std::size_t(width) * height * 4)
            return false;
    return true;
}
DecodedImage compositeCels(const CelStack &stack)
{
    if (!validCelStack(stack))
        throw std::invalid_argument("Invalid cel stack.");
    std::vector<const std::uint8_t *> sources;
    const DecodedImage *only = nullptr;
    sources.reserve(stack.cels.size());
    for (const auto &cel : stack.cels)
        if (cel.visible)
        {
            only = &cel.image;
            sources.push_back(only->pixels.data());
        }
    if (sources.size() == 1)
        return *only; // Preserve even RGB stored under zero alpha.

    const auto &base = stack.cels.front().image;
    DecodedImage result{base.width, base.height, {}};
    result.pixels.resize(base.pixels.size());
    if (sources.empty())
        return result;

    auto *out = result.pixels.data();
    parallelCpuRange(std::size_t(base.width) * base.height, 65536, [&](auto first, auto last) {
        for (auto i = first * 4; i < last * 4; i += 4)
        {
            Blend blend;
            for (const auto *source : sources)
                blend.add(source + i);
            blend.store(out + i);
        }
    });
    return result;
}

std::array<std::uint8_t, 4> sampleCels(const CelStack &stack, std::uint32_t x, std::uint32_t y)
{
    std::array<std::uint8_t, 4> result{};
    if (stack.cels.empty() || x >= stack.cels.front().image.width || y >= stack.cels.front().image.height)
        return result;
    const auto i = (std::size_t(y) * stack.cels.front().image.width + x) * 4;
    const std::uint8_t *first = nullptr;
    std::size_t visible = 0;
    Blend blend;
    for (const auto &cel : stack.cels)
        if (cel.visible)
        {
            const auto *pixel = cel.image.pixels.data() + i;
            if (++visible == 1)
                first = pixel;
            else
            {
                if (visible == 2)
                    blend.add(first);
                blend.add(pixel);
            }
        }
    if (visible == 1)
        std::copy_n(first, result.size(), result.data());
    else
        blend.store(result.data());
    return result;
}
