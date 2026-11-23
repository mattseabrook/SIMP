#include "editor_ui.h"
#include "editor_theme.h"
#include "menu_commands.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>

CanvasRenderData frame(CanvasState &state, int w, int h, double scale, int units, EditorUi *&out)
{
    const auto layout = computeCanvasLayout(state, w, h, scale, 0, 0);
    auto *ui = new EditorUi{state, layout, w, h, units, scale, "GPU NAME SHOWN HERE", {}};
    const int ruler = state.showRulers ? int(std::round(kRulerThickness * scale)) : 0;
    CanvasRenderData data;
    data.viewport = {layout.leftInset + ruler, layout.topInset + ruler, w - layout.rightInset,
                     h - int(std::round(kStatusHeight * scale))};
    ui->build(data);
    out = ui;
    return data;
}
void preview(const char *path, const CanvasState &state, const EditorUi &ui, const CanvasRenderData &data)
{
    const int w = ui.surfaceWidth, h = ui.surfaceHeight;
    std::vector<std::uint8_t> pixels(w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const auto i = (y * w + x) * 3;
            const auto &l = ui.layout;
            const bool canvas = data.viewport.contains(x, y) && x >= l.originX && y >= l.originY &&
                                x < l.originX + state.canvasWidthIn * l.pxPerIn &&
                                y < l.originY + state.canvasHeightIn * l.pxPerIn;
            pixels[i] = canvas ? 255 : (EditorTheme::kWorkspace & 255);
            pixels[i + 1] = canvas ? 255 : ((EditorTheme::kWorkspace >> 8) & 255);
            pixels[i + 2] = canvas ? 255 : ((EditorTheme::kWorkspace >> 16) & 255);
        }
    for (const auto &r : data.overlays)
        for (int y = int(r.y); y < int(r.y + r.height); ++y)
            for (int x = int(r.x); x < int(r.x + r.width); ++x)
            {
                const auto i = (y * w + x) * 3;
                const float alpha = r.alpha;
                pixels[i] = std::uint8_t(std::round(r.red * 255 * alpha + pixels[i] * (1 - alpha)));
                pixels[i + 1] = std::uint8_t(std::round(r.green * 255 * alpha + pixels[i + 1] * (1 - alpha)));
                pixels[i + 2] = std::uint8_t(std::round(r.blue * 255 * alpha + pixels[i + 2] * (1 - alpha)));
            }
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << " " << h << "\n255\n";
    out.write(reinterpret_cast<const char *>(pixels.data()), pixels.size());
}
int main(int argc, char **argv)
{
    CanvasState measured;
    measured.image() = {100, 200, std::vector<std::uint8_t>(100 * 200 * 4, 42)};
    measured.imagePpi = 100;
    measured.canvasWidthIn = 1;
    measured.canvasHeightIn = 2;
    measured.gridSizeIn = 0.1;
    const auto originalPixels = measured.image().pixels;
    assert(rulerUnitsPerInch(0, 100) == 100 && rulerUnitsPerInch(1, 100) == 1 && rulerUnitsPerInch(2, 100) == 2.54);
    assert(changeImagePpi(measured, 200));
    assert(measured.image().width == 100 && measured.image().height == 200 && measured.image().pixels == originalPixels);
    assert(measured.imageWidthIn == 0.5 && measured.imageHeightIn == 1 &&
           measured.gridSizeIn * measured.imagePpi == 10);
    assert(!changeImagePpi(measured, 0) && !changeImagePpi(measured, 5001) && measured.imagePpi == 200);
    CanvasState state;
    state.imageName = L"Untitled";
    state.image() = {2975, 3950, {}};
    state.imageWidthIn = state.canvasWidthIn;
    state.imageHeightIn = state.canvasHeightIn;
    for (int i = 0; i < 64; ++i)
        state.palette.push_back({std::uint8_t(i * 31), std::uint8_t(i * 59), std::uint8_t(i * 97)});
    state.lines = {{500, 3, 0x000000ff, true}, {900, 2, 0x00ff6600, false}, {-1000000000, 256, 0, false}};
    for (const auto size : {std::pair{1280, 720}, std::pair{1740, 937}, std::pair{3200, 1800}})
        for (double scale : {1.0, 1.25, 1.5, 2.0})
            for (int units = 0; units < 3; ++units)
                for (bool rulers : {true, false})
                    for (double zoom : {1.0, 128.0})
                    {
                        scale = std::min({scale, size.first / 1280.0, size.second / 720.0});
                        state.showRulers = rulers;
                        state.zoomScale = zoom;
                        EditorUi *ui = nullptr;
                        auto data = frame(state, size.first, size.second, scale, units, ui);
                        assert(data.viewport.right > data.viewport.left && data.viewport.bottom > data.viewport.top);
                        assert(data.overlays.size() < 50000); // Work is bounded by screen size, even at 1-pixel grids.
                        for (const auto &r : data.overlays)
                            assert(std::isfinite(r.x) && std::isfinite(r.y) && r.x >= 0 && r.y >= 0 && r.width > 0 &&
                                   r.height > 0 && r.x + r.width <= size.first && r.y + r.height <= size.second);
                        for (size_t i = 0; i < ui->hits.size(); ++i)
                        {
                            const auto &a = ui->hits[i].rect;
                            assert(a.left >= 0 && a.top >= 0 && a.right <= size.first && a.bottom <= size.second);
                            for (size_t j = i + 1; j < ui->hits.size(); ++j)
                            {
                                const auto &b = ui->hits[j].rect;
                                assert(a.left >= b.right || a.right <= b.left || a.top >= b.bottom ||
                                       a.bottom <= b.top);
                            }
                        }
                        const auto button = std::find_if(ui->hits.begin(), ui->hits.end(), [](const UiHit &hit) {
                            return hit.action == IDM_OPTIONS_IMAGE;
                        });
                        assert(button == ui->hits.end());
                        // Measure the actual emitted label pixels, including + and -.
                        for (const auto action :
                             std::array<int, 7>{UI_ZOOM_OUT, UI_ZOOM_IN, UI_ORIGINAL, UI_FIT, int(IDM_VIEW_GRID),
                                                int(IDM_VIEW_RULERS), int(IDM_VIEW_LINES)})
                        {
                            const auto hit = std::find_if(ui->hits.begin(), ui->hits.end(),
                                                          [&](const auto &h) { return h.action == action; });
                            assert(hit != ui->hits.end());
                            const auto &box = hit->rect;
                            double left = box.right, top = box.bottom, right = box.left, bottom = box.top;
                            for (const auto &r : data.overlays)
                                if (r.x >= box.left && r.y >= box.top && r.x + r.width <= box.right &&
                                    r.y + r.height <= box.bottom &&
                                    (r.red == float(EditorTheme::kText & 255) / 255 ||
                                     r.red == float(EditorTheme::kAccent & 255) / 255))
                                {
                                    left = std::min(left, double(r.x));
                                    right = std::max(right, double(r.x + r.width));
                                    top = std::min(top, double(r.y));
                                    bottom = std::max(bottom, double(r.y + r.height));
                                }
                            assert(right > left && bottom > top);
                            assert(std::abs(left + right - box.left - box.right) <= 1.01);
                            assert(std::abs(top + bottom - box.top - box.bottom) <= 1.01);
                        }
                        delete ui;
                    }
    state.showRulers = true;
    state.showLines = true;
    state.zoomScale = 1;
    state.lines = {{500, 3, 0x000000ff, true}, {900, 2, 0x00ff6600, false}};
    {
        const auto l = computeCanvasLayout(state, 1740, 937, 1, 0, 0);
        const int x = int(std::round(l.originX + 500 / state.imagePpi * l.pxPerIn));
        const int y = int(std::round(l.originY + 900 / state.imagePpi * l.pxPerIn));
        assert(rulerAxis(l, 1740, 937, 1, true, x, l.topInset + 8) == 0);
        assert(rulerAxis(l, 1740, 937, 1, true, l.leftInset + 8, y) == 1);
        assert(rulerAxis(l, 1740, 937, 1, true, x, y) == -1);
        assert(rulerAxis(l, 1740, 937, 1, false, x, l.topInset + 8) == -1);
        assert(hitGuide(state, l, 0, x, 6) == 0 && hitGuide(state, l, 1, y, 6) == 1);
        assert(hitGuide(state, l, 0, x + 20, 6) == -1);
        EditorUi *chrome = nullptr;
        const auto visible = frame(state, 1740, 937, 1, 0, chrome);
        const auto oldCount = visible.overlays.size();
        const auto rulerGlow = [&](const auto &r) {
            return r.alpha < 1 && ((r.y >= l.topInset && r.y + r.height <= l.topInset + kRulerThickness) ||
                                   (r.x >= l.leftInset && r.x + r.width <= l.leftInset + kRulerThickness));
        };
        assert(std::none_of(visible.overlays.begin(), visible.overlays.end(), rulerGlow));
        delete chrome;
        state.highlightedLine = 0;
        const auto highlighted = frame(state, 1740, 937, 1, 0, chrome);
        int transparent = 0;
        for (const auto &r : highlighted.overlays)
            if (rulerGlow(r))
                ++transparent;
        assert(transparent > 0);
        delete chrome;
        state.showLines = false;
        const auto hidden = frame(state, 1740, 937, 1, 0, chrome);
        assert(hitGuide(state, l, 0, x, 6) == -1 && hidden.overlays.size() < oldCount);
        assert(std::none_of(hidden.overlays.begin(), hidden.overlays.end(), rulerGlow));
        delete chrome;
        state.showLines = true;
    }
    state.showRulers = true;
    state.zoomScale = 1;
    state.palette.resize(32);
    state.highlightedLine = 0;
    if (argc == 2)
    {
        EditorUi *ui = nullptr;
        auto data = frame(state, 1740, 937, 1, 0, ui);
        preview(argv[1], state, *ui, data);
        delete ui;
    }
    for (const auto size : {std::pair{1, 1}, std::pair{2975, 3950}, std::pair{16384, 3906}})
    {
        CanvasState actual;
        actual.imagePpi = 100;
        actual.image().width = size.first;
        actual.image().height = size.second;
        actual.imageWidthIn = actual.canvasWidthIn = double(size.first) / 100;
        actual.imageHeightIn = actual.canvasHeightIn = double(size.second) / 100;
        const auto fit = computeCanvasLayout(actual, 1280, 720, 1, 0, 0);
        actual.zoomScale = originalZoomScale(actual, fit);
        const auto one = computeCanvasLayout(actual, 1280, 720, 1, 0, 0);
        assert(std::abs(one.pxPerIn / 100 - 1) < 1e-12);
    }
    const auto packed = layoutPalette(1280, 1, 1000000);
    assert(packed.capacity() < 1000000 && clampPaletteOffset(packed, 1000000, 1000000) > 0);
    // The active picker alone uses its exact color for the shared Gaussian glow.
    state.paintColor = {221, 37, 61};
    state.secondaryColor = {31, 177, 223};
    for (const bool secondary : {false, true})
    {
        state.secondaryActive = secondary;
        EditorUi *picker = nullptr;
        const auto data = frame(state, 1280, 720, 1, 0, picker);
        bool sawGlow = false;
        for (const auto &r : data.overlays)
            if (r.alpha < 1 && r.x >= 1280 - kRightPanelWidth)
            {
                const auto expected = activePaintColor(state);
                assert(r.red == float(expected.red) / 255 && r.green == float(expected.green) / 255 &&
                       r.blue == float(expected.blue) / 255);
                sawGlow = true;
            }
        assert(sawGlow);
        delete picker;
    }
    // Tiny physical grids must not produce a geometry explosion at fit view.
    state.gridSizeIn = 0.0002;
    state.imagePpi = 5000;
    EditorUi *ui = nullptr;
    auto data = frame(state, 1280, 720, 1, 0, ui);
    assert(data.overlays.size() < 50000);
    delete ui;
    std::cout << "Workspace bounds, hit targets, all ruler units, DPI scales, resize sizes, and zoom limits passed.\n";
}
