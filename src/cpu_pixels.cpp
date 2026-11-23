#include "cpu_pixels.h"
#include <cstring>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include <immintrin.h>
#ifdef _WIN32
#include <intrin.h>
#endif
#pragma clang diagnostic pop

namespace
{
#ifdef _WIN32
__attribute__((target("xsave"))) unsigned long long xcr0()
{
    return _xgetbv(0);
}
#endif
bool detectAvx2()
{
#ifdef _WIN32
    int cpu[4]{};
    __cpuid(cpu, 0);
    if (cpu[0] < 7)
        return false;
    __cpuidex(cpu, 1, 0);
    if ((cpu[2] & ((1 << 27) | (1 << 28))) != ((1 << 27) | (1 << 28)) || (xcr0() & 6) != 6)
        return false;
    __cpuidex(cpu, 7, 0);
    return (cpu[1] & (1 << 5)) != 0;
#else
    return __builtin_cpu_supports("avx2");
#endif
}
void swizzleBaseline(std::uint8_t *bytes, const std::uint8_t *source, std::size_t count)
{
    const __m128i keep = _mm_set1_epi32(int(0xff00ff00u));
    const __m128i rb = _mm_set1_epi32(0x00ff00ff);
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4)
    {
        const auto v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source + i * 4));
        const auto channels = _mm_and_si128(v, rb);
        const auto swapped = _mm_or_si128(_mm_slli_epi32(channels, 16), _mm_srli_epi32(channels, 16));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(bytes + i * 4), _mm_or_si128(_mm_and_si128(v, keep), swapped));
    }
    for (; i < count; ++i)
    {
        const auto r = source[i * 4], g = source[i * 4 + 1], b = source[i * 4 + 2], a = source[i * 4 + 3];
        bytes[i * 4] = b;
        bytes[i * 4 + 1] = g;
        bytes[i * 4 + 2] = r;
        bytes[i * 4 + 3] = a;
    }
}
__attribute__((target("avx2"))) void swizzleAvx2(std::uint8_t *bytes, const std::uint8_t *source, std::size_t count)
{
    const auto shuffle = _mm256_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15, 2, 1, 0, 3, 6, 5, 4, 7,
                                          10, 9, 8, 11, 14, 13, 12, 15);
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8)
    {
        const auto value = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(source + i * 4));
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(bytes + i * 4), _mm256_shuffle_epi8(value, shuffle));
    }
    swizzleBaseline(bytes + i * 4, source + i * 4, count - i);
}
void fillBaseline(std::uint8_t *bytes, std::size_t count, std::uint32_t bgra)
{
    const auto value = _mm_set1_epi32(int(bgra));
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4)
        _mm_storeu_si128(reinterpret_cast<__m128i *>(bytes + i * 4), value);
    for (; i < count; ++i)
        std::memcpy(bytes + i * 4, &bgra, 4);
}
__attribute__((target("avx2"))) void fillAvx2(std::uint8_t *bytes, std::size_t count, std::uint32_t bgra)
{
    const auto value = _mm256_set1_epi32(int(bgra));
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8)
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(bytes + i * 4), value);
    fillBaseline(bytes + i * 4, count - i, bgra);
}
} // namespace
bool cpuHasAvx2()
{
    static const bool supported = detectAvx2();
    return supported;
}
void swizzleRedBlue(std::uint8_t *bytes, std::size_t pixels, bool accelerated)
{
    (accelerated && cpuHasAvx2() ? swizzleAvx2 : swizzleBaseline)(bytes, bytes, pixels);
}
void fillPixelSpan(std::uint8_t *bytes, std::size_t pixels, std::uint32_t bgra, bool accelerated)
{
    (accelerated && cpuHasAvx2() ? fillAvx2 : fillBaseline)(bytes, pixels, bgra);
}

void convertRgbaToBgra(std::uint8_t *destination, const std::uint8_t *source, std::size_t pixels, bool accelerated)
{
    (accelerated && cpuHasAvx2() ? swizzleAvx2 : swizzleBaseline)(destination, source, pixels);
}
