#include "app.h"

#include "config.h"
#include "menu.h"
#include "simpformat.h"
#include "system.h"
#include "vulkan.h"
#include "window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <commdlg.h>
#include <shellapi.h>
#include "../resource.h"
#include "editor_ui.h"
#include "editor_format.h"
#include "image_history.h"
#include "paint_tools.h"
#include "cpu_work.h"
#include "cpu_pixels.h"
#include <deque>
#include <mutex>
#include <functional>
#include <thread>
#include <atomic>
#include <windowsx.h>

namespace
{
constexpr double kMaxZoom = 128.0;
constexpr double kZoomStep = 1.25;
struct PngPhysicalInfo
{
    bool found = false;
    double ppiX = 0.0;
    double ppiY = 0.0;
};

struct DragState
{
    bool panning = false;
    bool painting = false;
    bool havePosition = false;
    int lastX = -1;
    int lastY = -1;
    POINT startPoint{};
    double startOffsetXIn = 0.0;
    double startOffsetYIn = 0.0;
};

CanvasState g_state;
ImageHistory g_history;
bool g_documentModified = false;
std::wstring g_lastTitle;
std::filesystem::path g_documentPath;
int g_dragLine = -1;
void showLineDialog(HWND owner, int index);
CanvasLayout g_layout;
DragState g_drag;
RECT g_dirtyPixels{};
bool g_hasDirtyPixels = false;
HWND g_parent = nullptr;
int g_rulerUnits = 0; // pixels, inches, centimeters
int g_brushWheelDelta = 0;
double g_panX = 0.0, g_panY = 0.0;
double g_uiScale = 1.0;
int g_surfaceWidth = 0, g_surfaceHeight = 0;
std::vector<UiHit> g_uiHits;
constexpr UINT kCpuReadyMessage = WM_APP + 40;
std::mutex g_completionMutex;
std::deque<std::function<void()>> g_completions;
std::vector<std::pair<std::uint64_t, std::wstring>> g_activity;
std::uint64_t g_jobId = 0, g_epoch = 0, g_revision = 0, g_pixelsRevision = 0, g_openTicket = 0;
bool g_paletteRunning = false, g_paletteDirty = false, g_saving = false, g_saveAgain = false, g_shutdown = false,
     g_confirming = false;
bool g_trackingMouse = false, g_dragPalette = false, g_dragCels = false;
std::uint64_t g_nextCelId = 2;
void renameActiveCel(HWND);
void handleCelAction(HWND, int);
int g_lineOptions = -1;
std::shared_ptr<std::atomic<bool>> g_openCancel;
std::filesystem::path g_savingPath, g_queuedSavePath;
std::uint64_t g_savingEpoch = 0, g_queuedSaveEpoch = 0;
void createCanvasAsync(std::uint32_t width, std::uint32_t height, double ppi);

void requestCanvasRender();
void schedulePalette();
void finishActivity(std::uint64_t id)
{
    std::erase_if(g_activity, [id](const auto &item) { return item.first == id; });
    g_state.activity = g_activity.empty() ? L"" : g_activity.back().second;
}
template <class Work, class Done>
void startCpuJob(const wchar_t *label, Work work, Done done, std::function<void()> failed = {})
{
    const auto id = ++g_jobId;
    const HWND owner = g_parent;
    g_activity.emplace_back(id, label);
    g_state.activity = label;
    requestRender();
    enqueueCpuWork([id, owner, work = std::move(work), done = std::move(done), failed = std::move(failed)]() mutable {
        std::function<void()> completion;
        try
        {
            auto result = std::make_shared<decltype(work())>(work());
            completion = [id, result, done = std::move(done)]() mutable {
                finishActivity(id);
                done(std::move(*result));
                requestRender();
            };
        }
        catch (...)
        {
            const auto error = std::current_exception();
            completion = [id, error, owner, failed = std::move(failed)] {
                finishActivity(id);
                if (failed)
                    failed();
                try
                {
                    std::rethrow_exception(error);
                }
                catch (const std::exception &e)
                {
                    MessageBoxA(owner, e.what(), "SIMP", MB_OK | MB_ICONERROR);
                }
                catch (...)
                {
                    MessageBoxW(owner, L"The background operation failed.", L"SIMP", MB_OK | MB_ICONERROR);
                }
                requestRender();
            };
        }
        {
            std::lock_guard lock(g_completionMutex);
            g_completions.push_back(std::move(completion));
        }
        PostMessageW(owner, kCpuReadyMessage, 0, 0);
    });
}
void markDocumentModified()
{
    g_documentModified = true;
    ++g_revision;
}

void resetEditingSession()
{
    ++g_epoch;
    ++g_revision;
    ++g_pixelsRevision;
    ++g_openTicket;
    g_paletteDirty = false;
    g_state.cels = {Cel{}};
    g_state.activeCel = 0;
    g_state.celOffset = 0;
    g_state.celBusy = false;
    g_nextCelId = 2;
    g_state.paletteOffset = 0;
    if (g_openCancel)
        g_openCancel->store(true);
    g_history.clear();
    g_drag = {};
    g_dragLine = -1;
    g_dragPalette = false;
    g_dragCels = false;
    g_state.highlightedLine = -1;
    if (GetCapture() == g_parent)
        ReleaseCapture();
    g_documentPath.clear();
    g_state.lines.clear();
    g_state.probeValid = false;
    g_documentModified = false;
}

std::filesystem::path lastOpenFile()
{
    if (!config.contains("lastOpenFile") || !config["lastOpenFile"].is_string())
        return {};
    const auto text = config["lastOpenFile"].get<std::string>();
    if (text.size() > 131072 || text.find('\0') != std::string::npos)
        return {};
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
void rememberOpenFile(const std::filesystem::path &path)
{
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    if (error)
        return;
    const auto utf8 = absolute.u8string();
    config["lastOpenFile"] = std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    try
    {
        save_config(getConfigPath());
    }
    catch (...)
    {
        MessageBoxW(g_parent, L"Could not remember the file for startup. Check access to config.json.", L"SIMP",
                    MB_OK | MB_ICONWARNING);
    }
}
void refreshDocumentTitle()
{
    const std::wstring title = (g_state.imageName.empty() ? L"Untitled" : g_state.imageName) +
                               (g_documentModified ? L" * - SIMP" : L" - SIMP");
    if (g_parent && title != g_lastTitle)
    {
        SetWindowTextW(g_parent, title.c_str());
        g_lastTitle = title;
    }
}

double clampDouble(double value, double minimum, double maximum)
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : minimum;
}

PngPhysicalInfo parsePngPhysicalInfo(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};
    const auto end = input.tellg();
    input.seekg(0);
    std::array<std::uint8_t, 8> signature{};
    input.read(reinterpret_cast<char *>(signature.data()), 8);
    if (signature != std::array<std::uint8_t, 8>{137, 80, 78, 71, 13, 10, 26, 10})
        return {};
    const auto be32 = [](const std::uint8_t *p) {
        return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
    };
    while (input && end - input.tellg() >= 12)
    {
        std::array<std::uint8_t, 8> header{};
        input.read(reinterpret_cast<char *>(header.data()), 8);
        const auto size = be32(header.data());
        if (std::uint64_t(size) + 4 > static_cast<std::uint64_t>(end - input.tellg()))
            break;
        if (!std::memcmp(header.data() + 4, "IDAT", 4) || !std::memcmp(header.data() + 4, "IEND", 4))
            break;
        if (!std::memcmp(header.data() + 4, "pHYs", 4) && size == 9)
        {
            std::uint8_t physical[9]{};
            input.read(reinterpret_cast<char *>(physical), 9);
            const auto x = be32(physical), y = be32(physical + 4);
            if (physical[8] == 1 && x && y)
                return {true, x * 0.0254, y * 0.0254};
            break;
        }
        input.seekg(std::streamoff(size) + 4, std::ios::cur);
    }
    return {};
}

double jsonDouble(const nlohmann::json &object, const char *key, double fallback)
{
    if (!object.contains(key) || !object[key].is_number())
        return fallback;
    const double value = object[key].get<double>();
    return std::isfinite(value) ? value : fallback;
}

int jsonInt(const nlohmann::json &object, const char *key, int fallback)
{
    if (!object.contains(key) || !object[key].is_number_integer())
        return fallback;
    return object[key].get<int>();
}

bool jsonBool(const nlohmann::json &object, const char *key, bool fallback)
{
    if (!object.contains(key) || !object[key].is_boolean())
        return fallback;
    return object[key].get<bool>();
}

int ui(int value)
{
    return static_cast<int>(std::round(value * g_uiScale));
}

CanvasLayout computeLayout(std::uint32_t width, std::uint32_t height)
{
    return computeCanvasLayout(g_state, width, height, g_uiScale, g_panX, g_panY);
}

