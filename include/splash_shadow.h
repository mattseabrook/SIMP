#pragma once
#include "image.h"
#include <span>

// Premultiplied black shadow, generated once from combined text/logo coverage.
PixelBuffer makeSplashShadow(std::span<const std::uint8_t> coverage, int width, int height);
