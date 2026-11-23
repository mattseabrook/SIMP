#pragma once
#include "canvas_render.h"
#include "image.h"
#include "cels.h"
#include "color_inventory.h"
#include "guides.h"
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
inline constexpr double kUiFontPixel = 1.5;
enum class PaintTool
{
    Brush,
    Cell,
    Pick
};
inline constexpr int kLeftPanelWidth = 112;
inline constexpr int kRightPanelWidth = 224;
inline constexpr int kRulerThickness = 26;
inline constexpr int kTopToolbarHeight = 40;
inline constexpr int kStatusHeight = 26;
inline double rulerUnitsPerInch(int units, double ppi)
{
    return units == 0 ? ppi : (units == 2 ? 2.54 : 1.0);
}
struct CanvasState : CelStack
{
    double imageWidthIn = 30.0;
    double imageHeightIn = 40.0;
    double imagePpi = 100.0;
    double canvasWidthIn = 29.75;
    double canvasHeightIn = 39.5;
    double gridSizeIn = 2.0;
    double offsetXIn = 0.0;
    double offsetYIn = 0.0;
    double zoomScale = 1.0;
    int majorEvery = 0;
    int paletteCount = 32;
    bool showGrid = true;
    bool showRulers = true;
    bool showLines = true;
    std::vector<GuideLine> lines;
    bool hasImage = false;
    std::wstring imagePath;
    std::wstring imageName;
    double detectedPpi = 100.0;
    std::wstring ppiSource = L"manual";
    std::vector<PaletteColor> palette;
    bool paletteUpdating = false;
    std::size_t paletteOffset = 0;
    std::size_t celOffset = 0;
    bool celBusy = false;
    int highlightedLine = -1;
    std::wstring activity;
    PaletteColor paintColor{0, 0, 0};
    PaletteColor secondaryColor{255, 255, 255};
    PaintTool tool = PaintTool::Cell;
    int brushSize = 1;
    bool secondaryActive = false;
    bool probeValid = false;
    PaletteColor probeColor;
    std::wstring probeText = L"Move over image";
};

inline const PaletteColor &activePaintColor(const CanvasState &state)
{
    return state.secondaryActive ? state.secondaryColor : state.paintColor;
}

struct CanvasLayout
{
    double originX = 0.0;
    double originY = 0.0;
    double fitPxPerIn = 1.0;
    double pxPerIn = 1.0;
    double contentMaxXIn = 1.0;
    double contentMaxYIn = 1.0;
    int leftInset = 0;
    int rightInset = 0;
    int topInset = kTopToolbarHeight;
};

struct PaletteLayout
{
    CanvasViewport bounds, track;
    int cell = 1, gap = 1, columns = 1, rows = 1;
    std::size_t capacity() const
    {
        return std::size_t(columns) * rows;
    }
};
PaletteLayout layoutPalette(int width, double scale, std::size_t colors, int height = 720);
struct CelsLayout
{
    CanvasViewport bounds, track;
    int headingY, toolsY, rowHeight, rows;
};
CelsLayout layoutCels(int width, int height, double scale);
std::size_t clampPaletteOffset(const PaletteLayout &, std::size_t colors, std::size_t offset);
double originalZoomScale(const CanvasState &, const CanvasLayout &);
struct UiHit
{
    CanvasViewport rect;
    int action;
};
CanvasLayout computeCanvasLayout(const CanvasState &state, int width, int height, double scale, double panX,
                                 double panY);
bool changeImagePpi(CanvasState &state, double ppi);
int rulerAxis(const CanvasLayout &layout, int width, int height, double scale, bool visible, int x, int y);
int hitGuide(const CanvasState &state, const CanvasLayout &layout, int axis, int coordinate, double tolerance);
struct EditorUi
{
    const CanvasState &state;
    CanvasLayout layout;
    int surfaceWidth, surfaceHeight, units;
    double scale;
    std::string_view gpuName;
    std::vector<UiHit> hits;
    void build(CanvasRenderData &data);

  private:
    int ui(int value) const;
    double unitsPerInch() const;
    const wchar_t *unitName() const;
    void addOverlayRect(CanvasRenderData &, std::int32_t, std::int32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                        std::uint32_t, std::uint32_t);
    void addOverlayRectD(CanvasRenderData &, double, double, double, double, std::uint32_t, std::uint32_t,
                         std::uint32_t);
    void rect(CanvasRenderData &, double, double, double, double, std::uint32_t);
    void text(CanvasRenderData &, std::wstring_view, double, double, std::uint32_t, int maxWidth = 10000);
    void textCentered(CanvasRenderData &, std::wstring_view, CanvasViewport, std::uint32_t);
    void glow(CanvasRenderData &, CanvasViewport, std::uint32_t, CanvasViewport);
    void textVertical(CanvasRenderData &, const std::wstring &, int, int, std::uint32_t, int);
    void bevel(CanvasRenderData &, int, int, int, int, bool selected = false);
    void button(CanvasRenderData &, const wchar_t *, int, int, int, int, bool selected = false, bool enabled = true);
};