CanvasViewport workspaceRect()
{
    const int ruler = g_state.showRulers ? ui(kRulerThickness) : 0;
    return {g_layout.leftInset + ruler, g_layout.topInset + ruler, g_surfaceWidth - g_layout.rightInset,
            g_surfaceHeight - ui(kStatusHeight)};
}
bool insideWorkspace(int x, int y)
{
    const auto r = workspaceRect();
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
void updateGuideHighlight(int x, int y)
{
    const int axis = rulerAxis(g_layout, g_surfaceWidth, g_surfaceHeight, g_uiScale, g_state.showRulers, x, y);
    const int highlighted =
        g_dragLine >= 0
            ? g_dragLine
            : (g_lineOptions >= 0 ? g_lineOptions : hitGuide(g_state, g_layout, axis, axis == 0 ? x : y, ui(12)));
    if (highlighted != g_state.highlightedLine)
    {
        g_state.highlightedLine = highlighted;
        requestRender();
    }
}
void moveDraggedLine(int x, int y)
{
    if (g_dragLine < 0 || std::size_t(g_dragLine) >= g_state.lines.size())
        return;
    auto &line = g_state.lines[g_dragLine];
    const double pixel = ((line.vertical ? x : y) - (line.vertical ? g_layout.originX : g_layout.originY)) /
                         g_layout.pxPerIn * g_state.imagePpi;
    if (pixel != line.position && std::isfinite(pixel) && std::abs(pixel) <= 1000000000.0)
    {
        g_history.captureGuides(g_state.lines);
        line.position = pixel;
        markDocumentModified();
        requestCanvasRender();
    }
}
void scrollPaletteTo(int y)
{
    const auto p = layoutPalette(g_surfaceWidth, g_uiScale, g_state.palette.size(), g_surfaceHeight);
    const auto maximum = clampPaletteOffset(p, g_state.palette.size(), g_state.palette.size());
    const double at = std::clamp(double(y - p.track.top) / std::max(1, p.track.bottom - p.track.top), 0.0, 1.0);
    g_state.paletteOffset = clampPaletteOffset(p, g_state.palette.size(), std::size_t(at * maximum));
    requestRender();
}

void zoomCanvas(double factor, int anchorX, int anchorY)
{
    const double localX = (anchorX - g_layout.originX) / g_layout.pxPerIn;
    const double localY = (anchorY - g_layout.originY) / g_layout.pxPerIn;
    g_state.zoomScale =
        clampDouble(g_state.zoomScale * factor, std::min(1.0, originalZoomScale(g_state, g_layout)) / 128,
                    std::max(1.0, originalZoomScale(g_state, g_layout)) * 128);
    const auto next = computeLayout(g_surfaceWidth, g_surfaceHeight);
    g_panX += anchorX - (next.originX + localX * next.pxPerIn);
    g_panY += anchorY - (next.originY + localY * next.pxPerIn);
    requestCanvasRender();
}

bool pointInsideImage(int x, int y)
{
    if (!g_state.hasImage || !insideWorkspace(x, y))
        return false;

    const double imageX = g_layout.originX + g_state.offsetXIn * g_layout.pxPerIn;
    const double imageY = g_layout.originY + g_state.offsetYIn * g_layout.pxPerIn;
    const double imageWidth = g_state.imageWidthIn * g_layout.pxPerIn;
    const double imageHeight = g_state.imageHeightIn * g_layout.pxPerIn;
    return x >= imageX && y >= imageY && x < imageX + imageWidth && y < imageY + imageHeight;
}

bool pointToGridCell(int x, int y, int &cellX, int &cellY)
{
    if (!pointInsideImage(x, y) || g_layout.pxPerIn <= 0.0)
        return false;

    const double xIn = (static_cast<double>(x) - g_layout.originX) / g_layout.pxPerIn;
    const double yIn = (static_cast<double>(y) - g_layout.originY) / g_layout.pxPerIn;
    if (xIn < 0.0 || yIn < 0.0 || xIn >= g_state.canvasWidthIn || yIn >= g_state.canvasHeightIn)
        return false;

    cellX = static_cast<int>(std::floor(xIn / g_state.gridSizeIn));
    cellY = static_cast<int>(std::floor(yIn / g_state.gridSizeIn));
    return true;
}

void markDirtyPixels(int left, int top, int right, int bottom, bool upload = true)
{
    ++g_pixelsRevision;
    if (upload)
    {
        const RECT changed{left, top, right, bottom};
        if (!g_hasDirtyPixels)
            g_dirtyPixels = changed;
        else
        {
            RECT combined{};
            UnionRect(&combined, &g_dirtyPixels, &changed);
            g_dirtyPixels = combined;
        }
        g_hasDirtyPixels = true;
    }
    const bool hadChanges = g_documentModified;
    markDocumentModified();
    if (!hadChanges)
        refreshDocumentTitle();
}

PaletteColor &editingPaintColor()
{
    return g_state.secondaryActive ? g_state.secondaryColor : g_state.paintColor;
}
void setSecondaryActive(bool active)
{
    if (g_state.secondaryActive != active)
    {
        g_state.secondaryActive = active;
        requestRender();
    }
}
void choosePaintColor(HWND owner, bool secondary)
{
    static COLORREF customColors[16]{};
    const auto old = secondary ? g_state.secondaryColor : g_state.paintColor;
    CHOOSECOLORW choice{};
    choice.lStructSize = sizeof(choice);
    choice.hwndOwner = owner;
    choice.rgbResult = RGB(old.red, old.green, old.blue);
    choice.lpCustColors = customColors;
    choice.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&choice))
    {
        auto &color = secondary ? g_state.secondaryColor : g_state.paintColor;
        color = {GetRValue(choice.rgbResult), GetGValue(choice.rgbResult), GetBValue(choice.rgbResult)};
    }
    setSecondaryActive((GetKeyState(VK_MENU) & 0x8000) != 0);
}
void paintPixelRegion(PaintRegion region)
{
    const auto [left, top, right, bottom] = region;
    if (region.empty())
        return;
    g_history.capture(g_state.image(), left, top, right, bottom, g_state.cels[g_state.activeCel].id);
    const auto &ink = activePaintColor(g_state);
    const std::uint32_t color =
        std::uint32_t(ink.blue) | (std::uint32_t(ink.green) << 8) | (std::uint32_t(ink.red) << 16) | 0xff000000u;
    auto *pixels = g_state.image().pixels.data();
    const auto paint = [&](std::size_t begin, std::size_t end) {
        for (auto row = begin; row < end; ++row)
            fillPixelSpan(pixels + ((std::size_t(top) + row) * g_state.image().width + left) * 4, right - left, color);
    };
    if (std::size_t(right - left) * (bottom - top) >= 1048576)
        parallelCpuRange(bottom - top, 32, paint);
    else
        paint(0, bottom - top);
    const bool computed = paintVulkanCanvasRegion(left, top, right - left, bottom - top, color);
    markDirtyPixels(left, top, right, bottom, !computed);
}

void paintGridCell(int cellX, int cellY)
{
    if (!g_state.hasImage || g_state.image().pixels.empty())
        return;

    const double leftIn = static_cast<double>(cellX) * g_state.gridSizeIn - g_state.offsetXIn;
    const double topIn = static_cast<double>(cellY) * g_state.gridSizeIn - g_state.offsetYIn;
    const double rightIn = std::min((cellX + 1) * g_state.gridSizeIn, g_state.canvasWidthIn) - g_state.offsetXIn;
    const double bottomIn = std::min((cellY + 1) * g_state.gridSizeIn, g_state.canvasHeightIn) - g_state.offsetYIn;
    const int imageWidth = static_cast<int>(g_state.image().width);
    const int imageHeight = static_cast<int>(g_state.image().height);
    const auto pixelEdge = [](double inches, double extentIn, int pixels) {
        const double scaled = std::round(inches / extentIn * pixels * 1000000.0) / 1000000.0;
        return static_cast<int>(std::clamp(std::floor(scaled), 0.0, static_cast<double>(pixels)));
    };
    const int left = pixelEdge(leftIn, g_state.imageWidthIn, imageWidth);
    const int top = pixelEdge(topIn, g_state.imageHeightIn, imageHeight);
    const int right = pixelEdge(rightIn, g_state.imageWidthIn, imageWidth);
    const int bottom = pixelEdge(bottomIn, g_state.imageHeightIn, imageHeight);
    if (left >= right || top >= bottom)
        return;

    paintPixelRegion({left, top, right, bottom});
}

bool pointToImagePixel(int x, int y, int &px, int &py)
{
    if (!pointInsideImage(x, y))
        return false;
    const double localX = (x - g_layout.originX) / g_layout.pxPerIn - g_state.offsetXIn;
    const double localY = (y - g_layout.originY) / g_layout.pxPerIn - g_state.offsetYIn;
    const auto &image = g_state.image();
    px = std::clamp(int(localX / g_state.imageWidthIn * image.width), 0, int(image.width) - 1);
    py = std::clamp(int(localY / g_state.imageHeightIn * image.height), 0, int(image.height) - 1);
    return true;
}
bool pointToPaintPosition(int x, int y, int &px, int &py)
{
    if (!g_state.cels[g_state.activeCel].visible)
        return false;
    if (g_state.tool == PaintTool::Cell)
        return pointToGridCell(x, y, px, py);
    return g_state.tool == PaintTool::Brush && pointToImagePixel(x, y, px, py);
}
void paintPosition(int x, int y)
{
    if (g_state.tool == PaintTool::Brush)
        paintPixelRegion(brushRegion(x, y, g_state.brushSize, g_state.image().width, g_state.image().height));
    else if (g_state.tool == PaintTool::Cell)
        paintGridCell(x, y);
}
void continueStroke(int targetX, int targetY)
{
    if (g_state.tool == PaintTool::Brush)
        continueBrushLine(g_drag.lastX, g_drag.lastY, targetX, targetY, g_state.brushSize, g_state.image().width,
                          g_state.image().height, paintPixelRegion);
    else
        continuePaintLine(g_drag.lastX, g_drag.lastY, targetX, targetY, paintPosition);
    g_drag.lastX = targetX;
    g_drag.lastY = targetY;
    requestRender();
}
void advanceStrokeAt(int x, int y)
{
    int px = 0, py = 0;
    if (!pointToPaintPosition(x, y, px, py))
    {
        g_drag.havePosition = false;
        return;
    }
    if (!g_drag.havePosition)
    {
        paintPosition(px, py);
        g_drag.lastX = px;
        g_drag.lastY = py;
        requestRender();
    }
    else if (px != g_drag.lastX || py != g_drag.lastY)
        continueStroke(px, py);
    g_drag.havePosition = true;
}
void selectPaintTool(PaintTool tool)
{
    if (g_drag.painting)
    {
        g_drag.painting = false;
        g_history.commit();
        schedulePalette();
        if (GetCapture() == g_parent)
            ReleaseCapture();
    }
    g_state.tool = tool;
    g_brushWheelDelta = 0;
    requestRender();
}

