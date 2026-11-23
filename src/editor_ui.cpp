#include "editor_ui.h"
#include "editor_format.h"
#include "editor_theme.h"
#include "pixel_font.h"
#include "menu_commands.h"
#include <algorithm>
#include <cmath>
using namespace EditorTheme;
int EditorUi::ui(int value) const
{
    return int(std::round(value * scale));
}
CanvasLayout computeCanvasLayout(const CanvasState &state, int width, int height, double scale, double panX,
                                 double panY)
{
    const auto ui = [scale](int value) { return int(std::round(value * scale)); };
    CanvasLayout layout;
    layout.leftInset = ui(kLeftPanelWidth);
    layout.rightInset = ui(kRightPanelWidth);
    layout.topInset = ui(kTopToolbarHeight);
    const int ruler = state.showRulers ? ui(kRulerThickness) : 0;
    const double availableWidth = std::max(1.0, double(width) - layout.leftInset - layout.rightInset - ruler - ui(32));
    const double availableHeight = std::max(1.0, double(height) - layout.topInset - ui(kStatusHeight) - ruler - ui(32));
    layout.contentMaxXIn = std::max(state.canvasWidthIn, state.imageWidthIn);
    layout.contentMaxYIn = std::max(state.canvasHeightIn, state.imageHeightIn);
    layout.fitPxPerIn = std::min(availableWidth / layout.contentMaxXIn, availableHeight / layout.contentMaxYIn);
    layout.pxPerIn = std::max(0.0001, layout.fitPxPerIn * state.zoomScale);
    layout.originX =
        layout.leftInset + ruler + ui(16) + (availableWidth - layout.contentMaxXIn * layout.pxPerIn) / 2 + panX;
    layout.originY =
        layout.topInset + ruler + ui(16) + (availableHeight - layout.contentMaxYIn * layout.pxPerIn) / 2 + panY;
    return layout;
}

PaletteLayout layoutPalette(int width, double scale, std::size_t colors, int height)
{
    const auto ui = [scale](int x) { return int(std::round(x * scale)); };
    PaletteLayout p;
    const int paletteHeight = 104 + int(std::clamp((height / scale - 720) * 0.25, 0.0, 112.0));
    p.bounds = {width - ui(kRightPanelWidth) + ui(12), ui(kTopToolbarHeight + 38), width - ui(12),
                ui(kTopToolbarHeight + 38 + paletteHeight)};
    p.track = {p.bounds.right - ui(9), p.bounds.top + ui(3), p.bounds.right - ui(3), p.bounds.bottom - ui(3)};
    const int w = p.track.left - p.bounds.left - ui(6), h = p.bounds.bottom - p.bounds.top - ui(6);
    for (int size = 16; size >= 3; --size)
    {
        p.cell = ui(size);
        p.gap = ui(size >= 8 ? 2 : 1);
        p.columns = std::max(1, (w + p.gap) / (p.cell + p.gap));
        p.rows = std::max(1, (h + p.gap) / (p.cell + p.gap));
        if (p.capacity() >= colors)
            break;
    }
    return p;
}
CelsLayout layoutCels(int width, int height, double scale)
{
    const auto ui = [scale](int x) { return int(std::round(x * scale)); };
    const auto palette = layoutPalette(width, scale, 0, height);
    CelsLayout result;
    result.headingY = palette.bounds.bottom + ui(38);
    result.toolsY = result.headingY + ui(20);
    result.rowHeight = ui(22);
    const int listHeight = 66 + int(std::clamp((height / scale - 720) * 0.35, 0.0, 120.0));
    const int top = result.toolsY + ui(64);
    result.rows = std::max(1, ui(listHeight) / result.rowHeight);
    result.bounds = {width - ui(kRightPanelWidth) + ui(12), top, width - ui(12), top + result.rows * result.rowHeight};
    result.track = {result.bounds.right - ui(7), result.bounds.top, result.bounds.right, result.bounds.bottom};
    return result;
}
std::size_t clampPaletteOffset(const PaletteLayout &p, std::size_t colors, std::size_t offset)
{
    const auto columns = std::size_t(p.columns);
    const auto rowCount = (colors + columns - 1) / columns;
    const auto maximum = rowCount > std::size_t(p.rows) ? (rowCount - p.rows) * columns : 0;
    return std::min(maximum, offset / columns * columns);
}
double originalZoomScale(const CanvasState &s, const CanvasLayout &l)
{
    return s.imagePpi / l.fitPxPerIn;
}

