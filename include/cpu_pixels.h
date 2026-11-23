#pragma once
#include <cstddef>
#include <cstdint>

bool cpuHasAvx2();
void swizzleRedBlue(std::uint8_t *bytes, std::size_t pixels, bool accelerated = true);
void fillPixelSpan(std::uint8_t *bytes, std::size_t pixels, std::uint32_t bgra, bool accelerated = true);

void convertRgbaToBgra(std::uint8_t *destination, const std::uint8_t *source, std::size_t pixels,
                       bool accelerated = true);