void finishCelStroke()
{
    const bool painted = g_drag.painting;
    g_drag.painting = false;
    g_history.commit();
    if (GetCapture() == g_parent)
        ReleaseCapture();
    flushCanvasCaliperEdits();
    if (painted)
        schedulePalette();
}
void revealActiveCel()
{
    const auto list = layoutCels(g_surfaceWidth, g_surfaceHeight, g_uiScale);
    const auto row = g_state.cels.size() - 1 - g_state.activeCel;
    if (row < g_state.celOffset)
        g_state.celOffset = row;
    else if (row >= g_state.celOffset + list.rows)
        g_state.celOffset = row - list.rows + 1;
}
void commitCelChange()
{
    ++g_pixelsRevision;
    g_history.commit();
    g_hasDirtyPixels = false;
    markDocumentModified();
    setVulkanCels(g_state);
    schedulePalette();
    revealActiveCel();
    requestCanvasRender();
}
void createCel(bool duplicate)
{
    if (!g_state.hasImage || g_state.celBusy || g_state.cels.size() >= kMaxCels ||
        std::uint64_t(g_state.image().width) * g_state.image().height * (g_state.cels.size() + 1) > kMaxCelPixels)
        return;
    finishCelStroke();
    g_state.celBusy = true;
    const auto epoch = g_epoch;
    const auto source = duplicate ? g_state.image() : DecodedImage{g_state.image().width, g_state.image().height, {}};
    const auto name = duplicate ? g_state.cels[g_state.activeCel].name + L" copy" : L"";
    startCpuJob(
        duplicate ? L"Duplicating cel..." : L"Creating cel...",
        [source, duplicate] {
            DecodedImage image;
            if (duplicate)
            {
                image = source;
                image.pixels.data();
            }
            else
            {
                image.width = source.width;
                image.height = source.height;
                image.pixels.resize(std::size_t(source.width) * source.height * 4);
            }
            return image;
        },
        [epoch, name](DecodedImage image) {
            if (epoch != g_epoch)
                return;
            g_state.celBusy = false;
            if (std::uint64_t(image.width) * image.height * (g_state.cels.size() + 1) > kMaxCelPixels ||
                g_state.cels.size() >= kMaxCels)
                return;
            finishCelStroke();
            g_history.captureCels(g_state);
            Cel cel;
            while (!g_nextCelId || std::any_of(g_state.cels.begin(), g_state.cels.end(),
                                               [](const auto &item) { return item.id == g_nextCelId; }))
                ++g_nextCelId;
            cel.id = g_nextCelId++;
            cel.name = name.empty() ? L"Cel " + std::to_wstring(cel.id) : name;
            cel.image = std::move(image);
            const auto at = g_state.activeCel + 1;
            g_state.cels.insert(g_state.cels.begin() + at, std::move(cel));
            g_state.activeCel = at;
            commitCelChange();
        },
        [epoch] {
            if (epoch == g_epoch)
                g_state.celBusy = false;
        });
}
void handleCelAction(HWND owner, int action)
{
    if (action == UI_CEL_SCROLL)
    {
        g_dragCels = true;
        SetCapture(owner);
        return;
    }
    if (!g_state.hasImage)
        return;
    if (action == UI_CEL_NEW || action == UI_CEL_DUPLICATE)
    {
        createCel(action == UI_CEL_DUPLICATE);
        return;
    }
    finishCelStroke();
    if (action >= UI_CEL_SELECT_FIRST && action < UI_CEL_SELECT_FIRST + int(kMaxCels))
    {
        const auto index = std::uint32_t(action - UI_CEL_SELECT_FIRST);
        if (index < g_state.cels.size())
        {
            g_state.activeCel = index;
            selectVulkanCel(index);
            revealActiveCel();
            requestCanvasRender();
        }
        return;
    }
    if (action >= UI_CEL_VISIBLE_FIRST && action < UI_CEL_VISIBLE_FIRST + int(kMaxCels))
    {
        const auto index = std::size_t(action - UI_CEL_VISIBLE_FIRST);
        if (index < g_state.cels.size())
        {
            g_history.captureCels(g_state);
            g_state.cels[index].visible = !g_state.cels[index].visible;
            g_history.commit();
            markDocumentModified();
            setVulkanCelVisibility(std::uint32_t(index), g_state.cels[index].visible);
            requestCanvasRender();
        }
        return;
    }
    if (action == UI_CEL_RENAME)
    {
        renameActiveCel(owner);
        return;
    }
    if (action == UI_CEL_DELETE && g_state.cels.size() > 1)
    {
        g_history.captureCels(g_state);
        g_state.cels.erase(g_state.cels.begin() + g_state.activeCel);
        g_state.activeCel = std::min(g_state.activeCel, std::uint32_t(g_state.cels.size() - 1));
        commitCelChange();
    }
    if ((action == UI_CEL_UP && g_state.activeCel + 1 < g_state.cels.size()) ||
        (action == UI_CEL_DOWN && g_state.activeCel > 0))
    {
        g_history.captureCels(g_state);
        const auto next = action == UI_CEL_UP ? g_state.activeCel + 1 : g_state.activeCel - 1;
        std::swap(g_state.cels[g_state.activeCel], g_state.cels[next]);
        g_state.activeCel = next;
        commitCelChange();
    }
}
void updateProbe(int x, int y)
{
    int px = 0, py = 0;
    if ((g_state.probeValid = pointToImagePixel(x, y, px, py)))
    {
        const auto [blue, green, red, alpha] = sampleCels(g_state, px, py);
        g_state.probeColor = {red, green, blue};
        wchar_t text[96];
        std::swprintf(text, std::size(text), L"#%02X%02X%02X  rgb(%u, %u, %u)  px %d,%d", unsigned(red),
                      unsigned(green), unsigned(blue), unsigned(red), unsigned(green), unsigned(blue), px, py);
        g_state.probeText = text;
    }
    requestRender();
}

void schedulePalette()
{
    g_paletteDirty = true;
    if (g_paletteRunning || !g_state.hasImage)
        return;
    g_paletteRunning = true;
    g_paletteDirty = false;
    g_state.paletteUpdating = true;
    const auto epoch = g_epoch, revision = g_pixelsRevision;
    const CelStack snapshot = g_state;
    startCpuJob(
        L"Analyzing colors...", [snapshot] { return collectCelColors(snapshot); },
        [epoch, revision](std::vector<PaletteColor> colors) {
            g_paletteRunning = false;
            g_state.paletteUpdating = false;
            if (epoch == g_epoch && revision == g_pixelsRevision)
            {
                g_state.palette = std::move(colors);
                const auto p = layoutPalette(g_surfaceWidth, g_uiScale, g_state.palette.size(), g_surfaceHeight);
                g_state.paletteOffset = clampPaletteOffset(p, g_state.palette.size(), g_state.paletteOffset);
            }
            if (g_paletteDirty)
                schedulePalette();
        },
        [] {
            g_paletteRunning = false;
            g_state.paletteUpdating = false;
        });
}

void applyPngSpecsToState()
{
    if (!g_state.hasImage || g_state.detectedPpi <= 0.0)
        return;

    g_state.imagePpi = clampDouble(g_state.detectedPpi, 1.0, 5000.0);
    g_state.gridSizeIn = std::max(g_state.gridSizeIn, 1.0 / g_state.imagePpi);
    g_state.imageWidthIn = clampDouble(static_cast<double>(g_state.image().width) / g_state.imagePpi, 0.0002, 16384.0);
    g_state.imageHeightIn =
        clampDouble(static_cast<double>(g_state.image().height) / g_state.imagePpi, 0.0002, 16384.0);
}

void requestCanvasRender()
{
    layoutCanvasCaliperChrome();
    refreshDocumentTitle();
    requestRender();
}
} // namespace
void loadCanvasCaliperConfig()
{
    const nlohmann::json empty = nlohmann::json::object();
    const nlohmann::json &cc =
        (config.contains("canvasCaliper") && config["canvasCaliper"].is_object()) ? config["canvasCaliper"] : empty;

    g_state.imageWidthIn = clampDouble(jsonDouble(cc, "imageWidthIn", g_state.imageWidthIn), 0.0002, 16384.0);
    g_state.imageHeightIn = clampDouble(jsonDouble(cc, "imageHeightIn", g_state.imageHeightIn), 0.0002, 16384.0);
    g_state.imagePpi = clampDouble(jsonDouble(cc, "imagePpi", g_state.imagePpi), 1.0, 5000.0);
    g_state.canvasWidthIn = clampDouble(jsonDouble(cc, "canvasWidthIn", g_state.canvasWidthIn), 0.0002, 16384.0);
    g_state.canvasHeightIn = clampDouble(jsonDouble(cc, "canvasHeightIn", g_state.canvasHeightIn), 0.0002, 16384.0);
    g_state.gridSizeIn = clampDouble(jsonDouble(cc, "gridSizeIn", g_state.gridSizeIn), 0.000001, 10000.0);
    g_state.offsetXIn = clampDouble(jsonDouble(cc, "offsetXIn", g_state.offsetXIn), -10000, 10000);
    g_state.offsetYIn = clampDouble(jsonDouble(cc, "offsetYIn", g_state.offsetYIn), -10000, 10000);
    g_state.zoomScale = clampDouble(jsonDouble(cc, "zoomScale", g_state.zoomScale), 1.0, kMaxZoom);
    g_state.majorEvery = std::clamp(jsonInt(cc, "majorEvery", g_state.majorEvery), 0, 10000);
    g_state.paletteCount = std::clamp(jsonInt(cc, "paletteCount", g_state.paletteCount), 8, 64);
    g_state.gridSizeIn = std::max(g_state.gridSizeIn, 1.0 / g_state.imagePpi);
    g_state.showGrid = jsonBool(cc, "showGrid", g_state.showGrid);
    g_state.showRulers = jsonBool(cc, "showRulers", g_state.showRulers);
    g_state.showLines = jsonBool(cc, "showLines", true);

    g_rulerUnits = std::clamp(jsonInt(cc, "rulerUnits", 0), 0, 2);
}

void saveCanvasCaliperConfig()
{
    nlohmann::json cc = {{"imageWidthIn", g_state.imageWidthIn},
                         {"imageHeightIn", g_state.imageHeightIn},
                         {"imagePpi", g_state.imagePpi},
                         {"canvasWidthIn", g_state.canvasWidthIn},
                         {"canvasHeightIn", g_state.canvasHeightIn},
                         {"gridSizeIn", g_state.gridSizeIn},
                         {"majorEvery", g_state.majorEvery},
                         {"offsetXIn", g_state.offsetXIn},
                         {"offsetYIn", g_state.offsetYIn},
                         {"zoomScale", g_state.zoomScale},
                         {"paletteCount", g_state.paletteCount},
                         {"showGrid", g_state.showGrid},
                         {"showRulers", g_state.showRulers}};
    cc["rulerUnits"] = g_rulerUnits;
    cc["showLines"] = g_state.showLines;
    config["canvasCaliper"] = cc;
}