int rulerAxis(const CanvasLayout &l, int width, int height, double scale, bool visible, int x, int y)
{
    if (!visible)
        return -1;
    const int r = int(std::round(kRulerThickness * scale));
    if (x >= l.leftInset + r && x < width - l.rightInset && y >= l.topInset && y < l.topInset + r)
        return 0;
    if (x >= l.leftInset && x < l.leftInset + r && y >= l.topInset + r &&
        y < height - int(std::round(kStatusHeight * scale)))
        return 1;
    return -1;
}
int hitGuide(const CanvasState &state, const CanvasLayout &l, int axis, int coordinate, double tolerance)
{
    if (axis < 0 || !state.showLines)
        return -1;
    int found = -1;
    double distance = tolerance;
    for (size_t i = 0; i < state.lines.size(); ++i)
    {
        const auto &line = state.lines[i];
        if (line.vertical != (axis == 0))
            continue;
        const double at = (line.vertical ? l.originX : l.originY) + line.position / state.imagePpi * l.pxPerIn;
        const double delta = std::abs(at - coordinate);
        if (delta <= distance)
        {
            distance = delta;
            found = int(i);
        }
    }
    return found;
}

bool changeImagePpi(CanvasState &state, double ppi)
{
    if (!std::isfinite(ppi) || ppi < 1 || ppi > 5000)
        return false;
    const double ratio = state.imagePpi / ppi;
    const double width = state.canvasWidthIn * ratio, height = state.canvasHeightIn * ratio,
                 grid = state.gridSizeIn * ratio;
    if (width < 0.0002 || height < 0.0002 || width > 16384 || height > 16384 || grid > 10000 ||
        std::abs(state.offsetXIn * ratio) > 10000 || std::abs(state.offsetYIn * ratio) > 10000)
        return false;
    state.canvasWidthIn = width;
    state.canvasHeightIn = height;
    state.gridSizeIn = grid;
    state.offsetXIn *= ratio;
    state.offsetYIn *= ratio;
    state.imagePpi = state.detectedPpi = ppi;
    state.ppiSource = L"manual";
    state.imageWidthIn = double(state.image().width) / ppi;
    state.imageHeightIn = double(state.image().height) / ppi;
    return true;
}

void EditorUi::addOverlayRect(CanvasRenderData &data, std::int32_t x, std::int32_t y, std::uint32_t width,
                              std::uint32_t height, std::uint32_t color, std::uint32_t surfaceWidth,
                              std::uint32_t surfaceHeight)
{
    if (width == 0 || height == 0 || surfaceWidth == 0 || surfaceHeight == 0)
        return;

    std::int32_t left = x;
    std::int32_t top = y;
    std::int32_t right = x + static_cast<std::int32_t>(width);
    std::int32_t bottom = y + static_cast<std::int32_t>(height);

    left = std::max<std::int32_t>(0, left);
    top = std::max<std::int32_t>(0, top);
    right = std::min<std::int32_t>(static_cast<std::int32_t>(surfaceWidth), right);
    bottom = std::min<std::int32_t>(static_cast<std::int32_t>(surfaceHeight), bottom);
    if (right <= left || bottom <= top)
        return;

    CanvasOverlayRect rect;
    rect.x = left;
    rect.y = top;
    rect.width = static_cast<std::uint32_t>(right - left);
    rect.height = static_cast<std::uint32_t>(bottom - top);
    rect.red = static_cast<float>((color & 255)) / 255.0f;
    rect.green = static_cast<float>(((color >> 8) & 255)) / 255.0f;
    rect.blue = static_cast<float>(((color >> 16) & 255)) / 255.0f;
    rect.alpha = 1.0f;
    data.overlays.push_back(rect);
}

