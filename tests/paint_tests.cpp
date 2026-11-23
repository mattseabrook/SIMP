#include "paint_tools.h"
#include "cpu_pixels.h"
#include "editor_ui.h"
#include "image_history.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

int main()
{
    constexpr int width = 73, height = 67;
    for (int size : {1, 2, 3, 16, 1024})
        for (const auto start : {std::pair{0, 0}, std::pair{36, 33}, std::pair{72, 66}})
            for (const auto end :
                 {std::pair{0, 0}, std::pair{72, 0}, std::pair{0, 66}, std::pair{72, 66}, std::pair{71, 8}})
            {
                std::vector<std::uint8_t> reference(width * height), merged(reference);
                const auto fill = [&](auto &pixels, PaintRegion r) {
                    assert(r.left >= 0 && r.top >= 0 && r.right <= width && r.bottom <= height);
                    for (int y = r.top; y < r.bottom; ++y)
                        std::fill(pixels.begin() + y * width + r.left, pixels.begin() + y * width + r.right, 1);
                };
                const auto stamp = [&](int x, int y) { fill(reference, brushRegion(x, y, size, width, height)); };
                stamp(start.first, start.second);
                continuePaintLine(start.first, start.second, end.first, end.second, stamp);
                fill(merged, brushRegion(start.first, start.second, size, width, height));
                continueBrushLine(start.first, start.second, end.first, end.second, size, width, height,
                                  [&](auto region) { fill(merged, region); });
                assert(merged == reference);
                if (size == 1)
                    assert(std::count(reference.begin(), reference.end(), 1) ==
                           std::max(std::abs(end.first - start.first), std::abs(end.second - start.second)) + 1);
            }
    int dispatches = 0;
    continueBrushLine(2, 4, 70, 4, 1, width, height, [&](auto) { ++dispatches; });
    assert(dispatches == 1); // One fill for a long horizontal stroke, not 68 dispatches.
    dispatches = 0;
    continueBrushLine(4, 2, 4, 65, 8, width, height, [&](auto) { ++dispatches; });
    assert(dispatches == 1);

    std::size_t written = 0;
    continueBrushLine(10, 10, 60, 60, 16, width, height,
                      [&](auto r) { written += std::size_t(r.right - r.left) * (r.bottom - r.top); });
    assert(written <= 16 * 16 + 50 * 32); // Large diagonal stamps touch each newly exposed strip once.

    CanvasState state;
    state.image() = {width, height, std::vector<std::uint8_t>(width * height * 4, 255)};
    state.paintColor = {12, 34, 56};
    state.secondaryColor = {220, 170, 90};
    ImageHistory history;
    const auto initial = state.image().pixels;
    const auto paint = [&](PaintRegion r) {
        history.capture(state.image(), r.left, r.top, r.right, r.bottom);
        const auto ink = activePaintColor(state);
        const std::uint32_t bgra =
            std::uint32_t(ink.blue) | (std::uint32_t(ink.green) << 8) | (std::uint32_t(ink.red) << 16) | 0xff000000;
        auto *data = state.image().pixels.data();
        for (int y = r.top; y < r.bottom; ++y)
            fillPixelSpan(data + (y * width + r.left) * 4, r.right - r.left, bgra);
    };
    paint(brushRegion(36, 33, 1, width, height));
    history.commit();
    const auto first = state.image().pixels;
    std::size_t changed = 0;
    for (std::size_t i = 0; i < first.size(); i += 4)
        if (first[i] != 255 || first[i + 1] != 255 || first[i + 2] != 255)
            ++changed;
    assert(changed == 1);
    state.secondaryActive = true;
    continueBrushLine(36, 33, 72, 66, 7, width, height, paint);
    history.commit();
    const auto second = state.image().pixels;
    const auto edge = (66 * width + 72) * 4;
    assert(second[edge] == state.secondaryColor.blue && second[edge + 2] == state.secondaryColor.red);
    assert(state.paintColor == (PaletteColor{12, 34, 56}));
    assert(history.undo(state.image(), state.lines) && state.image().pixels == first);
    assert(history.undo(state.image(), state.lines) && state.image().pixels == initial);
    assert(history.redo(state.image(), state.lines) && state.image().pixels == first);
    assert(history.redo(state.image(), state.lines) && state.image().pixels == second);
    std::cout << "One-pixel/resized brush coverage, clipping, connected strokes, dispatch merging, secondary color and "
                 "undo/redo passed.\n";
}