void initializeCanvasCaliper(HWND hwnd)
{
    g_parent = hwnd;
    g_shutdown = false;
    loadCanvasCaliperConfig();
    DragAcceptFiles(hwnd, TRUE);
    layoutCanvasCaliperChrome();
    const auto last = lastOpenFile();
    if (!last.empty())
        loadCanvasCaliperImagePath(hwnd, last.c_str(), true);
    else
        newCanvasCaliperImage();
}
void shutdownCanvasCaliper()
{
    g_shutdown = true;
    g_history.clear();
    if (g_openCancel)
        g_openCancel->store(true);
    waitForCpuWork();
    {
        std::lock_guard lock(g_completionMutex);
        g_completions.clear();
    }

    if (g_parent && IsWindow(g_parent))
        DragAcceptFiles(g_parent, FALSE);
    g_parent = nullptr;
}
void layoutCanvasCaliperChrome()
{
    RECT client{};
    if (!g_parent || !GetClientRect(g_parent, &client))
        return;
    g_uiScale = std::max(1.0, std::min({double(GetDpiForWindow(g_parent)) / 96.0, double(client.right) / 1280.0,
                                        double(client.bottom) / 720.0}));
    g_surfaceWidth = client.right;
    g_surfaceHeight = client.bottom;
    g_layout = computeLayout(client.right, client.bottom);
}
bool loadCanvasCaliperImagePath(HWND owner, const wchar_t *filename, bool restore)
{
    if (!filename || !*filename)
        return false;
    if (g_openCancel)
        g_openCancel->store(true);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    g_openCancel = cancel;
    const std::filesystem::path path(filename);
    const auto ticket = ++g_openTicket;
    const double fallbackPpi = g_state.imagePpi;
    const auto gpuLimit = vulkanDeviceProperties().limits.maxImageDimension2D;
    struct Loaded
    {
        SimpDocument document;
        DecodedImage image;
        std::vector<PaletteColor> colors;
        PngPhysicalInfo physical;
        bool native = false, canceled = false;
    };
    startCpuJob(
        L"Opening image...",
        [path, cancel, fallbackPpi, gpuLimit] {
            Loaded loaded;
            if (cancel->load())
            {
                loaded.canceled = true;
                return loaded;
            }
            std::ifstream input(path, std::ios::binary);
            char magic[4]{};
            input.read(magic, 4);
            loaded.native = input && !std::memcmp(magic, "SIMP", 4);
            input.close();
            if (loaded.native)
            {
                if (!loadSimpDocument(path, loaded.document))
                    throw std::runtime_error("Could not read this SIMP document.");
                if (loaded.document.hasCels)
                {
                    loaded.image.width = loaded.document.stack.image().width;
                    loaded.image.height = loaded.document.stack.image().height;
                    loaded.document.layerImage = {}; // Compatibility preview isn't editable storage.
                }
                else
                    loaded.image = std::move(loaded.document.layerImage);
            }
            else
            {
                if (!decodeImageFileBGRA(path, loaded.image))
                    throw std::runtime_error("Could not decode this image.");
                loaded.physical = parsePngPhysicalInfo(path);
                loaded.document.layerPpi =
                    loaded.physical.found ? (loaded.physical.ppiX + loaded.physical.ppiY) * 0.5 : fallbackPpi;
            }
            if (loaded.image.width > gpuLimit || loaded.image.height > gpuLimit)
                throw std::runtime_error("This image exceeds the selected GPU texture limit.");
            if (cancel->load())
            {
                loaded.canceled = true;
                return loaded;
            }
            loaded.colors =
                loaded.document.hasCels ? collectCelColors(loaded.document.stack) : collectImageColors(loaded.image);
            return loaded;
        },
        [owner, path, ticket](Loaded loaded) {
            if (loaded.canceled || ticket != g_openTicket || !confirmCanvasCaliperClose(owner) ||
                ticket != g_openTicket)
                return;
            resetEditingSession();
            g_hasDirtyPixels = false;
            if (loaded.document.hasCels)
                static_cast<CelStack &>(g_state) = std::move(loaded.document.stack);
            else
                g_state.image() = std::move(loaded.image);
            for (const auto &cel : g_state.cels)
                g_nextCelId = std::max(g_nextCelId, cel.id + 1);
            g_state.hasImage = !g_state.image().pixels.empty();
            g_state.imageName = path.filename().wstring();
            g_state.imagePath = loaded.native ? L"" : path.wstring();
            g_state.imagePpi = g_state.detectedPpi = clampDouble(loaded.document.layerPpi, 1, 5000);
            g_state.ppiSource = loaded.native ? L"document" : (loaded.physical.found ? L"PNG pHYs" : L"manual");
            g_state.offsetXIn = loaded.native ? loaded.document.layerOffsetXIn : 0;
            g_state.offsetYIn = loaded.native ? loaded.document.layerOffsetYIn : 0;
            if (loaded.native)
            {
                const auto &d = loaded.document;
                g_state.canvasWidthIn = d.canvasWidthIn;
                g_state.canvasHeightIn = d.canvasHeightIn;
                g_state.gridSizeIn = d.gridSizeIn;
                g_state.majorEvery = d.majorEvery;
                g_state.showGrid = d.showGrid;
                g_state.showRulers = d.showRulers;
                g_state.showLines = d.showLines;
                g_rulerUnits = d.rulerUnits;
                g_state.lines = std::move(loaded.document.lines);
                g_documentPath = path;
            }
            applyPngSpecsToState();
            if (!loaded.native)
            {
                g_state.canvasWidthIn = g_state.imageWidthIn;
                g_state.canvasHeightIn = g_state.imageHeightIn;
            }
            if (!g_state.hasImage)
            {
                g_state.imageWidthIn = g_state.canvasWidthIn;
                g_state.imageHeightIn = g_state.canvasHeightIn;
            }
            g_panX = g_panY = 0;
            g_state.zoomScale = 1;
            g_state.palette = std::move(loaded.colors);
            g_state.paletteUpdating = false;
            if (g_state.hasImage)
                setVulkanCels(g_state);
            else
                clearVulkanCanvasImage();
            rememberOpenFile(path);
            requestCanvasRender();
        },
        [restore, ticket] {
            if (restore && ticket == g_openTicket)
            {
                config["lastOpenFile"] = "";
                newCanvasCaliperImage();
            }
        });
    return true;
}

namespace
{
void createCanvasAsync(std::uint32_t width, std::uint32_t height, double ppi)
{
    if (g_openCancel)
        g_openCancel->store(true);
    const auto ticket = ++g_openTicket, revision = g_revision;
    startCpuJob(
        L"Creating canvas...",
        [width, height] {
            DecodedImage image{width, height, {}};
            image.pixels.assign(std::size_t(width) * height * 4, 255);
            return image;
        },
        [width, height, ppi, ticket, revision](DecodedImage image) {
            if (ticket != g_openTicket || (revision != g_revision && !confirmCanvasCaliperClose(g_parent)) ||
                ticket != g_openTicket)
                return;
            resetEditingSession();
            g_hasDirtyPixels = false;
            g_state.image() = std::move(image);
            g_state.hasImage = true;
            g_panX = g_panY = 0;
            g_state.zoomScale = 1;
            g_state.imagePath.clear();
            g_state.imageName = L"Untitled";
            g_state.imagePpi = g_state.detectedPpi = ppi;
            g_state.ppiSource = L"new canvas";
            g_state.imageWidthIn = g_state.canvasWidthIn = double(width) / ppi;
            g_state.imageHeightIn = g_state.canvasHeightIn = double(height) / ppi;
            g_state.offsetXIn = g_state.offsetYIn = 0;
            g_state.gridSizeIn = std::max(g_state.gridSizeIn, 1 / ppi);
            g_state.palette = {{255, 255, 255}};
            g_state.paletteUpdating = false;
            g_state.paintColor = {0, 0, 0};
            setVulkanCels(g_state);
            requestCanvasRender();
        });
}
} // namespace
void newCanvasCaliperImage()
{
    const double w = std::round(g_state.canvasWidthIn * g_state.imagePpi),
                 h = std::round(g_state.canvasHeightIn * g_state.imagePpi);
    const auto limit = std::min<std::uint32_t>(16384, vulkanDeviceProperties().limits.maxImageDimension2D);
    if (w < 1 || h < 1 || w > limit || h > limit || w * h > 64000000)
    {
        MessageBoxW(g_parent, L"Canvas dimensions exceed the image or GPU limit.", L"SIMP", MB_OK | MB_ICONWARNING);
        return;
    }
    createCanvasAsync(std::uint32_t(w), std::uint32_t(h), g_state.imagePpi);
}

void flushCanvasCaliperEdits()
{
    if (!g_hasDirtyPixels || !g_state.hasImage)
        return;
    updateVulkanCanvasRegion(g_state.image(), static_cast<std::uint32_t>(g_dirtyPixels.left),
                             static_cast<std::uint32_t>(g_dirtyPixels.top),
                             static_cast<std::uint32_t>(g_dirtyPixels.right - g_dirtyPixels.left),
                             static_cast<std::uint32_t>(g_dirtyPixels.bottom - g_dirtyPixels.top));
    g_hasDirtyPixels = false;
}

void openCanvasCaliperDocumentDialog(HWND owner)
{
    std::array<wchar_t, 32768> filename{};
    OPENFILENAMEW open{};
    open.lStructSize = sizeof(open);
    open.hwndOwner = owner;
    open.lpstrFilter = L"SIMP Documents (*.simp)\0*.simp\0"
                       L"PNG Images (*.png)\0*.png\0"
                       L"JPEG Images (*.jpg; *.jpeg)\0*.jpg;*.jpeg\0"
                       L"Windows Bitmaps (*.bmp)\0*.bmp\0"
                       L"Targa Images (*.tga)\0*.tga\0"
                       L"GIF Images (*.gif)\0*.gif\0"
                       L"Photoshop Images (*.psd)\0*.psd\0"
                       L"Radiance HDR Images (*.hdr)\0*.hdr\0"
                       L"Softimage Images (*.pic)\0*.pic\0"
                       L"Portable Anymap Images (*.pnm)\0*.pnm\0"
                       L"Portable Pixmap Images (*.ppm)\0*.ppm\0"
                       L"Portable Graymap Images (*.pgm)\0*.pgm\0"
                       L"All Files (*.*)\0*.*\0";
    open.lpstrFile = filename.data();
    open.nMaxFile = DWORD(filename.size());
    open.lpstrTitle = L"Open";
    open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&open))
        loadCanvasCaliperImagePath(owner, filename.data());
}

