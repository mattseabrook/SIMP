#include "cpu_pixels.h"
#include "cpu_work.h"
#include "color_inventory.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <future>
#include <iostream>
#include <random>
#include <set>

int main()
{
    std::mt19937 random(731);
    for (std::size_t n = 0; n < 1025; ++n)
    {
        std::vector<std::uint8_t> input(n * 4 + 32), baseline(input.size(), 71), fast(input.size(), 71),
            reference(input.size(), 71);
        for (auto &value : input)
            value = std::uint8_t(random());
        for (std::size_t i = 0; i < n; ++i)
        {
            reference[3 + i * 4] = input[1 + i * 4 + 2];
            reference[3 + i * 4 + 1] = input[1 + i * 4 + 1];
            reference[3 + i * 4 + 2] = input[1 + i * 4];
            reference[3 + i * 4 + 3] = input[1 + i * 4 + 3];
        }
        convertRgbaToBgra(baseline.data() + 3, input.data() + 1, n, false);
        convertRgbaToBgra(fast.data() + 3, input.data() + 1, n, true);
        assert(baseline == reference && fast == reference);
        baseline = input;
        fast = input;
        swizzleRedBlue(baseline.data() + 1, n, false);
        swizzleRedBlue(fast.data() + 1, n, true);
        assert(baseline == fast);
        std::fill(reference.begin(), reference.end(), 71);
        baseline = reference;
        fast = reference;
        const std::uint32_t color = random();
        for (std::size_t i = 0; i < n; ++i)
            std::memcpy(reference.data() + 3 + i * 4, &color, 4);
        fillPixelSpan(baseline.data() + 3, n, color, false);
        fillPixelSpan(fast.data() + 3, n, color, true);
        assert(baseline == reference && fast == reference);
    }
    for (const auto count : {0, 1, 2, 4096, 4097})
    {
        DecodedImage small{std::uint32_t(count), 1, std::vector<std::uint8_t>(count * 4)};
        std::set<std::uint32_t> expected;
        auto *data = small.pixels.data();
        for (int i = 0; i < count; ++i)
        {
            const std::uint32_t value = (i % 29) | ((i & 255) << 24);
            std::memcpy(data + i * 4, &value, 4);
            expected.insert(value & 0xffffff);
        }
        const auto colors = collectImageColors(small);
        assert(colors.size() == expected.size());
        std::size_t index = 0;
        for (auto rgb : expected)
            assert(colors[index++] ==
                   (PaletteColor{std::uint8_t(rgb >> 16), std::uint8_t(rgb >> 8), std::uint8_t(rgb)}));
    }
    DecodedImage image{1025, 1025, std::vector<std::uint8_t>(1025 * 1025 * 4)};
    auto *pixels = image.pixels.data();
    std::set<std::uint32_t> expected;
    for (std::size_t i = 0; i < std::size_t(image.width) * image.height; ++i)
    {
        const std::uint32_t rgb = std::uint32_t(i % 100003);
        const std::uint32_t rgba = rgb | (std::uint32_t(random() & 255) << 24);
        std::memcpy(pixels + i * 4, &rgba, 4);
        expected.insert(rgb);
    }
    const std::uint32_t last = 0x01febadc;
    std::memcpy(pixels + image.pixels.size() - 4, &last, 4);
    expected.insert(last & 0xffffff);
    const auto colors = collectImageColors(image);
    assert(colors.size() == expected.size());
    std::size_t index = 0;
    for (const auto rgb : expected)
        assert(colors[index++] == (PaletteColor{std::uint8_t(rgb >> 16), std::uint8_t(rgb >> 8), std::uint8_t(rgb)}));
    // A worker reads a snapshot while the window's image detaches and changes.
    const DecodedImage snapshot = image;
    auto promise = std::make_shared<std::promise<std::vector<PaletteColor>>>();
    auto future = promise->get_future();
    enqueueCpuWork([snapshot, promise] { promise->set_value(collectImageColors(snapshot)); });
    fillPixelSpan(image.pixels.data(), std::size_t(image.width) * image.height, 0xff123456);
    assert(future.get() == colors);
    assert(collectImageColors(image) == std::vector<PaletteColor>({{0x12, 0x34, 0x56}}));
    // Nested range jobs must complete even when every pool thread starts one.
    std::atomic<std::uint64_t> sum = 0;
    std::vector<std::future<void>> nested;
    for (unsigned job = 0; job < cpuWorkerCount(); ++job)
    {
        auto done = std::make_shared<std::promise<void>>();
        nested.push_back(done->get_future());
        enqueueCpuWork([done, &sum] {
            parallelCpuRange(20000, 17, [&](auto begin, auto end) {
                for (auto i = begin; i < end; ++i)
                    sum.fetch_add(i, std::memory_order_relaxed);
            });
            done->set_value();
        });
    }
    for (auto &done : nested)
        done.get();
    assert(sum == std::uint64_t(cpuWorkerCount()) * 19999 * 20000 / 2);
    bool propagated = false;
    try
    {
        parallelCpuRange(100, 1, [](auto begin, auto) {
            if (begin == 17)
                throw std::runtime_error("test");
        });
    }
    catch (const std::runtime_error &)
    {
        propagated = true;
    }
    assert(propagated);
    waitForCpuWork();
    std::cout
        << "SIMD equality/tails, exact colors, concurrent immutable snapshots and nested parallel jobs passed (AVX2="
        << cpuHasAvx2() << ").\n";
}
