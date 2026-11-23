#include "splash_shadow.h"
#include "splash_video_frame.h"
#include <algorithm>
#include <cassert>
#include <iostream>

int main()
{
    std::vector<std::uint8_t> rows(32, 37);
    for (int i = 0; i < 3; ++i)
    {
        rows[i * 4] = 10;
        rows[16 + i * 4] = 20;
    }
    const auto sourceRows = rows;
    DecodedImage frame;
    assert(copySplashVideoFrame(rows, 0, 16, 3, 2, frame));
    assert(frame.pixels.size() == 24 && frame.pixels[0] == 10 && frame.pixels[12] == 20);
    assert(copySplashVideoFrame(rows, 16, -16, 3, 2, frame));
    assert(frame.pixels[0] == 20 && frame.pixels[12] == 10);
    for (int i = 0; i < 6; ++i) assert(frame.pixels[i * 4 + 3] == 255);
    assert(rows == sourceRows);
    assert(!copySplashVideoFrame(rows, 0, -16, 3, 2, frame));
    assert(!copySplashVideoFrame(rows, 20, 16, 3, 2, frame));
    assert(!copySplashVideoFrame(rows, 0, 8, 3, 2, frame));
    assert(!copySplashVideoFrame(rows, 33, 16, 3, 2, frame));
    assert(!copySplashVideoFrame(rows, 0, INT32_MIN, 3, 2, frame));
    assert(!copySplashVideoFrame(rows, 0, 16, 0, 2, frame));
    constexpr int width = 320, height = 200;
    std::vector<std::uint8_t> coverage(width * height);
    for (int y = 60; y < 140; ++y)
        std::fill_n(coverage.begin() + y * width + 80, 160, 255);
    const auto original = coverage;
    const auto shadow = makeSplashShadow(coverage, width, height);
    assert(coverage == original && shadow.size() == coverage.size() * 4);
    const auto alpha = [&](int x, int y) { return shadow[(y * width + x) * 4 + 3]; };
    unsigned peak = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const auto offset = (y * width + x) * 4;
            assert(!shadow[offset] && !shadow[offset + 1] && !shadow[offset + 2]);
            peak = std::max(peak, unsigned(alpha(x, y)));
            if (!x || !y || x == width - 1 || y == height - 1)
                assert(alpha(x, y) == 0); // Padded artwork fades out before the bitmap edge.
        }
    assert(peak > 120 && peak <= 145); // Softened opacity even under dense logos.
    assert(alpha(265, 100) > 0 && alpha(300, 100) == 0); // Broad, uncut tail.
    for (int x = 244; x < 300; ++x)
    {
        assert(alpha(x + 1, 100) <= alpha(x, 100));
        assert(alpha(x, 100) - alpha(x + 1, 100) <= 8);
    }
    bool rejected = false;
    try { makeSplashShadow(coverage, width + 1, height); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    std::cout << "Splash video row orientation/padding/bounds and shadow opacity/extended falloff passed.\n";
}