namespace
{
bool writeCanvasDocument(HWND owner, const std::filesystem::path &path)
{
    if (g_saving)
    {
        g_saveAgain = true;
        g_queuedSavePath = path;
        g_queuedSaveEpoch = g_epoch;
        return true;
    }
    SimpDocument document;
    document.canvasWidthIn = g_state.canvasWidthIn;
    document.canvasHeightIn = g_state.canvasHeightIn;
    document.gridSizeIn = g_state.gridSizeIn;
    document.majorEvery = g_state.majorEvery;
    document.paletteCount = g_state.paletteCount;
    document.showGrid = g_state.showGrid;
    document.showRulers = g_state.showRulers;
    document.rulerUnits = g_rulerUnits;
    document.lines = g_state.lines;
    document.showLines = g_state.showLines;
    if (g_state.hasImage)
    {
        document.hasLayer = true;
        document.layerOffsetXIn = g_state.offsetXIn;
        document.layerOffsetYIn = g_state.offsetYIn;
        document.layerPpi = g_state.imagePpi;
        document.layerName = path.filename().wstring();
        document.hasCels = true;
        document.stack = g_state;
    }
    const auto epoch = g_epoch, revision = g_revision;
    g_saving = true;
    g_savingPath = path;
    g_savingEpoch = epoch;
    startCpuJob(
        L"Saving document...", [document = std::move(document), path] { return saveSimpDocument(path, document); },
        [owner, path, epoch, revision](bool success) {
            g_saving = false;
            if (!success)
                MessageBoxW(owner, L"Could not save the document. The previous file was preserved.", L"SIMP",
                            MB_OK | MB_ICONERROR);
            else if (epoch == g_epoch)
            {
                g_documentPath = path;
                g_state.imageName = path.filename().wstring();
                rememberOpenFile(path);
                if (revision == g_revision)
                    g_documentModified = false;
                requestCanvasRender();
            }
            if (g_saveAgain)
            {
                g_saveAgain = false;
                if (g_queuedSaveEpoch == g_epoch)
                    writeCanvasDocument(owner, g_queuedSavePath);
            }
        },
        [] {
            g_saving = false;
            g_saveAgain = false;
        });
    return true;
}
} // namespace
void saveCanvasCaliperDocumentAsDialog(HWND owner)
{
    std::array<wchar_t, 32768> filename{};
    auto initial = g_documentPath.empty() ? std::filesystem::path(g_state.imageName) : g_documentPath;
    if (initial.empty())
        initial = L"Untitled";
    initial.replace_extension(L".simp");
    wcsncpy_s(filename.data(), filename.size(), initial.c_str(), _TRUNCATE);
    OPENFILENAMEW save{};
    save.lStructSize = sizeof(save);
    save.hwndOwner = owner;
    save.lpstrFilter = L"SIMP Documents (*.simp)\0*.simp\0All Files (*.*)\0*.*\0";
    save.lpstrFile = filename.data();
    save.nMaxFile = DWORD(filename.size());
    save.lpstrTitle = L"Save As";
    save.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    save.lpstrDefExt = L"simp";
    if (GetSaveFileNameW(&save))
        writeCanvasDocument(owner, filename.data());
}
void saveCanvasCaliperDocument(HWND owner)
{
    if (g_saving && g_savingEpoch == g_epoch)
        writeCanvasDocument(owner, g_savingPath);
    else if (g_documentPath.empty())
        saveCanvasCaliperDocumentAsDialog(owner);
    else
        writeCanvasDocument(owner, g_documentPath);
}
void editCanvasCaliperHistory(HWND owner, bool redo)
{
    if (g_drag.painting)
    {
        g_drag.painting = false;
        g_history.commit();
        if (GetCapture() == owner)
            ReleaseCapture();
    }
    flushCanvasCaliperEdits();
    if (redo ? g_history.redo(static_cast<CelStack &>(g_state), g_state.lines)
             : g_history.undo(static_cast<CelStack &>(g_state), g_state.lines))
    {
        const auto r = g_history.lastRegion();
        if (g_history.stackChanged())
        {
            ++g_pixelsRevision;
            setVulkanCels(g_state);
            schedulePalette();
        }
        else if (r.right > r.left && r.bottom > r.top)
        {
            if (g_history.celsChanged())
                selectVulkanCel(g_state.activeCel);
            markDirtyPixels(r.left, r.top, r.right, r.bottom);
            schedulePalette();
        }
        if (g_history.celsChanged())
            revealActiveCel();
        markDocumentModified();
        g_dragLine = -1;
        requestCanvasRender();
    }
    else if (!g_history.error().empty())
        MessageBoxA(owner, g_history.error().c_str(), "SIMP Undo", MB_OK | MB_ICONERROR);
}