void EditorUi::addOverlayRectD(CanvasRenderData &data, double x, double y, double width, double height,
                               std::uint32_t color, std::uint32_t surfaceWidth, std::uint32_t surfaceHeight)
{
    addOverlayRect(data, static_cast<std::int32_t>(std::round(x)), static_cast<std::int32_t>(std::round(y)),
                   static_cast<std::uint32_t>(std::max(1.0, std::round(width))),
                   static_cast<std::uint32_t>(std::max(1.0, std::round(height))), color, surfaceWidth, surfaceHeight);
}

void EditorUi::rect(CanvasRenderData &d, double x, double y, double w, double h, std::uint32_t c)
{
    if (w > 0 && h > 0)
        addOverlayRectD(d, x, y, w, h, c, surfaceWidth, surfaceHeight);
}
namespace
{
float glowOpacity(double distance, double sigma)
{
    return float(0.32 * std::exp(-distance * distance / (2 * sigma * sigma)));
}
} // namespace
void EditorUi::text(CanvasRenderData &d, std::wstring_view value, double x, double y, std::uint32_t color, int maxWidth)
{
    const double pixel = kUiFontPixel * scale;
    for (wchar_t c : value)
    {
        if (x + 5 * pixel > maxWidth)
            break;
        const auto glyph = pixelGlyph(c);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5;)
            {
                if (!(glyph[row] & (1 << (4 - col))))
                {
                    ++col;
                    continue;
                }
                const int start = col++;
                while (col < 5 && (glyph[row] & (1 << (4 - col))))
                    ++col;
                const int left = int(std::round(x + start * pixel)), top = int(std::round(y + row * pixel));
                rect(d, left, top, std::round(x + col * pixel) - left, std::round(y + (row + 1) * pixel) - top, color);
            }
        x += 6 * pixel;
    }
}
void EditorUi::textCentered(CanvasRenderData &d, std::wstring_view value, CanvasViewport box, std::uint32_t color)
{
    int left = int(value.size()) * 6, right = 0, top = 7, bottom = 0;
    int at = 0;
    for (auto c : value)
    {
        const auto glyph = pixelGlyph(c);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (glyph[row] & (1 << (4 - col)))
                {
                    left = std::min(left, at + col);
                    right = std::max(right, at + col + 1);
                    top = std::min(top, row);
                    bottom = std::max(bottom, row + 1);
                }
        at += 6;
    }
    const double pixel = kUiFontPixel * scale;
    text(d, value, (double(box.left) + box.right - (left + right) * pixel) / 2,
         (double(box.top) + box.bottom - (top + bottom) * pixel) / 2, color, box.right);
}
void EditorUi::textVertical(CanvasRenderData &d, const std::wstring &value, int x, int y, std::uint32_t color, int maxY)
{
    const double pixel = kUiFontPixel * scale;
    double pen = y;
    for (wchar_t c : value)
    {
        if (pen + 5 * pixel > maxY)
            break;
        const auto glyph = pixelGlyph(c);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5;)
            {
                if (!(glyph[row] & (1 << (4 - col))))
                {
                    ++col;
                    continue;
                }
                const int start = col++;
                while (col < 5 && (glyph[row] & (1 << (4 - col))))
                    ++col;
                const int left = int(std::round(x + (6 - row) * pixel)), top = int(std::round(pen + start * pixel));
                rect(d, left, top, std::round(x + (7 - row) * pixel) - left, std::round(pen + col * pixel) - top,
                     color);
            }
        pen += 6 * pixel;
    }
}
void EditorUi::glow(CanvasRenderData &d, CanvasViewport box, std::uint32_t color, CanvasViewport clip)
{
    const int spread = ui(18);
    const double sigma = std::max(1, ui(6));
    const auto strip = [&](int x, int y, int w, int h, float alpha) {
        const int left = std::max(x, clip.left), top = std::max(y, clip.top);
        const int right = std::min(x + w, clip.right), bottom = std::min(y + h, clip.bottom);
        if (right <= left || bottom <= top)
            return;
        rect(d, left, top, right - left, bottom - top, color);
        d.overlays.back().alpha = alpha;
    };
    for (int distance = spread; distance >= 1; --distance)
    {
        const int left = box.left - distance, top = box.top - distance;
        const int right = box.right + distance, bottom = box.bottom + distance;
        const float alpha = glowOpacity(distance, sigma);
        strip(left, top, right - left, 1, alpha);
        strip(left, bottom - 1, right - left, 1, alpha);
        strip(left, top + 1, 1, bottom - top - 2, alpha);
        strip(right - 1, top + 1, 1, bottom - top - 2, alpha);
    }
}
void EditorUi::bevel(CanvasRenderData &d, int x, int y, int w, int h, bool selected)
{
    rect(d, x, y, w, h, selected ? kPressed : kControl);
    rect(d, x, y, w, ui(1), selected ? kEdgeDark : kEdgeLight);
    rect(d, x, y, ui(1), h, selected ? kEdgeDark : kEdgeLight);
    rect(d, x, y + h - ui(1), w, ui(1), selected ? kEdgeLight : kEdgeDark);
    rect(d, x + w - ui(1), y, ui(1), h, selected ? kEdgeLight : kEdgeDark);
}
void EditorUi::button(CanvasRenderData &d, const wchar_t *label, int x, int y, int w, int action, bool selected,
                      bool enabled)
{
    const int h = ui(26);
    bevel(d, x, y, w, h, selected);
    textCentered(d, label, {x, y, x + w, y + h}, !enabled ? kRulerText : selected ? kAccent : kText);
    if (enabled)
        hits.push_back({{x, y, x + w, y + h}, action});
}
double EditorUi::unitsPerInch() const
{
    return rulerUnitsPerInch(units, state.imagePpi);
}
const wchar_t *EditorUi::unitName() const
{
    return units == 0 ? L"PX" : (units == 2 ? L"CM" : L"IN");
}

