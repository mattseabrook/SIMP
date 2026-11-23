#include "color_inventory.h"
#include "cpu_work.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <span>

namespace
{
std::vector<PaletteColor> collectImages(std::span<const DecodedImage> images)
{
    if (images.empty())
        return {};
    for (const auto &current : images)
        if (current.pixels.size() != std::size_t(current.width) * current.height * 4)
            throw std::runtime_error("Invalid image storage for color analysis.");
    const auto &image = images.front();
    const auto count = std::size_t(image.width) * image.height;
    if (image.pixels.size() != count * 4)
        throw std::runtime_error("Invalid image storage for color analysis.");
    if (!count && images.size() == 1)
        return {};
    if (images.size() == 1 && count <= 4096)
    {
        // Icons and tiny canvases need neither a two-MiB bitset nor pool jobs.
        std::vector<std::uint32_t> rgb(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            std::memcpy(&rgb[i], image.pixels.data() + i * 4, 4);
            rgb[i] &= 0xffffff;
        }
        std::sort(rgb.begin(), rgb.end());
        rgb.erase(std::unique(rgb.begin(), rgb.end()), rgb.end());
        std::vector<PaletteColor> colors;
        colors.reserve(rgb.size());
        for (const auto value : rgb)
            colors.push_back({std::uint8_t(value >> 16), std::uint8_t(value >> 8), std::uint8_t(value)});
        return colors;
    }
    constexpr std::size_t words = (1u << 24) / 64;
    // Two MiB, independent of image size; never allocate a hash node per pixel.
    auto seen = std::make_unique<std::atomic<std::uint64_t>[]>(words);
    for (const auto &current : images)
    {
        const auto *pixels = current.pixels.data();
        parallelCpuRange(std::size_t(current.width) * current.height, 262144, [&](std::size_t begin, std::size_t end) {
            std::uint32_t previous = 0xffffffff;
            for (auto i = begin; i < end; ++i)
            {
                std::uint32_t rgba;
                std::memcpy(&rgba, pixels + i * 4, 4);
                const auto rgb = rgba & 0xffffff;
                if (rgb == previous)
                    continue;
                previous = rgb;
                auto &word = seen[rgb >> 6];
                const auto bit = std::uint64_t(1) << (rgb & 63);
                if (!(word.load(std::memory_order_relaxed) & bit))
                    word.fetch_or(bit, std::memory_order_relaxed);
            }
        });
    }
    constexpr std::size_t groups = 32;
    std::array<std::size_t, groups + 1> offsets{};
    parallelCpuRange(groups, 1, [&](std::size_t begin, std::size_t end) {
        for (auto group = begin; group < end; ++group)
            for (auto i = group * (words / groups); i < (group + 1) * (words / groups); ++i)
                offsets[group + 1] += std::popcount(seen[i].load(std::memory_order_relaxed));
    });
    for (std::size_t i = 1; i <= groups; ++i)
        offsets[i] += offsets[i - 1];
    std::vector<PaletteColor> colors(offsets.back());
    parallelCpuRange(groups, 1, [&](std::size_t begin, std::size_t end) {
        for (auto group = begin; group < end; ++group)
        {
            auto out = offsets[group];
            for (auto i = group * (words / groups); i < (group + 1) * (words / groups); ++i)
            {
                auto bits = seen[i].load(std::memory_order_relaxed);
                while (bits)
                {
                    const auto rgb = std::uint32_t(i * 64 + std::countr_zero(bits));
                    colors[out++] = {std::uint8_t(rgb >> 16), std::uint8_t(rgb >> 8), std::uint8_t(rgb)};
                    bits &= bits - 1;
                }
            }
        }
    });
    return colors;
}

} // namespace
std::vector<PaletteColor> collectImageColors(const DecodedImage &image)
{
    return collectImages({&image, 1});
}
std::vector<PaletteColor> collectCelColors(const CelStack &stack)
{
    std::vector<DecodedImage> images;
    images.reserve(stack.cels.size());
    for (const auto &cel : stack.cels)
        images.push_back(cel.image);
    return collectImages(images);
}