void toggleCanvasView(HWND owner, UINT command)
{
    switch (command)
    {
    case IDM_VIEW_GRID:
        g_state.showGrid = !g_state.showGrid;
        break;
    case IDM_VIEW_RULERS:
        g_state.showRulers = !g_state.showRulers;
        break;
    case IDM_VIEW_LINES:
        g_state.showLines = !g_state.showLines;
        break;
    }
    updateCanvasCaliperMenu(GetMenu(owner));
    DrawMenuBar(owner);
    requestCanvasRender();
}
bool handleCanvasCaliperWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT &result)
{
    switch (message)
    {
    case kCpuReadyMessage:
        result = 0;
        return true;
    case WM_MOUSELEAVE:
        g_trackingMouse = false;
        if (std::exchange(g_state.probeValid, false))
            requestRender();
        if (g_dragLine < 0 && g_lineOptions < 0 && g_state.highlightedLine != -1)
        {
            g_state.highlightedLine = -1;
            requestRender();
        }
        result = 0;
        return true;
    case WM_SETCURSOR: {
        if (g_dragLine >= 0 && std::size_t(g_dragLine) < g_state.lines.size())
        {
            SetCursor(LoadCursorW(nullptr, g_state.lines[g_dragLine].vertical ? IDC_SIZEWE : IDC_SIZENS));
            result = TRUE;
            return true;
        }
        POINT p{};
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        const int axis = rulerAxis(g_layout, g_surfaceWidth, g_surfaceHeight, g_uiScale, g_state.showRulers, p.x, p.y);
        if (axis >= 0 && hitGuide(g_state, g_layout, axis, axis == 0 ? p.x : p.y, ui(12)) >= 0)
        {
            SetCursor(LoadCursorW(nullptr, axis == 0 ? IDC_SIZEWE : IDC_SIZENS));
            result = TRUE;
            return true;
        }
        break;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path))))
            loadCanvasCaliperImagePath(hwnd, path);
        DragFinish(drop);
        result = 0;
        return true;
    }

    case WM_MOUSEMOVE: {
        setSecondaryActive((GetKeyState(VK_MENU) & 0x8000) != 0);
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        if (!g_trackingMouse)
        {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
            g_trackingMouse = true;
        }
        updateGuideHighlight(x, y);
        if (g_dragCels)
        {
            const auto list = layoutCels(g_surfaceWidth, g_surfaceHeight, g_uiScale);
            const auto maximum = g_state.cels.size() > std::size_t(list.rows) ? g_state.cels.size() - list.rows : 0;
            const double at =
                std::clamp(double(y - list.track.top) / std::max(1, list.track.bottom - list.track.top), 0.0, 1.0);
            g_state.celOffset = std::size_t(at * maximum);
            requestRender();
            result = 0;
            return true;
        }
        if (g_dragPalette)
        {
            scrollPaletteTo(y);
            result = 0;
            return true;
        }
        if (g_dragLine >= 0 && std::size_t(g_dragLine) < g_state.lines.size())
        {
            if (wParam & MK_LBUTTON)
                moveDraggedLine(x, y);
            SetCursor(LoadCursorW(nullptr, g_state.lines[g_dragLine].vertical ? IDC_SIZEWE : IDC_SIZENS));
            result = 0;
            return true;
        }
        if (g_drag.painting)
            advanceStrokeAt(x, y);
        if (g_drag.panning)
        {
            g_panX = g_drag.startOffsetXIn + x - g_drag.startPoint.x;
            g_panY = g_drag.startOffsetYIn + y - g_drag.startPoint.y;
            requestCanvasRender();
        }
        updateProbe(x, y);
        break;
    }

    case WM_LBUTTONDOWN: {
        setSecondaryActive((GetKeyState(VK_MENU) & 0x8000) != 0);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int axis =
            rulerAxis(g_layout, g_surfaceWidth, g_surfaceHeight, g_uiScale, g_state.showRulers, point.x, point.y);
        if (axis >= 0)
        {
            g_history.commit();
            g_dragLine = hitGuide(g_state, g_layout, axis, axis == 0 ? point.x : point.y, ui(12));
            if (g_dragLine >= 0)
            {
                g_state.highlightedLine = g_dragLine;
                SetCapture(hwnd);
                requestRender();
            }
            result = 0;
            return true;
        }
        for (const auto &hit : g_uiHits)
            if (hit.rect.contains(point.x, point.y))
            {
                const int action = hit.action;
                if ((action >= UI_CEL_SELECT_FIRST && action < UI_CEL_SELECT_FIRST + int(kMaxCels)) ||
                    (action >= UI_CEL_VISIBLE_FIRST && action < UI_CEL_VISIBLE_FIRST + int(kMaxCels)) ||
                    (action >= UI_CEL_NEW && action <= UI_CEL_SCROLL))
                    handleCelAction(hwnd, action);
                else if (action >= UI_PALETTE_FIRST)
                {
                    const auto index = std::size_t(action - UI_PALETTE_FIRST);
                    if (index < g_state.palette.size())
                        editingPaintColor() = g_state.palette[index];
                }
                else if (action == UI_PALETTE_SCROLL)
                {
                    g_dragPalette = true;
                    SetCapture(hwnd);
                    scrollPaletteTo(point.y);
                }
                else if (action == UI_ORIGINAL)
                {
                    g_state.zoomScale = originalZoomScale(g_state, g_layout);
                    g_panX = g_panY = 0;
                    const auto l = computeLayout(g_surfaceWidth, g_surfaceHeight);
                    const auto area = workspaceRect();
                    g_panX = std::floor((double(area.left) + area.right - g_state.image().width) / 2) -
                             (l.originX + g_state.offsetXIn * l.pxPerIn);
                    g_panY = std::floor((double(area.top) + area.bottom - g_state.image().height) / 2) -
                             (l.originY + g_state.offsetYIn * l.pxPerIn);
                }
                else if (action == UI_ZOOM_OUT)
                    zoomCanvas(1 / kZoomStep, (workspaceRect().left + workspaceRect().right) / 2,
                               (workspaceRect().top + workspaceRect().bottom) / 2);
                else if (action == UI_ZOOM_IN)
                    zoomCanvas(kZoomStep, (workspaceRect().left + workspaceRect().right) / 2,
                               (workspaceRect().top + workspaceRect().bottom) / 2);
                else if (action == UI_FIT)
                {
                    g_state.zoomScale = 1;
                    g_panX = g_panY = 0;
                }
                else if (action == UI_BRUSH)
                    selectPaintTool(PaintTool::Brush);
                else if (action == UI_CELL)
                    selectPaintTool(PaintTool::Cell);
                else if (action == UI_PICKER)
                    selectPaintTool(PaintTool::Pick);
                else if (action == UI_COLOR || action == UI_SECONDARY_COLOR)
                {
                    choosePaintColor(hwnd, action == UI_SECONDARY_COLOR || g_state.secondaryActive);
                }
                else
                    PostMessageW(hwnd, WM_COMMAND, action, 0);
                requestCanvasRender();
                result = 0;
                return true;
            }
        if (g_state.tool == PaintTool::Pick && pointInsideImage(point.x, point.y))
        {
            updateProbe(point.x, point.y);
            if (g_state.probeValid)
                editingPaintColor() = g_state.probeColor;
            requestRender();
            result = 0;
            return true;
        }
        int cellX = 0;
        int cellY = 0;
        if (!g_drag.panning && pointToPaintPosition(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), cellX, cellY))
        {
            g_drag.painting = true;
            g_drag.havePosition = true;
            g_drag.lastX = cellX;
            g_drag.lastY = cellY;
            SetCapture(hwnd);
            paintPosition(cellX, cellY);
            requestRender();
            result = 0;
            return true;
        }
        break;
    }

    case WM_LBUTTONDBLCLK: {
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        const int axis = rulerAxis(g_layout, g_surfaceWidth, g_surfaceHeight, g_uiScale, g_state.showRulers, x, y);
        if (axis >= 0)
        {
            const double pixel = ((axis == 0 ? x : y) - (axis == 0 ? g_layout.originX : g_layout.originY)) /
                                 g_layout.pxPerIn * g_state.imagePpi;
            if (std::abs(pixel) <= 1000000000.0)
            {
                g_dragLine = -1;
                ReleaseCapture();
                g_history.commit();
                g_history.captureGuides(g_state.lines);
                GuideLine line;
                line.vertical = axis == 0;
                line.position = pixel;
                g_state.lines.push_back(line);
                g_state.highlightedLine = int(g_state.lines.size() - 1);
                g_history.commit();
                markDocumentModified();
                requestCanvasRender();
            }
            result = 0;
            return true;
        }
        return handleCanvasCaliperWindowMessage(hwnd, WM_LBUTTONDOWN, wParam, lParam, result);
    }

    case WM_LBUTTONUP:
        if (g_dragCels)
        {
            g_dragCels = false;
            ReleaseCapture();
            result = 0;
            return true;
        }
        if (g_dragPalette)
        {
            g_dragPalette = false;
            ReleaseCapture();
            result = 0;
            return true;
        }
        if (g_dragLine >= 0)
        {
            moveDraggedLine(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            g_dragLine = -1;
            updateGuideHighlight(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            g_history.commit();
            ReleaseCapture();
            result = 0;
            return true;
        }
        if (g_drag.painting)
        {
            advanceStrokeAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            g_drag.painting = false;
            g_history.commit();
            schedulePalette();
            ReleaseCapture();
            result = 0;
            return true;
        }
        break;

    case WM_RBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const int axis = rulerAxis(g_layout, g_surfaceWidth, g_surfaceHeight, g_uiScale, g_state.showRulers, x, y);
        if (axis >= 0)
        {
            const int index = hitGuide(g_state, g_layout, axis, axis == 0 ? x : y, ui(12));
            if (index >= 0)
                showLineDialog(hwnd, index);
            result = 0;
            return true;
        }
        if (!g_drag.painting && insideWorkspace(x, y))
        {
            g_drag.panning = true;
            g_drag.startPoint = {x, y};
            g_drag.startOffsetXIn = g_panX;
            g_drag.startOffsetYIn = g_panY;
            SetCapture(hwnd);
            result = 0;
            return true;
        }
        break;
    }

    case WM_RBUTTONUP:
        if (g_drag.panning)
        {
            g_drag.panning = false;
            if (message == WM_RBUTTONUP)
                ReleaseCapture();
            result = 0;
            return true;
        }
        break;

    case WM_CAPTURECHANGED:
        g_dragCels = false;
        g_dragPalette = false;
        if (g_drag.painting)
            schedulePalette();
        g_dragLine = -1;
        {
            POINT p{};
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            updateGuideHighlight(p.x, p.y);
        }
        g_history.commit();
        g_drag.painting = false;
        g_drag.panning = false;
        break;

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &p);
        if ((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0)
        {
            zoomCanvas(std::pow(kZoomStep, double(delta) / WHEEL_DELTA), p.x, p.y);
            result = 0;
            return true;
        }
        if (g_state.tool == PaintTool::Brush)
        {
            g_brushWheelDelta += delta;
            const int steps = g_brushWheelDelta / WHEEL_DELTA;
            g_brushWheelDelta %= WHEEL_DELTA;
            g_state.brushSize = std::clamp(g_state.brushSize + steps, 1, kMaxBrushSize);
            requestRender();
            result = 0;
            return true;
        }
        const auto list = layoutCels(g_surfaceWidth, g_surfaceHeight, g_uiScale);
        if (list.bounds.contains(p.x, p.y))
        {
            const auto maximum = g_state.cels.size() > std::size_t(list.rows) ? g_state.cels.size() - list.rows : 0;
            g_state.celOffset = std::size_t(
                std::clamp<std::int64_t>(std::int64_t(g_state.celOffset) - delta / WHEEL_DELTA, 0, maximum));
            requestRender();
            result = 0;
            return true;
        }
        const auto layout = layoutPalette(g_surfaceWidth, g_uiScale, g_state.palette.size(), g_surfaceHeight);
        if (layout.bounds.contains(p.x, p.y))
        {
            const auto delta = std::int64_t(GET_WHEEL_DELTA_WPARAM(wParam)) * layout.columns * 3 / WHEEL_DELTA;
            const auto next = std::max<std::int64_t>(0, std::int64_t(g_state.paletteOffset) - delta);
            g_state.paletteOffset = clampPaletteOffset(layout, g_state.palette.size(), std::size_t(next));
            requestRender();
            result = 0;
            return true;
        }
        break;
    }

    case WM_KILLFOCUS:
        setSecondaryActive(false);
        if (g_drag.painting)
        {
            g_drag.painting = false;
            g_history.commit();
            schedulePalette();
        }
        if (GetCapture() == hwnd)
            ReleaseCapture();
        break;
    case WM_SETFOCUS:
        setSecondaryActive((GetKeyState(VK_MENU) & 0x8000) != 0);
        break;
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_KEYUP:
        if (wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU)
        {
            setSecondaryActive(message == WM_SYSKEYDOWN);
            result = 0;
            return true;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU)
        {
            setSecondaryActive(true);
            result = 0;
            return true;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) == 0)
        {
            if (wParam == 'B')
                selectPaintTool(PaintTool::Brush);
            else if (wParam == 'C')
                selectPaintTool(PaintTool::Cell);
            else if (wParam == 'I')
                selectPaintTool(PaintTool::Pick);
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        {
            if (wParam == 'Z' || wParam == 'Y')
            {
                editCanvasCaliperHistory(hwnd, wParam == 'Y' || (GetKeyState(VK_SHIFT) & 0x8000));
                result = 0;
                return true;
            }
            if (wParam == 'N')
            {
                PostMessageW(hwnd, WM_COMMAND, IDM_FILE_NEW, 0);
                return true;
            }
            if (wParam == 'O')
            {
                PostMessageW(hwnd, WM_COMMAND, IDM_FILE_OPEN, 0);
                return true;
            }
            if (wParam == 'S')
            {
                PostMessageW(hwnd, WM_COMMAND,
                             (GetKeyState(VK_SHIFT) & 0x8000) ? IDM_FILE_SAVE_DOCUMENT_AS : IDM_FILE_SAVE, 0);
                return true;
            }
            if (wParam == '0')
            {
                g_state.zoomScale = 1.0;
                g_panX = g_panY = 0;
                requestCanvasRender();
                result = 0;
                return true;
            }
            if (wParam == VK_OEM_PLUS || wParam == VK_ADD)
            {
                zoomCanvas(kZoomStep, (workspaceRect().left + workspaceRect().right) / 2,
                           (workspaceRect().top + workspaceRect().bottom) / 2);
                requestCanvasRender();
                result = 0;
                return true;
            }
            if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT)
            {
                zoomCanvas(1 / kZoomStep, (workspaceRect().left + workspaceRect().right) / 2,
                           (workspaceRect().top + workspaceRect().bottom) / 2);
                requestCanvasRender();
                result = 0;
                return true;
            }
        }
        break;

    case WM_CONTEXTMENU:
        if (g_state.hasImage)
        {
            result = 0;
            return true;
        }
        break;
    }

    return false;
}