void EditorUi::build(CanvasRenderData &d)
{
    const auto &l = layout;
    const int width = surfaceWidth, height = surfaceHeight;
    // Clip the grid to the viewport and decimate lines below four screen pixels.
    const auto area = d.viewport;
    const double right = std::min(double(area.right), l.originX + state.canvasWidthIn * l.pxPerIn);
    const double bottom = std::min(double(area.bottom), l.originY + state.canvasHeightIn * l.pxPerIn);
    const double left = std::max(double(area.left), l.originX), top = std::max(double(area.top), l.originY);
    if (state.showGrid && right > left && bottom > top)
    {
        const double base = state.gridSizeIn * l.pxPerIn;
        const int skip = std::max(1, static_cast<int>(std::ceil(ui(4) / base)));
        const double step = base * skip;
        for (int i = std::max(0, int(std::ceil((left - l.originX) / step))); l.originX + i * step < right; ++i)
            rect(d, l.originX + i * step, top, 1, bottom - top,
                 state.majorEvery > 0 && (i * skip) % state.majorEvery == 0 ? kGridMajor : kGridMinor);
        for (int i = std::max(0, int(std::ceil((top - l.originY) / step))); l.originY + i * step < bottom; ++i)
            rect(d, left, l.originY + i * step, right - left, 1,
                 state.majorEvery > 0 && (i * skip) % state.majorEvery == 0 ? kGridMajor : kGridMinor);
    }
    if (state.showLines)
        for (const auto &line : state.lines)
        {
            const double at = (line.vertical ? l.originX : l.originY) + line.position / state.imagePpi * l.pxPerIn;
            const double thickness = std::max(1.0, double(line.thickness) * l.pxPerIn / state.imagePpi);
            if (line.vertical)
                rect(d, std::max(double(area.left), at - thickness / 2), area.top,
                     std::max(0.0, std::min(double(area.right), at + thickness / 2) -
                                       std::max(double(area.left), at - thickness / 2)),
                     area.bottom - area.top, line.color);
            else
                rect(d, area.left, std::max(double(area.top), at - thickness / 2), area.right - area.left,
                     std::max(0.0, std::min(double(area.bottom), at + thickness / 2) -
                                       std::max(double(area.top), at - thickness / 2)),
                     line.color);
        }
    // UI masks always cover canvas pixels outside the workspace.
    rect(d, 0, 0, width, l.topInset, kPanel);
    rect(d, 0, l.topInset, l.leftInset, height - l.topInset, kPanel);
    rect(d, width - l.rightInset, l.topInset, l.rightInset, height - l.topInset, kPanel);
    rect(d, 0, height - ui(kStatusHeight), width, ui(kStatusHeight), kStatus);
    rect(d, l.leftInset - ui(1), l.topInset, ui(1), height, kEdgeDark);
    rect(d, width - l.rightInset, l.topInset, ui(1), height, kEdgeDark);
    rect(d, 0, l.topInset - ui(1), width, ui(1), kEdgeDark);
    hits.clear();
    button(d, L"-", ui(8), ui(7), ui(30), UI_ZOOM_OUT);
    button(d, L"+", ui(44), ui(7), ui(30), UI_ZOOM_IN);
    button(d, L"ORIGINAL", ui(80), ui(7), ui(92), UI_ORIGINAL, std::abs(l.pxPerIn - state.imagePpi) < 0.0001);
    button(d, L"FIT", ui(178), ui(7), ui(48), UI_FIT);
    button(d, L"GRID", ui(242), ui(7), ui(58), IDM_VIEW_GRID, state.showGrid);
    button(d, L"RULERS", ui(306), ui(7), ui(70), IDM_VIEW_RULERS, state.showRulers);
    button(d, L"LINES", ui(382), ui(7), ui(62), IDM_VIEW_LINES, state.showLines);
    const double pixelZoom = l.pxPerIn / state.imagePpi * 100;
    text(d, formatDouble(pixelZoom) + L"%", ui(460), ui(16), kText, width - l.rightInset - ui(8));
    textCentered(d, L"TOOLS", {0, l.topInset + ui(8), l.leftInset, l.topInset + ui(28)}, kText);
    button(d, L"BRUSH [B]", ui(8), l.topInset + ui(36), ui(96), UI_BRUSH, state.tool == PaintTool::Brush);
    button(d, L"CELL [C]", ui(8), l.topInset + ui(68), ui(96), UI_CELL, state.tool == PaintTool::Cell);
    button(d, L"PICK [I]", ui(8), l.topInset + ui(100), ui(96), UI_PICKER, state.tool == PaintTool::Pick);
    if (state.tool == PaintTool::Brush)
        textCentered(d, formatInt(state.brushSize) + L" PX",
                     {0, l.topInset + ui(140), l.leftInset, l.topInset + ui(160)}, kMutedText);
    const int px = width - l.rightInset + ui(12);
    text(d, L"PALETTE", px, l.topInset + ui(16), kText);
    const auto palette = layoutPalette(width, scale, state.palette.size(), height);
    const auto box = palette.bounds;
    rect(d, box.left, box.top, box.right - box.left, box.bottom - box.top, rgb(0, 0, 0));
    const auto offset = clampPaletteOffset(palette, state.palette.size(), state.paletteOffset);
    for (std::size_t slot = 0; slot < palette.capacity() && offset + slot < state.palette.size(); ++slot)
    {
        const auto &c = state.palette[offset + slot];
        const int x = box.left + ui(3) + int(slot % palette.columns) * (palette.cell + palette.gap);
        const int y = box.top + ui(3) + int(slot / palette.columns) * (palette.cell + palette.gap);
        const bool selected = c == activePaintColor(state);
        rect(d, x - ui(1), y - ui(1), palette.cell + ui(2), palette.cell + ui(2), selected ? kAccent : kSwatchBorder);
        rect(d, x, y, palette.cell, palette.cell, rgb(c.red, c.green, c.blue));
        hits.push_back({{x, y, x + palette.cell, y + palette.cell}, UI_PALETTE_FIRST + int(offset + slot)});
    }
    if (state.palette.size() > palette.capacity())
    {
        const auto track = palette.track;
        const int h = track.bottom - track.top;
        const auto maximum = clampPaletteOffset(palette, state.palette.size(), state.palette.size());
        const double thumb = std::max(double(ui(12)), double(h) * palette.capacity() / state.palette.size());
        const double y = track.top + double(offset) / maximum * (h - thumb);
        rect(d, track.left, track.top, track.right - track.left, h, kScrollTrack);
        rect(d, track.left, y, track.right - track.left, thumb, kScrollThumb);
        hits.push_back({track, UI_PALETTE_SCROLL});
    }
    text(d, state.paletteUpdating ? L"UPDATING..." : formatInt(int(state.palette.size())) + L" COLORS", px,
         box.bottom + ui(12), kText, width - ui(12));
    const auto cels = layoutCels(width, height, scale);
    text(d, L"CELS " + formatInt(int(state.cels.size())), px, cels.headingY, kText);
    const bool add =
        state.hasImage && !state.celBusy && state.cels.size() < kMaxCels &&
        std::uint64_t(state.image().width) * state.image().height * (state.cels.size() + 1) <= kMaxCelPixels;
    button(d, L"NEW", px, cels.toolsY, ui(60), UI_CEL_NEW, false, add);
    button(d, L"DUP", px + ui(70), cels.toolsY, ui(60), UI_CEL_DUPLICATE, false, add);
    button(d, L"DEL", px + ui(140), cels.toolsY, ui(60), UI_CEL_DELETE, false, state.hasImage && state.cels.size() > 1);
    button(d, L"NAME", px, cels.toolsY + ui(30), ui(60), UI_CEL_RENAME, false, state.hasImage);
    button(d, L"UP", px + ui(70), cels.toolsY + ui(30), ui(60), UI_CEL_UP, false,
           state.hasImage && state.activeCel + 1 < state.cels.size());
    button(d, L"DOWN", px + ui(140), cels.toolsY + ui(30), ui(60), UI_CEL_DOWN, false,
           state.hasImage && state.activeCel > 0);
    rect(d, cels.bounds.left, cels.bounds.top, cels.bounds.right - cels.bounds.left,
         cels.bounds.bottom - cels.bounds.top, kPressed);
    const auto maxOffset = state.cels.size() > std::size_t(cels.rows) ? state.cels.size() - cels.rows : 0;
    const auto celOffset = std::min(state.celOffset, maxOffset);
    for (int row = 0; row < cels.rows && celOffset + row < state.cels.size(); ++row)
    {
        const auto index = state.cels.size() - 1 - celOffset - row;
        const auto &cel = state.cels[index];
        const int y = cels.bounds.top + row * cels.rowHeight;
        if (index == state.activeCel)
            rect(d, px, y, cels.track.left - px, cels.rowHeight, kControl);
        textCentered(d, cel.visible ? L"V" : L"-", {px, y, px + ui(20), y + cels.rowHeight},
                     cel.visible ? kAccent : kRulerText);
        text(d, cel.name, px + ui(24), y + ui(6), index == state.activeCel ? kAccent : kText, cels.track.left - ui(4));
        hits.push_back({{px, y, px + ui(20), y + cels.rowHeight}, UI_CEL_VISIBLE_FIRST + int(index)});
        hits.push_back({{px + ui(20), y, cels.track.left, y + cels.rowHeight}, UI_CEL_SELECT_FIRST + int(index)});
    }
    if (maxOffset)
    {
        const int h = cels.bounds.bottom - cels.bounds.top;
        const double thumb = std::max(double(ui(12)), double(h) * cels.rows / state.cels.size());
        rect(d, cels.track.left, cels.track.top, cels.track.right - cels.track.left, h, kScrollTrack);
        rect(d, cels.track.left, cels.track.top + double(celOffset) / maxOffset * (h - thumb),
             cels.track.right - cels.track.left, thumb, kScrollThumb);
        hits.push_back({cels.track, UI_CEL_SCROLL});
    }
    const int pickerY = cels.bounds.bottom + ui(22), swatchY = pickerY + ui(22);
    text(d, L"PICKER", px, pickerY, kText);
    text(d, L"ALT", px + ui(108), pickerY, kMutedText);
    const CanvasViewport primary{px, swatchY, px + ui(92), swatchY + ui(52)};
    const CanvasViewport secondary{px + ui(108), swatchY, px + ui(200), swatchY + ui(52)};
    const auto &active = activePaintColor(state);
    glow(d, state.secondaryActive ? secondary : primary, rgb(active.red, active.green, active.blue),
         {width - l.rightInset + ui(1), pickerY + ui(14), width - ui(1), swatchY + ui(72)});
    const auto swatch = [&](CanvasViewport at, const PaletteColor &color, int action) {
        bevel(d, at.left, at.top, at.right - at.left, at.bottom - at.top, true);
        rect(d, at.left + ui(3), at.top + ui(3), at.right - at.left - ui(6), at.bottom - at.top - ui(6),
             rgb(color.red, color.green, color.blue));
        hits.push_back({at, action});
    };
    swatch(primary, state.paintColor, UI_COLOR);
    swatch(secondary, state.secondaryColor, UI_SECONDARY_COLOR);
    const int valuesY = swatchY + ui(68);
    const auto value = [&](std::wstring label, int row) {
        text(d, label, px, valuesY + ui(row * 18), kText, width - ui(12));
    };
    value(L"HEX: " + formatHex(active), 0);
    value(L"R: " + formatInt(active.red), 1);
    value(L"G: " + formatInt(active.green), 2);
    value(L"B: " + formatInt(active.blue), 3);
    const double r = active.red / 255.0, g = active.green / 255.0, b = active.blue / 255.0;
    const double maximum = std::max({r, g, b}), minimum = std::min({r, g, b}), delta = maximum - minimum;
    double hue = delta == 0 ? 0
                            : (maximum == r   ? (g - b) / delta
                               : maximum == g ? 2 + (b - r) / delta
                                              : 4 + (r - g) / delta) *
                                  60;
    if (hue < 0)
        hue += 360;
    value(L"H: " + formatInt(int(std::round(hue))) + L" DEG", 4);
    value(L"S: " + formatInt(int(std::round(maximum == 0 ? 0 : delta / maximum * 100))) + L"%", 5);
    value(L"V: " + formatInt(int(std::round(maximum * 100))) + L"%", 6);
    const int infoY = height - ui(kStatusHeight) - ui(86);
    text(d, L"IMAGE", px, infoY, kText);
    text(d, state.imageName, px, infoY + ui(20), kText, width - ui(12));
    text(d, formatInt(state.image().width) + L" X " + formatInt(state.image().height) + L" PX", px, infoY + ui(40),
         kText, width - ui(12));
    text(d, formatDouble(state.imagePpi) + L" PPI", px, infoY + ui(60), kText);
    const wchar_t *ready = !state.cels[state.activeCel].visible ? L"ACTIVE CEL HIDDEN"
                           : state.tool == PaintTool::Brush     ? L"READY  |  BRUSH"
                           : state.tool == PaintTool::Cell      ? L"READY  |  CELL PAINT"
                                                                : L"READY  |  PICK";
    text(d, !state.activity.empty() ? state.activity : (state.probeValid ? state.probeText : ready), ui(10),
         height - ui(17), kMutedText, width - ui(360));
    const std::string_view gpu = gpuName;
    text(d, std::wstring(gpu.begin(), gpu.end()), std::max(ui(520), int(width) - ui(340)), height - ui(17), kAccent,
         width - ui(8));
    if (state.showRulers)
    {
        const int r = ui(kRulerThickness), rx = l.leftInset, ry = l.topInset;
        rect(d, rx, ry, width - l.leftInset - l.rightInset, r, kRuler);
        rect(d, rx, ry, r, height - ry - ui(kStatusHeight), kRuler);
        text(d, unitName(), rx + ui(4), ry + ui(9), kRulerText);
        const double scale = l.pxPerIn / unitsPerInch();
        const double desired = ui(70) / scale, magnitude = std::pow(10.0, std::floor(std::log10(desired)));
        double major = magnitude;
        for (double n : {1.0, 2.0, 5.0, 10.0})
            if (n * magnitude >= desired)
            {
                major = n * magnitude;
                break;
            }
        if (units == 0)
            major = std::max(1.0, major);
        const double minor = major / (units == 0 && major < 5 ? 1 : 5);
        for (int axis = 0; axis < 2; ++axis)
        {
            const double origin = axis ? l.originY : l.originX;
            const int start = axis ? area.top : area.left, end = axis ? area.bottom : area.right;
            const int first = int(std::ceil((start - origin) / (minor * scale))),
                      last = int(std::floor((end - origin) / (minor * scale)));
            for (int i = first; i <= last; ++i)
            {
                const double value = i * minor, pos = origin + value * scale;
                const bool big = std::abs(value / major - std::round(value / major)) < 0.001;
                const int len = ui(big ? 10 : 4);
                if (axis)
                    rect(d, rx + r - len, pos, len, 1, kTicks);
                else
                    rect(d, pos, ry + r - len, 1, len, kTicks);
                if (big)
                {
                    wchar_t label[32];
                    std::swprintf(label, 32, L"%.6g", value == 0 ? 0 : value);
                    if (axis)
                        textVertical(d, label, rx + ui(2), int(pos) + ui(3), kRulerText, area.bottom);
                    else
                        text(d, label, int(pos) + ui(3), ry + ui(3), kRulerText, int(width) - l.rightInset);
                }
            }
        }
        if (state.showLines)
            for (std::size_t index = 0; index < state.lines.size(); ++index)
            {
                const auto &line = state.lines[index];
                const double at = (line.vertical ? l.originX : l.originY) + line.position / state.imagePpi * l.pxPerIn;
                const int low = line.vertical ? area.left : area.top, high = line.vertical ? area.right : area.bottom;
                if (at < low || at >= high)
                    continue;
                const double spread = ui(18), sigma = std::max(1.0, ui(6) * 1.0);
                if (int(index) == state.highlightedLine)
                    for (int offset = -int(spread); offset <= int(spread); ++offset)
                    {
                        const double start = std::floor(at) + offset;
                        if (start >= high || start < low)
                            continue;
                        const auto count = d.overlays.size();
                        if (line.vertical)
                            rect(d, start, ry, 1, r, line.color);
                        else
                            rect(d, rx, start, r, 1, line.color);
                        if (d.overlays.size() > count)
                            d.overlays.back().alpha = glowOpacity(offset, sigma);
                    }
                if (line.vertical)
                    rect(d, at, ry, 1, r, line.color);
                else
                    rect(d, rx, at, r, 1, line.color);
            }
    }
}
