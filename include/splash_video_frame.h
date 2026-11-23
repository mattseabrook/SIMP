#pragma once
#include "image.h"
#include <cstring>
#include <span>

// RGB32 media buffers can have row padding and either orientation. Validate
// the complete footprint before reading, then normalize to top-down opaque BGRA.
inline bool copySplashVideoFrame(std::span<const std::uint8_t> storage, std::size_t firstRow,
                                 std::int32_t stride, std::uint32_t width, std::uint32_t height,
                                 DecodedImage &image)
{
    if (!width || !height || width > 16384 || height > 16384 || std::uint64_t(width) * height > 64000000)
        return false;
    const auto rowBytes = std::size_t(width) * 4;
    const auto pitch = stride < 0 ? -std::int64_t(stride) : std::int64_t(stride);
    const auto travel = std::uint64_t(pitch) * (height - 1);
    if (pitch < std::int64_t(rowBytes) || firstRow > storage.size() ||
        rowBytes > storage.size() - firstRow ||
        (stride < 0 ? travel > firstRow : travel > storage.size() - firstRow - rowBytes))
        return false;
    image.width = width;
    image.height = height;
    image.pixels.resize(rowBytes * height);
    auto *out = image.pixels.data();
    for (std::uint32_t y = 0; y < height; ++y)
    {
        const auto delta = std::size_t(y) * std::size_t(pitch);
        auto *row = out + y * rowBytes;
        std::memcpy(row, storage.data() + (stride < 0 ? firstRow - delta : firstRow + delta), rowBytes);
        for (std::uint32_t x = 0; x < width; ++x) row[x * 4 + 3] = 255;
    }
    return true;
}