void updateCanvasCaliperMenu(HMENU menu)
{
    if (!menu)
        return;

    EnableMenuItem(menu, IDM_EDIT_UNDO, MF_BYCOMMAND | (g_history.canUndo() ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, IDM_EDIT_REDO, MF_BYCOMMAND | (g_history.canRedo() ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, IDM_FILE_EXPORT_PNG, MF_BYCOMMAND | (g_state.hasImage ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(menu, IDM_VIEW_LINES, MF_BYCOMMAND | (g_state.showLines ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_VIEW_GRID, MF_BYCOMMAND | (g_state.showGrid ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_VIEW_RULERS, MF_BYCOMMAND | (g_state.showRulers ? MF_CHECKED : MF_UNCHECKED));
}

CanvasRenderData &buildCanvasRenderData(std::uint32_t width, std::uint32_t height)
{
    g_surfaceWidth = width;
    g_surfaceHeight = height;

    static CanvasRenderData data;
    data.surfaceWidth = width;
    data.surfaceHeight = height;
    data.hasImage = false;
    data.overlays.clear();
    data.overlays.reserve(12000);
    g_layout = computeLayout(width, height);

    if (g_state.hasImage)
    {
        data.hasImage = true;
        data.imageX = static_cast<float>(g_layout.originX + g_state.offsetXIn * g_layout.pxPerIn);
        data.imageY = static_cast<float>(g_layout.originY + g_state.offsetYIn * g_layout.pxPerIn);
        data.imageWidth = static_cast<float>(g_state.imageWidthIn * g_layout.pxPerIn);
        data.imageHeight = static_cast<float>(g_state.imageHeightIn * g_layout.pxPerIn);
    }

    data.viewport = workspaceRect();
    EditorUi chrome{g_state,
                    g_layout,
                    int(width),
                    int(height),
                    g_rulerUnits,
                    g_uiScale,
                    vulkanDeviceProperties().deviceName,
                    std::move(g_uiHits)};
    chrome.build(data);
    g_uiHits = std::move(chrome.hits);
    return data;
}

namespace
{
bool dialogNumber(HWND dialog, int id, double low, double high, double &value)
{
    wchar_t buffer[96]{};
    GetDlgItemTextW(dialog, id, buffer, 96);
    wchar_t *end = nullptr;
    value = std::wcstod(buffer, &end);
    while (end && (*end == L' ' || *end == L'\t'))
        ++end;
    if (end == buffer || !end || *end || !std::isfinite(value) || value < low || value > high)
    {
        const std::wstring message = L"Enter a number from " + formatDouble(low) + L" to " + formatDouble(high) + L".";
        MessageBoxW(dialog, message.c_str(), L"Check setting", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, id));
        SendDlgItemMessageW(dialog, id, EM_SETSEL, 0, -1);
        return false;
    }
    return true;
}
struct SettingsContext
{
    UINT command;
    int gridUnits;
};
INT_PTR CALLBACK SettingsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *context = reinterpret_cast<SettingsContext *>(GetWindowLongPtrW(dialog, DWLP_USER));
    const UINT command = context ? context->command : 0;
    if (message == WM_INITDIALOG)
    {
        context = reinterpret_cast<SettingsContext *>(lParam);
        const UINT mode = context->command;
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        const HWND combo = GetDlgItem(dialog, IDC_SETTING_UNITS);
        if (combo)
        {
            for (const wchar_t *label : {L"Pixels", L"Inches", L"Centimeters"})
                SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendMessageW(combo, CB_SETCURSEL, g_rulerUnits, 0);
        }
        if (mode == IDM_OPTIONS_GRID)
        {
            SetDlgItemTextW(
                dialog, IDC_SETTING_VALUE,
                formatNumber(g_state.gridSizeIn * rulerUnitsPerInch(g_rulerUnits, g_state.imagePpi)).c_str());
            SetDlgItemInt(dialog, IDC_SETTING_MAJOR, g_state.majorEvery, FALSE);
            CheckDlgButton(dialog, IDC_SETTING_VISIBLE, g_state.showGrid ? BST_CHECKED : BST_UNCHECKED);
        }
        else if (mode == IDM_OPTIONS_RULERS)
            CheckDlgButton(dialog, IDC_SETTING_VISIBLE, g_state.showRulers ? BST_CHECKED : BST_UNCHECKED);
        else
        {
            SetDlgItemTextW(dialog, IDC_SETTING_VALUE, formatDouble(g_state.imagePpi).c_str());
            SetDlgItemInt(dialog, IDC_SETTING_WIDTH, g_state.image().width, FALSE);
            SetDlgItemInt(dialog, IDC_SETTING_HEIGHT, g_state.image().height, FALSE);
            const auto info = formatInt(g_state.image().width) + L" x " + formatInt(g_state.image().height) +
                              L" pixels\n" + formatDouble(g_state.imageWidthIn) + L" x " +
                              formatDouble(g_state.imageHeightIn) + L" inches";
            SetDlgItemTextW(dialog, IDC_SETTING_INFO, info.c_str());
        }
        RECT owner{}, r{};
        GetWindowRect(GetParent(dialog), &owner);
        GetWindowRect(dialog, &r);
        SetWindowPos(dialog, nullptr, owner.left + (owner.right - owner.left - r.right + r.left) / 2,
                     owner.top + (owner.bottom - owner.top - r.bottom + r.top) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        return TRUE;
    }
    if (message == WM_COMMAND)
    {
        if (context && command == IDM_OPTIONS_GRID && LOWORD(wParam) == IDC_SETTING_UNITS &&
            HIWORD(wParam) == CBN_SELCHANGE)
        {
            wchar_t buffer[96]{};
            GetDlgItemTextW(dialog, IDC_SETTING_VALUE, buffer, 96);
            const double value = std::wcstod(buffer, nullptr);
            const int next = static_cast<int>(SendDlgItemMessageW(dialog, IDC_SETTING_UNITS, CB_GETCURSEL, 0, 0));
            const auto factor = [](int units) { return units == 0 ? g_state.imagePpi : (units == 2 ? 2.54 : 1.0); };
            if (std::isfinite(value) && value > 0)
            {
                std::swprintf(buffer, 96, L"%.9g", value / factor(context->gridUnits) * factor(next));
                SetDlgItemTextW(dialog, IDC_SETTING_VALUE, buffer);
            }
            context->gridUnits = next;
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(dialog, 0);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK)
        {
            double value = 0, major = 0, w = 0, h = 0;
            const int units = static_cast<int>(SendDlgItemMessageW(dialog, IDC_SETTING_UNITS, CB_GETCURSEL, 0, 0));
            if (command == IDM_OPTIONS_GRID)
            {
                if (!dialogNumber(dialog, IDC_SETTING_VALUE, 0.000001, 10000000, value) ||
                    !dialogNumber(dialog, IDC_SETTING_MAJOR, 0, 10000, major))
                    return TRUE;
                const double factor = units == 0 ? g_state.imagePpi : (units == 2 ? 2.54 : 1.0);
                const double spacing = value / factor;
                if (spacing * g_state.imagePpi < 0.999999 || spacing > 10000 || major != std::floor(major))
                {
                    MessageBoxW(
                        dialog,
                        L"Use a spacing of at least one image pixel, up to 10,000 inches, and a whole number of cells.",
                        L"Check grid", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                g_state.gridSizeIn = spacing;
                g_state.majorEvery = static_cast<int>(major);
                g_state.showGrid = IsDlgButtonChecked(dialog, IDC_SETTING_VISIBLE) == BST_CHECKED;
            }
            else if (command == IDM_OPTIONS_RULERS)
            {
                g_rulerUnits = std::clamp(units, 0, 2);
                g_state.showRulers = IsDlgButtonChecked(dialog, IDC_SETTING_VISIBLE) == BST_CHECKED;
            }
            else
            {
                if (!dialogNumber(dialog, IDC_SETTING_VALUE, 1, 5000, value))
                    return TRUE;
                if (command == IDM_FILE_NEW)
                {
                    if (!dialogNumber(dialog, IDC_SETTING_WIDTH, 1, 16384, w) ||
                        !dialogNumber(dialog, IDC_SETTING_HEIGHT, 1, 16384, h))
                        return TRUE;
                    if (w != std::floor(w) || h != std::floor(h) || w * h > 64000000 ||
                        w > vulkanDeviceProperties().limits.maxImageDimension2D ||
                        h > vulkanDeviceProperties().limits.maxImageDimension2D)
                    {
                        MessageBoxW(dialog,
                                    L"Use whole pixel dimensions totaling at most 64 million pixels, within the GPU "
                                    L"texture limit.",
                                    L"Check canvas size", MB_OK | MB_ICONWARNING);
                        return TRUE;
                    }
                    createCanvasAsync(std::uint32_t(w), std::uint32_t(h), value);
                }
                else
                {
                    if (!changeImagePpi(g_state, value))
                    {
                        MessageBoxW(dialog, L"This PPI would exceed the supported physical canvas or grid bounds.",
                                    L"Check PPI", MB_OK | MB_ICONWARNING);
                        return TRUE;
                    }
                    markDocumentModified();
                }
            }
            updateCanvasCaliperMenu(GetMenu(g_parent));
            saveCanvasCaliperConfig();
            requestCanvasRender();
            EndDialog(dialog, IDOK);
            return TRUE;
        }
    }
    return FALSE;
}
} // namespace
void showCanvasSettingsDialog(HWND owner, UINT command)
{
    const int resource = command == IDM_OPTIONS_GRID     ? IDD_GRID_SETTINGS
                         : command == IDM_OPTIONS_RULERS ? IDD_RULER_SETTINGS
                         : command == IDM_FILE_NEW       ? IDD_NEW_CANVAS
                                                         : IDD_IMAGE_SETTINGS;
    g_history.commit();
    if (command == IDM_FILE_NEW && !confirmCanvasCaliperClose(owner))
        return;
    SettingsContext context{command, g_rulerUnits};
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resource), owner, SettingsProc,
                    reinterpret_cast<LPARAM>(&context));
    requestRender();
}

bool confirmCanvasCaliperClose(HWND owner)
{
    if (!g_documentModified)
        return true;
    if (g_confirming)
        return false;
    g_confirming = true;
    const int answer = MessageBoxW(owner, L"Save changes to this image?", L"SIMP", MB_YESNOCANCEL | MB_ICONQUESTION);
    bool proceed = answer == IDNO;
    if (answer == IDYES)
    {
        saveCanvasCaliperDocument(owner);
        const bool enabled = IsWindowEnabled(owner) != FALSE;
        if (enabled)
            EnableWindow(owner, FALSE);
        while (g_saving)
        {
            if (!processEvents())
            {
                PostQuitMessage(0);
                break;
            }
            if (shouldRender())
                renderFrame();
            if (g_saving)
                MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        if (enabled)
            EnableWindow(owner, TRUE);
        proceed = !g_documentModified;
    }
    g_confirming = false;
    return proceed;
}

namespace
{
void positionDialogAtCursor(HWND dialog)
{
    POINT point{};
    GetCursorPos(&point);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT r{};
    GetWindowRect(dialog, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int x = std::clamp<int>(point.x + 12, monitor.rcWork.left,
                                  std::max<int>(monitor.rcWork.left, monitor.rcWork.right - w));
    const int y =
        std::clamp<int>(point.y + 12, monitor.rcWork.top, std::max<int>(monitor.rcWork.top, monitor.rcWork.bottom - h));
    SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}
INT_PTR CALLBACK CelNameProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INITDIALOG)
    {
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        SendDlgItemMessageW(dialog, IDC_CEL_NAME, EM_SETLIMITTEXT, 128, 0);
        SetDlgItemTextW(dialog, IDC_CEL_NAME, reinterpret_cast<std::wstring *>(lParam)->c_str());
        SendDlgItemMessageW(dialog, IDC_CEL_NAME, EM_SETSEL, 0, -1);
        positionDialogAtCursor(dialog);
        return TRUE;
    }
    if (message == WM_COMMAND)
    {
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK)
        {
            wchar_t name[129]{};
            GetDlgItemTextW(dialog, IDC_CEL_NAME, name, 129);
            std::wstring value = name;
            const auto first = value.find_first_not_of(L" \t\r\n"), last = value.find_last_not_of(L" \t\r\n");
            if (first == std::wstring::npos)
            {
                SetFocus(GetDlgItem(dialog, IDC_CEL_NAME));
                return TRUE;
            }
            *reinterpret_cast<std::wstring *>(GetWindowLongPtrW(dialog, DWLP_USER)) =
                value.substr(first, last - first + 1);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
    }
    return FALSE;
}
void renameActiveCel(HWND owner)
{
    auto name = g_state.cels[g_state.activeCel].name;
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_CEL_NAME), owner, CelNameProc,
                        reinterpret_cast<LPARAM>(&name)) == IDOK &&
        name != g_state.cels[g_state.activeCel].name)
    {
        g_history.captureCels(g_state);
        g_state.cels[g_state.activeCel].name = std::move(name);
        g_history.commit();
        markDocumentModified();
        requestCanvasRender();
    }
}
struct LineDialogState
{
    GuideLine line;
    bool remove = false;
};
INT_PTR CALLBACK LineSettingsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *state = reinterpret_cast<LineDialogState *>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG)
    {
        state = reinterpret_cast<LineDialogState *>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        SetDlgItemTextW(dialog, IDC_SETTING_VALUE, formatNumber(state->line.thickness).c_str());
        const PaletteColor color{GetRValue(state->line.color), GetGValue(state->line.color),
                                 GetBValue(state->line.color)};
        SetDlgItemTextW(dialog, IDC_LINE_COLOR, formatHex(color).c_str());
        positionDialogAtCursor(dialog);
        return TRUE;
    }
    if (message == WM_COMMAND && state)
    {
        switch (LOWORD(wParam))
        {
        case IDC_LINE_COLOR: {
            static COLORREF custom[16]{};
            CHOOSECOLORW choice{};
            choice.lStructSize = sizeof(choice);
            choice.hwndOwner = dialog;
            choice.rgbResult = state->line.color;
            choice.lpCustColors = custom;
            choice.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&choice))
            {
                state->line.color = choice.rgbResult;
                const PaletteColor color{GetRValue(choice.rgbResult), GetGValue(choice.rgbResult),
                                         GetBValue(choice.rgbResult)};
                SetDlgItemTextW(dialog, IDC_LINE_COLOR, formatHex(color).c_str());
            }
            return TRUE;
        }
        case IDC_LINE_DELETE:
            state->remove = true;
            EndDialog(dialog, IDOK);
            return TRUE;
        case IDOK: {
            double thickness = 0;
            if (!dialogNumber(dialog, IDC_SETTING_VALUE, 1, 256, thickness))
                return TRUE;
            state->line.thickness = float(thickness);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}
void showLineDialog(HWND owner, int index)
{
    if (index < 0 || std::size_t(index) >= g_state.lines.size())
        return;
    g_lineOptions = index;
    g_state.highlightedLine = index;
    requestRender();
    if (shouldRender())
        renderFrame();
    const auto old = g_state.lines[index];
    LineDialogState state{old};
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_LINE_SETTINGS), owner, LineSettingsProc,
                        reinterpret_cast<LPARAM>(&state)) == IDOK &&
        (state.remove || state.line != old))
    {
        g_history.commit();
        g_history.captureGuides(g_state.lines);
        if (state.remove)
            g_state.lines.erase(g_state.lines.begin() + index);
        else
            g_state.lines[index] = state.line;
        g_history.commit();
        markDocumentModified();
        requestCanvasRender();
    }
    g_lineOptions = -1;
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(owner, &cursor);
    updateGuideHighlight(cursor.x, cursor.y);
    requestRender();
}
INT_PTR CALLBACK PngExportProc(HWND dialog, UINT message, WPARAM wParam, LPARAM)
{
    if (message == WM_INITDIALOG)
    {
        auto path = g_documentPath.empty() ? std::filesystem::path(g_state.imageName) : g_documentPath;
        if (path.empty())
            path = L"Untitled";
        path.replace_extension(L".png");
        SendDlgItemMessageW(dialog, IDC_EXPORT_PATH, EM_SETLIMITTEXT, 32767, 0);
        SetDlgItemTextW(dialog, IDC_EXPORT_PATH, path.c_str());
        SendDlgItemMessageW(dialog, IDC_EXPORT_FILTER, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(L"Adaptive (smaller files)"));
        SendDlgItemMessageW(dialog, IDC_EXPORT_FILTER, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(L"None (faster encoding)"));
        SendDlgItemMessageW(dialog, IDC_EXPORT_FILTER, CB_SETCURSEL, 0, 0);
        positionDialogAtCursor(dialog);
        return TRUE;
    }
    if (message == WM_COMMAND)
    {
        const auto command = LOWORD(wParam);
        if (command == IDCANCEL)
        {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (command == IDC_EXPORT_BROWSE)
        {
            std::array<wchar_t, 32768> name{};
            GetDlgItemTextW(dialog, IDC_EXPORT_PATH, name.data(), int(name.size()));
            OPENFILENAMEW save{};
            save.lStructSize = sizeof(save);
            save.hwndOwner = dialog;
            save.lpstrFilter = L"PNG Images (*.png)\0*.png\0";
            save.lpstrFile = name.data();
            save.nMaxFile = DWORD(name.size());
            save.lpstrDefExt = L"png";
            save.lpstrTitle = L"Export to PNG";
            save.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetSaveFileNameW(&save))
                SetDlgItemTextW(dialog, IDC_EXPORT_PATH, name.data());
            return TRUE;
        }
        if (command == IDOK)
        {
            std::array<wchar_t, 32768> name{};
            GetDlgItemTextW(dialog, IDC_EXPORT_PATH, name.data(), int(name.size()));
            if (!name[0])
            {
                SetFocus(GetDlgItem(dialog, IDC_EXPORT_PATH));
                return TRUE;
            }
            std::filesystem::path path(name.data());
            if (_wcsicmp(path.extension().c_str(), L".png"))
            {
                path.replace_extension(L".png");
                SetDlgItemTextW(dialog, IDC_EXPORT_PATH, path.c_str());
            }
            std::error_code error;
            if (!g_documentPath.empty() && std::filesystem::equivalent(path, g_documentPath, error))
            {
                MessageBoxW(dialog, L"Choose a different file. PNG export cannot replace the open native document.",
                            L"Export to PNG", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            error.clear();
            if (std::filesystem::exists(path, error) &&
                MessageBoxW(dialog, L"Replace the existing PNG file?", L"Export to PNG", MB_YESNO | MB_ICONQUESTION) !=
                    IDYES)
                return TRUE;
            const bool adaptive = SendDlgItemMessageW(dialog, IDC_EXPORT_FILTER, CB_GETCURSEL, 0, 0) == 0;
            const bool interlace = IsDlgButtonChecked(dialog, IDC_EXPORT_INTERLACE) == BST_CHECKED;
            const CelStack snapshot = g_state;
            const double ppi = g_state.imagePpi;
            const HWND owner = g_parent;
            startCpuJob(
                L"Exporting PNG...",
                [snapshot, path, ppi, interlace, adaptive] {
                    return encodePngFileBGRA(path, compositeCels(snapshot), ppi, interlace, adaptive);
                },
                [owner](bool success) {
                    if (!success)
                        MessageBoxW(owner, L"Could not export PNG. Check the path and available disk space.", L"SIMP",
                                    MB_OK | MB_ICONERROR);
                });
            EndDialog(dialog, IDOK);
            return TRUE;
        }
    }
    return FALSE;
}
} // namespace
void exportCanvasCaliperPngDialog(HWND owner)
{
    if (g_state.hasImage)
        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PNG_EXPORT), owner, PngExportProc, 0);
    requestRender();
}

void serviceCanvasBackgroundWork()
{
    if (g_shutdown)
        return;
    // A completion may pump messages while waiting for a save. Leave later
    // completions accessible to that nested pump instead of hiding a batch.
    for (;;)
    {
        std::function<void()> done;
        {
            std::lock_guard lock(g_completionMutex);
            if (g_completions.empty() || g_shutdown)
                return;
            done = std::move(g_completions.front());
            g_completions.pop_front();
        }
        done();
    }
}
