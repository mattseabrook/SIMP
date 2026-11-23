#include "splash_shadow.h"
#include "cpu_work.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace
{
std::vector<float> gaussian(const std::vector<float> &input, int width, int height, float sigma)
{
    const int radius = int(std::ceil(sigma * 3));
    std::vector<float> weights(radius * 2 + 1);
    float sum = 0;
    for (int i = -radius; i <= radius; ++i)
        sum += weights[i + radius] = std::exp(-float(i * i) / (2 * sigma * sigma));
    for (auto &weight : weights)
        weight /= sum;
    std::vector<float> temporary(input.size()), result(input.size());
    parallelCpuRange(height, 16, [&](auto first, auto last) {
        for (int y = int(first); y < int(last); ++y)
            for (int x = 0; x < width; ++x)
            {
                float value = 0;
                for (int k = std::max(-radius, -x); k <= std::min(radius, width - x - 1); ++k)
                    value += input[std::size_t(y) * width + x + k] * weights[k + radius];
                temporary[std::size_t(y) * width + x] = value;
            }
    });
    parallelCpuRange(height, 16, [&](auto first, auto last) {
        for (int y = int(first); y < int(last); ++y)
        {
            auto *row = result.data() + std::size_t(y) * width;
            // Contiguous inner spans allow SIMD across columns without changing
            // summation order. The kernel runs only during splash initialization.
            for (int k = std::max(-radius, -y); k <= std::min(radius, height - y - 1); ++k)
            {
                const auto *source = temporary.data() + std::size_t(y + k) * width;
                const float weight = weights[k + radius];
                for (int x = 0; x < width; ++x)
                    row[x] += source[x] * weight;
            }
        }
    });
    return result;
}
} // namespace
PixelBuffer makeSplashShadow(std::span<const std::uint8_t> coverage, int width, int height)
{
    if (width < 1 || height < 1 || std::size_t(width) * height != coverage.size())
        throw std::invalid_argument("Invalid splash shadow coverage.");
    // A small spread and low-opacity, wider kernels keep artwork readable
    // without the heavy black outline from the previous shadow.
    std::vector<float> horizontal(coverage.size()), spread(coverage.size());
    parallelCpuRange(height, 16, [&](auto first, auto last) {
        for (int y = std::max(4, int(first)); y < int(last); ++y)
            for (int x = 0; x < width; ++x)
            {
                std::uint8_t alpha = 0;
                // Offset before blurring so the fade remains smooth at window edges.
                for (int k = std::max(0, x - 3); k <= x - 1; ++k)
                    alpha = std::max(alpha, coverage[std::size_t(y - 4) * width + k]);
                horizontal[std::size_t(y) * width + x] = float(alpha) / 255;
            }
    });
    parallelCpuRange(height, 16, [&](auto first, auto last) {
        for (int y = int(first); y < int(last); ++y)
            for (int x = 0; x < width; ++x)
                for (int k = std::max(0, y - 1); k <= std::min(height - 1, y + 1); ++k)
                    spread[std::size_t(y) * width + x] =
                        std::max(spread[std::size_t(y) * width + x], horizontal[std::size_t(k) * width + x]);
    });
    horizontal.clear();
    horizontal.shrink_to_fit();
    const auto broad = gaussian(spread, width, height, 16);
    const auto tight = gaussian(spread, width, height, 6);
    PixelBuffer shadow;
    shadow.resize(coverage.size() * 4);
    auto *pixels = shadow.data();
    parallelCpuRange(height, 16, [&](auto first, auto last) {
        for (int y = int(first); y < int(last); ++y)
            for (int x = 0; x < width; ++x)
            {
                const auto i = std::size_t(y) * width + x;
                const float alpha = 1 - (1 - broad[i] * 0.48f) * (1 - tight[i] * 0.16f);
                pixels[(std::size_t(y) * width + x) * 4 + 3] =
                    std::uint8_t(std::clamp(int(std::round(alpha * 255)), 0, 255));
            }
    });
    return shadow;
}
