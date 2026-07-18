#include "app.h"

#include "config.h"
#include "menu.h"
#include "system.h"
#include "vulkan.h"
#include "window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace
{
constexpr int kHeaderHeight = 32;
constexpr int kSnapDistance = 28;
constexpr int kLeftPanelWidth = 244;
constexpr int kRightPanelWidth = 360;
constexpr int kFloatingLeftHeight = 690;
constexpr int kFloatingRightHeight = 650;
constexpr int kRulerThickness = 28;
constexpr double kMaxZoom = 32.0;
constexpr double kZoomStep = 1.25;

enum ControlId : UINT
{
    IDC_CC_IMAGE_WIDTH = 51001,
    IDC_CC_IMAGE_HEIGHT,
    IDC_CC_IMAGE_PPI,
    IDC_CC_CANVAS_WIDTH,
    IDC_CC_CANVAS_HEIGHT,
    IDC_CC_GRID_SIZE,
    IDC_CC_MAJOR_EVERY,
    IDC_CC_OFFSET_X,
    IDC_CC_OFFSET_Y,
    IDC_CC_ZOOM,
    IDC_CC_PALETTE_COUNT,
    IDC_CC_OPEN,
    IDC_CC_USE_PNG,
    IDC_CC_RESET_OFFSET,
    IDC_CC_FIT_VIEW,
    IDC_CC_ZOOM_OUT,
    IDC_CC_ZOOM_IN,
    IDC_CC_CLEAR,
    IDC_CC_SAVE
};

enum class ToolbarKind
{
    Left,
    Right
};

enum class ToolbarDock
{
    Floating,
    Left,
    Right
};

struct PngPhysicalInfo
{
    bool found = false;
    double ppiX = 0.0;
    double ppiY = 0.0;
};

struct PaletteColor
{
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint32_t count = 0;
};

struct CanvasState
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
    bool hasImage = false;
    std::wstring imagePath;
    std::wstring imageName;
    double detectedPpi = 100.0;
    std::wstring ppiSource = L"manual";
    DecodedImage image;
    std::vector<PaletteColor> palette;
    bool probeValid = false;
    PaletteColor probeColor;
    std::wstring probeText = L"Move over image";
    std::wstring cursorSummary = L"-";
};

struct CanvasLayout
{
    double originX = 0.0;
    double originY = 0.0;
    double fitPxPerIn = 1.0;
    double pxPerIn = 1.0;
    double contentMinXIn = 0.0;
    double contentMinYIn = 0.0;
    double contentMaxXIn = 1.0;
    double contentMaxYIn = 1.0;
    int leftInset = 0;
    int rightInset = 0;
};

struct ToolPanel
{
    ToolbarKind kind = ToolbarKind::Left;
    ToolbarDock dock = ToolbarDock::Floating;
    HWND hwnd = nullptr;
    SIZE preferredSize{};
    RECT floatingRect{};
    bool dragging = false;
    POINT dragStartScreen{};
    RECT dragStartRect{};
};

struct LeftControls
{
    HWND status = nullptr;
    HWND pixels = nullptr;
    HWND ppi = nullptr;
    HWND sourceSize = nullptr;
    HWND imageSummary = nullptr;
    HWND canvasSummary = nullptr;
    HWND gridSummary = nullptr;
    HWND cursorSummary = nullptr;
    HWND displaySummary = nullptr;
    HWND imageWidth = nullptr;
    HWND imageHeight = nullptr;
    HWND imagePpi = nullptr;
    HWND canvasWidth = nullptr;
    HWND canvasHeight = nullptr;
    HWND gridSize = nullptr;
    HWND majorEvery = nullptr;
    HWND offsetX = nullptr;
    HWND offsetY = nullptr;
    HWND zoom = nullptr;
    HWND paletteCount = nullptr;
};

struct DragState
{
    bool panning = false;
    POINT startPoint{};
    double startOffsetXIn = 0.0;
    double startOffsetYIn = 0.0;
};

CanvasState g_state;
CanvasLayout g_layout;
ToolPanel g_leftPanel{ToolbarKind::Left, ToolbarDock::Left, nullptr, {kLeftPanelWidth, kFloatingLeftHeight}, {0, 0, kLeftPanelWidth, kFloatingLeftHeight}};
ToolPanel g_rightPanel{ToolbarKind::Right, ToolbarDock::Right, nullptr, {kRightPanelWidth, kFloatingRightHeight}, {0, 0, kRightPanelWidth, kFloatingRightHeight}};
LeftControls g_controls;
DragState g_drag;
HWND g_parent = nullptr;
HFONT g_uiFont = nullptr;
HBRUSH g_panelBrush = nullptr;
HBRUSH g_editBrush = nullptr;
bool g_syncingControls = false;
bool g_registeredPanelClass = false;

const wchar_t* kPanelClassName = L"SimpCanvasCaliperToolPanel";

double clampDouble(double value, double minimum, double maximum)
{
    return std::min(maximum, std::max(minimum, value));
}

double snapQuarter(double value)
{
    return std::round(value * 4.0) / 4.0;
}

std::wstring formatDouble(double value, int precision = 2)
{
    wchar_t buffer[64] = {};
    std::swprintf(buffer, std::size(buffer), precision == 3 ? L"%.3f" : L"%.2f", value);
    return buffer;
}

std::wstring formatInt(int value)
{
    wchar_t buffer[32] = {};
    std::swprintf(buffer, std::size(buffer), L"%d", value);
    return buffer;
}

std::wstring formatHex(const PaletteColor& color)
{
    wchar_t buffer[16] = {};
    std::swprintf(buffer, std::size(buffer), L"#%02X%02X%02X", color.red, color.green, color.blue);
    return buffer;
}

std::wstring filenameOnly(const std::filesystem::path& path)
{
    return path.filename().wstring();
}

bool isPngPath(const std::filesystem::path& path)
{
    const std::wstring extension = path.extension().wstring();
    return _wcsicmp(extension.c_str(), L".png") == 0;
}

std::uint32_t readBigEndian32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
        static_cast<std::uint32_t>(bytes[offset + 3]);
}

PngPhysicalInfo parsePngPhysicalInfo(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};

    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());

    constexpr std::uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < sizeof(signature) || !std::equal(std::begin(signature), std::end(signature), bytes.begin()))
        return {};

    std::size_t offset = sizeof(signature);
    while (offset + 12 <= bytes.size())
    {
        const std::uint32_t length = readBigEndian32(bytes, offset);
        if (offset + 12u + length > bytes.size())
            break;

        const char type0 = static_cast<char>(bytes[offset + 4]);
        const char type1 = static_cast<char>(bytes[offset + 5]);
        const char type2 = static_cast<char>(bytes[offset + 6]);
        const char type3 = static_cast<char>(bytes[offset + 7]);
        if (type0 == 'p' && type1 == 'H' && type2 == 'Y' && type3 == 's' && length >= 9)
        {
            const std::size_t data = offset + 8;
            const std::uint32_t ppmX = readBigEndian32(bytes, data);
            const std::uint32_t ppmY = readBigEndian32(bytes, data + 4);
            const std::uint8_t unit = bytes[data + 8];
            if (unit == 1 && ppmX > 0 && ppmY > 0)
                return {true, static_cast<double>(ppmX) * 0.0254, static_cast<double>(ppmY) * 0.0254};
        }

        offset += 12u + length;
    }

    return {};
}

double jsonDouble(const nlohmann::json& object, const char* key, double fallback)
{
    if (!object.contains(key) || !object[key].is_number())
        return fallback;
    return object[key].get<double>();
}

int jsonInt(const nlohmann::json& object, const char* key, int fallback)
{
    if (!object.contains(key) || !object[key].is_number_integer())
        return fallback;
    return object[key].get<int>();
}

bool jsonBool(const nlohmann::json& object, const char* key, bool fallback)
{
    if (!object.contains(key) || !object[key].is_boolean())
        return fallback;
    return object[key].get<bool>();
}

ToolbarDock dockFromString(const std::string& value, ToolbarDock fallback)
{
    if (value == "left")
        return ToolbarDock::Left;
    if (value == "right")
        return ToolbarDock::Right;
    if (value == "floating")
        return ToolbarDock::Floating;
    return fallback;
}

std::string dockToString(ToolbarDock dock)
{
    switch (dock)
    {
    case ToolbarDock::Left:
        return "left";
    case ToolbarDock::Right:
        return "right";
    case ToolbarDock::Floating:
    default:
        return "floating";
    }
}

void setControlText(HWND hwnd, const std::wstring& text)
{
    if (hwnd)
        SetWindowTextW(hwnd, text.c_str());
}

double readControlDouble(HWND hwnd, double fallback, double minimum, double maximum)
{
    wchar_t buffer[128] = {};
    if (!hwnd || GetWindowTextW(hwnd, buffer, static_cast<int>(std::size(buffer))) <= 0)
        return fallback;

    wchar_t* end = nullptr;
    const double value = std::wcstod(buffer, &end);
    if (end == buffer || !std::isfinite(value))
        return fallback;

    return clampDouble(value, minimum, maximum);
}

int readControlInt(HWND hwnd, int fallback, int minimum, int maximum)
{
    wchar_t buffer[128] = {};
    if (!hwnd || GetWindowTextW(hwnd, buffer, static_cast<int>(std::size(buffer))) <= 0)
        return fallback;

    wchar_t* end = nullptr;
    const long value = std::wcstol(buffer, &end, 10);
    if (end == buffer)
        return fallback;

    return static_cast<int>(std::clamp<long>(value, minimum, maximum));
}

void setChildFont(HWND hwnd)
{
    if (hwnd && g_uiFont)
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
}

HWND makeStatic(HWND parent, const wchar_t* text, int x, int y, int width, int height, UINT id = 0)
{
    HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    setChildFont(hwnd);
    return hwnd;
}

HWND makeEdit(HWND parent, UINT id, int x, int y, int width, int height)
{
    HWND hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    setChildFont(hwnd);
    SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
    return hwnd;
}

HWND makeButton(HWND parent, UINT id, const wchar_t* text, int x, int y, int width, int height)
{
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    setChildFont(hwnd);
    SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
    return hwnd;
}

void drawPanelText(HDC dc, const std::wstring& text, int x, int y, int width, int height, COLORREF color, UINT format = DT_LEFT | DT_TOP | DT_SINGLELINE)
{
    RECT rect{x, y, x + width, y + height};
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
}

void syncControlsFromState();
void updateStatusControls();
void requestCanvasRender();
void readControlsToState();
void applyPngSpecsToState();
void computePalette();
bool handlePanelCommand(WPARAM wParam, LPARAM lParam);
void snapToolPanel(ToolPanel& panel);
void moveFloatingToolPanel(ToolPanel& panel, const RECT& rect);
LRESULT CALLBACK ToolPanelProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

CanvasLayout computeLayout(std::uint32_t width, std::uint32_t height)
{
    CanvasLayout layout;

    if (g_leftPanel.hwnd && IsWindowVisible(g_leftPanel.hwnd) && g_leftPanel.dock == ToolbarDock::Left)
        layout.leftInset = g_leftPanel.preferredSize.cx;
    if (g_rightPanel.hwnd && IsWindowVisible(g_rightPanel.hwnd) && g_rightPanel.dock == ToolbarDock::Right)
        layout.rightInset = g_rightPanel.preferredSize.cx;

    const int ruler = g_state.showRulers ? kRulerThickness : 0;
    layout.contentMinXIn = std::min(0.0, g_state.hasImage ? g_state.offsetXIn : 0.0);
    layout.contentMinYIn = std::min(0.0, g_state.hasImage ? g_state.offsetYIn : 0.0);
    layout.contentMaxXIn = std::max(g_state.canvasWidthIn, g_state.hasImage ? g_state.offsetXIn + g_state.imageWidthIn : g_state.canvasWidthIn);
    layout.contentMaxYIn = std::max(g_state.canvasHeightIn, g_state.hasImage ? g_state.offsetYIn + g_state.imageHeightIn : g_state.canvasHeightIn);

    const double contentWidthIn = std::max(0.01, layout.contentMaxXIn - layout.contentMinXIn);
    const double contentHeightIn = std::max(0.01, layout.contentMaxYIn - layout.contentMinYIn);
    const double availableWidth = std::max(80.0, static_cast<double>(width - std::min<std::uint32_t>(width, layout.leftInset + layout.rightInset + ruler)));
    const double availableHeight = std::max(80.0, static_cast<double>(height > static_cast<std::uint32_t>(ruler) ? height - ruler : height));

    layout.fitPxPerIn = std::min(availableWidth / contentWidthIn, availableHeight / contentHeightIn);
    layout.pxPerIn = std::max(1.0, layout.fitPxPerIn * g_state.zoomScale);
    layout.originX = static_cast<double>(layout.leftInset + ruler) - layout.contentMinXIn * layout.pxPerIn;
    layout.originY = static_cast<double>(ruler) - layout.contentMinYIn * layout.pxPerIn;
    return layout;
}

void addOverlayRect(CanvasRenderData& data, std::int32_t x, std::int32_t y, std::uint32_t width, std::uint32_t height, COLORREF color, std::uint32_t surfaceWidth, std::uint32_t surfaceHeight)
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
    rect.red = static_cast<float>(GetRValue(color)) / 255.0f;
    rect.green = static_cast<float>(GetGValue(color)) / 255.0f;
    rect.blue = static_cast<float>(GetBValue(color)) / 255.0f;
    rect.alpha = 1.0f;
    data.overlays.push_back(rect);
}

void addOverlayRectD(CanvasRenderData& data, double x, double y, double width, double height, COLORREF color, std::uint32_t surfaceWidth, std::uint32_t surfaceHeight)
{
    addOverlayRect(
        data,
        static_cast<std::int32_t>(std::round(x)),
        static_cast<std::int32_t>(std::round(y)),
        static_cast<std::uint32_t>(std::max(1.0, std::round(width))),
        static_cast<std::uint32_t>(std::max(1.0, std::round(height))),
        color,
        surfaceWidth,
        surfaceHeight);
}

void buildOverlayGeometry(CanvasRenderData& data, std::uint32_t width, std::uint32_t height, const CanvasLayout& layout)
{
    const COLORREF rulerBackground = RGB(184, 184, 184);
    const COLORREF gridRed = RGB(155, 0, 14);
    const COLORREF hotRed = RGB(194, 0, 23);

    const double pxPerIn = layout.pxPerIn;
    const double originX = layout.originX;
    const double originY = layout.originY;
    const double canvasWidth = g_state.canvasWidthIn * pxPerIn;
    const double canvasHeight = g_state.canvasHeightIn * pxPerIn;
    const double canvasRight = originX + canvasWidth;
    const double canvasBottom = originY + canvasHeight;

    const std::size_t gridColumns = static_cast<std::size_t>(std::floor(g_state.canvasWidthIn / std::max(0.001, g_state.gridSizeIn))) + 1;
    const std::size_t gridRows = static_cast<std::size_t>(std::floor(g_state.canvasHeightIn / std::max(0.001, g_state.gridSizeIn))) + 1;
    const std::size_t gridReserve = g_state.showGrid ? gridColumns + gridRows + 4 : 0;

    if (g_state.showRulers)
    {
        const double maxRulerXIn = std::max({g_state.canvasWidthIn, g_state.hasImage ? g_state.offsetXIn + g_state.imageWidthIn : 0.0, 0.0});
        const double maxRulerYIn = std::max({g_state.canvasHeightIn, g_state.hasImage ? g_state.offsetYIn + g_state.imageHeightIn : 0.0, 0.0});

        const std::size_t tickCountX = static_cast<std::size_t>(std::floor(maxRulerXIn / 0.25)) + 1;
        const std::size_t tickCountY = static_cast<std::size_t>(std::floor(maxRulerYIn / 0.25)) + 1;
        data.overlays.reserve(data.overlays.size() + tickCountX + tickCountY + 3 + gridReserve);

        addOverlayRectD(data, originX, originY - kRulerThickness, maxRulerXIn * pxPerIn, kRulerThickness, rulerBackground, width, height);
        addOverlayRectD(data, originX - kRulerThickness, originY, kRulerThickness, maxRulerYIn * pxPerIn, rulerBackground, width, height);

        constexpr double quarter = 0.25;
        for (int index = 0; index <= static_cast<int>(std::floor(maxRulerXIn / quarter)); ++index)
        {
            const double inch = static_cast<double>(index) * quarter;
            const bool whole = std::abs(inch - std::round(inch)) < 0.0001;
            const bool half = std::abs(inch * 2.0 - std::round(inch * 2.0)) < 0.0001;
            const double length = whole ? 14.0 : (half ? 9.0 : 5.0);
            const double tickWidth = whole ? 2.0 : 1.0;
            const double x = originX + inch * pxPerIn;
            addOverlayRectD(data, x, originY - length, tickWidth, length, gridRed, width, height);
        }

        for (int index = 0; index <= static_cast<int>(std::floor(maxRulerYIn / quarter)); ++index)
        {
            const double inch = static_cast<double>(index) * quarter;
            const bool whole = std::abs(inch - std::round(inch)) < 0.0001;
            const bool half = std::abs(inch * 2.0 - std::round(inch * 2.0)) < 0.0001;
            const double length = whole ? 14.0 : (half ? 9.0 : 5.0);
            const double tickWidth = whole ? 2.0 : 1.0;
            const double y = originY + inch * pxPerIn;
            addOverlayRectD(data, originX - length, y, length, tickWidth, gridRed, width, height);
        }

        addOverlayRectD(data, originX - 2.0, originY - 2.0, 4.0, 4.0, hotRed, width, height);
    }

    if (!g_state.showGrid)
        return;

    if (!g_state.showRulers)
        data.overlays.reserve(data.overlays.size() + gridReserve);

    const double gridStep = std::max(0.001, g_state.gridSizeIn) * pxPerIn;
    const int fullColumns = static_cast<int>(gridColumns) - 1;
    const int fullRows = static_cast<int>(gridRows) - 1;

    for (int column = 0; column <= fullColumns; ++column)
    {
        const bool major = g_state.majorEvery > 0 && column % g_state.majorEvery == 0;
        const double x = originX + static_cast<double>(column) * gridStep;
        addOverlayRectD(data, x, originY, major ? 2.0 : 1.0, canvasHeight, major ? hotRed : gridRed, width, height);
    }

    for (int row = 0; row <= fullRows; ++row)
    {
        const bool major = g_state.majorEvery > 0 && row % g_state.majorEvery == 0;
        const double y = originY + static_cast<double>(row) * gridStep;
        addOverlayRectD(data, originX, y, canvasWidth, major ? 2.0 : 1.0, major ? hotRed : gridRed, width, height);
    }

    addOverlayRectD(data, originX, originY, canvasWidth, 2.0, hotRed, width, height);
    addOverlayRectD(data, originX, canvasBottom - 2.0, canvasWidth, 2.0, hotRed, width, height);
    addOverlayRectD(data, originX, originY, 2.0, canvasHeight, hotRed, width, height);
    addOverlayRectD(data, canvasRight - 2.0, originY, 2.0, canvasHeight, hotRed, width, height);
}

bool pointInsideImage(int x, int y)
{
    if (!g_state.hasImage)
        return false;

    const double imageX = g_layout.originX + g_state.offsetXIn * g_layout.pxPerIn;
    const double imageY = g_layout.originY + g_state.offsetYIn * g_layout.pxPerIn;
    const double imageWidth = g_state.imageWidthIn * g_layout.pxPerIn;
    const double imageHeight = g_state.imageHeightIn * g_layout.pxPerIn;
    return x >= imageX && y >= imageY && x <= imageX + imageWidth && y <= imageY + imageHeight;
}

void updateProbe(int x, int y)
{
    g_state.probeValid = false;
    g_state.probeText = L"Move over image";

    if (!g_state.hasImage || g_state.image.width == 0 || g_state.image.height == 0 || g_layout.pxPerIn <= 0.0)
    {
        if (g_rightPanel.hwnd)
            InvalidateRect(g_rightPanel.hwnd, nullptr, FALSE);
        return;
    }

    const double absoluteXIn = (static_cast<double>(x) - g_layout.originX) / g_layout.pxPerIn;
    const double absoluteYIn = (static_cast<double>(y) - g_layout.originY) / g_layout.pxPerIn;
    const double imageLocalXIn = absoluteXIn - g_state.offsetXIn;
    const double imageLocalYIn = absoluteYIn - g_state.offsetYIn;

    if (imageLocalXIn < 0.0 || imageLocalYIn < 0.0 || imageLocalXIn >= g_state.imageWidthIn || imageLocalYIn >= g_state.imageHeightIn)
    {
        if (g_rightPanel.hwnd)
            InvalidateRect(g_rightPanel.hwnd, nullptr, FALSE);
        return;
    }

    const UINT px = static_cast<UINT>(std::clamp<double>(std::floor((imageLocalXIn / g_state.imageWidthIn) * g_state.image.width), 0.0, static_cast<double>(g_state.image.width - 1)));
    const UINT py = static_cast<UINT>(std::clamp<double>(std::floor((imageLocalYIn / g_state.imageHeightIn) * g_state.image.height), 0.0, static_cast<double>(g_state.image.height - 1)));
    const std::size_t index = (static_cast<std::size_t>(py) * g_state.image.width + px) * 4;
    if (index + 3 >= g_state.image.pixels.size())
        return;

    g_state.probeColor.blue = g_state.image.pixels[index + 0];
    g_state.probeColor.green = g_state.image.pixels[index + 1];
    g_state.probeColor.red = g_state.image.pixels[index + 2];
    g_state.probeColor.count = 1;
    g_state.probeValid = true;

    wchar_t text[160] = {};
    std::swprintf(
        text,
        std::size(text),
        L"%ls  rgb(%u, %u, %u)  px %u,%u",
        formatHex(g_state.probeColor).c_str(),
        g_state.probeColor.red,
        g_state.probeColor.green,
        g_state.probeColor.blue,
        px,
        py);
    g_state.probeText = text;

    if (g_rightPanel.hwnd)
        InvalidateRect(g_rightPanel.hwnd, nullptr, FALSE);
}

void updateCursorSummary(int x, int y)
{
    if (g_layout.pxPerIn <= 0.0)
        return;

    const double xIn = (static_cast<double>(x) - g_layout.originX) / g_layout.pxPerIn;
    const double yIn = (static_cast<double>(y) - g_layout.originY) / g_layout.pxPerIn;
    if (xIn < 0.0 || yIn < 0.0 || xIn > g_state.canvasWidthIn || yIn > g_state.canvasHeightIn)
    {
        g_state.cursorSummary = L"-";
        updateStatusControls();
        return;
    }

    const double gridSize = std::max(0.001, g_state.gridSizeIn);
    const int cellX = static_cast<int>(std::floor(xIn / gridSize));
    const int cellY = static_cast<int>(std::floor(yIn / gridSize));
    const int unitsPerCell = static_cast<int>(std::round(gridSize * 1000.0));
    const int localX = static_cast<int>(std::clamp<double>(std::round((xIn - cellX * gridSize) * 1000.0), 0.0, std::max(0, unitsPerCell - 1)));
    const int localY = static_cast<int>(std::clamp<double>(std::round((yIn - cellY * gridSize) * 1000.0), 0.0, std::max(0, unitsPerCell - 1)));

    wchar_t text[160] = {};
    std::swprintf(text, std::size(text), L"X:%d Y:%d  cell %d,%d  %.3f in, %.3f in", localX, localY, cellX, cellY, xIn, yIn);
    g_state.cursorSummary = text;
    updateStatusControls();
}

void computePalette()
{
    g_state.palette.clear();
    if (!g_state.hasImage || g_state.image.pixels.empty())
        return;

    struct Accumulator
    {
        std::uint64_t red = 0;
        std::uint64_t green = 0;
        std::uint64_t blue = 0;
        std::uint32_t count = 0;
    };

    std::unordered_map<std::uint16_t, Accumulator> buckets;
    const std::size_t pixelCount = static_cast<std::size_t>(g_state.image.width) * g_state.image.height;
    const std::size_t stride = std::max<std::size_t>(1, pixelCount / 240000);

    for (std::size_t pixel = 0; pixel < pixelCount; pixel += stride)
    {
        const std::size_t byteIndex = pixel * 4;
        if (byteIndex + 3 >= g_state.image.pixels.size())
            break;

        const std::uint8_t blue = g_state.image.pixels[byteIndex + 0];
        const std::uint8_t green = g_state.image.pixels[byteIndex + 1];
        const std::uint8_t red = g_state.image.pixels[byteIndex + 2];
        const std::uint8_t alpha = g_state.image.pixels[byteIndex + 3];
        if (alpha == 0)
            continue;

        const std::uint16_t key = static_cast<std::uint16_t>(((red >> 4) << 8) | ((green >> 4) << 4) | (blue >> 4));
        Accumulator& bucket = buckets[key];
        bucket.red += red;
        bucket.green += green;
        bucket.blue += blue;
        ++bucket.count;
    }

    std::vector<PaletteColor> colors;
    colors.reserve(buckets.size());
    for (const auto& entry : buckets)
    {
        const Accumulator& bucket = entry.second;
        if (bucket.count == 0)
            continue;

        PaletteColor color;
        color.red = static_cast<std::uint8_t>(bucket.red / bucket.count);
        color.green = static_cast<std::uint8_t>(bucket.green / bucket.count);
        color.blue = static_cast<std::uint8_t>(bucket.blue / bucket.count);
        color.count = bucket.count;
        colors.push_back(color);
    }

    std::sort(colors.begin(), colors.end(), [](const PaletteColor& left, const PaletteColor& right) {
        return left.count > right.count;
    });

    const std::size_t desired = static_cast<std::size_t>(std::clamp(g_state.paletteCount, 8, 64));
    for (const PaletteColor& color : colors)
    {
        bool tooClose = false;
        for (const PaletteColor& selected : g_state.palette)
        {
            const int dr = static_cast<int>(color.red) - static_cast<int>(selected.red);
            const int dg = static_cast<int>(color.green) - static_cast<int>(selected.green);
            const int db = static_cast<int>(color.blue) - static_cast<int>(selected.blue);
            if (dr * dr + dg * dg + db * db < 900)
            {
                tooClose = true;
                break;
            }
        }

        if (!tooClose)
            g_state.palette.push_back(color);
        if (g_state.palette.size() >= desired)
            break;
    }

    for (const PaletteColor& color : colors)
    {
        if (g_state.palette.size() >= desired)
            break;
        if (std::find_if(g_state.palette.begin(), g_state.palette.end(), [&](const PaletteColor& selected) {
                return selected.red == color.red && selected.green == color.green && selected.blue == color.blue;
            }) == g_state.palette.end())
        {
            g_state.palette.push_back(color);
        }
    }
}

void applyPngSpecsToState()
{
    if (!g_state.hasImage || g_state.detectedPpi <= 0.0)
        return;

    g_state.imagePpi = clampDouble(g_state.detectedPpi, 1.0, 5000.0);
    g_state.imageWidthIn = clampDouble(static_cast<double>(g_state.image.width) / g_state.imagePpi, 0.01, 10000.0);
    g_state.imageHeightIn = clampDouble(static_cast<double>(g_state.image.height) / g_state.imagePpi, 0.01, 10000.0);
}

void updateStatusControls()
{
    if (!g_controls.status)
        return;

    if (g_state.hasImage)
    {
        setControlText(g_controls.status, L"Loaded: " + g_state.imageName);

        wchar_t pixels[96] = {};
        std::swprintf(pixels, std::size(pixels), L"%u x %u px", g_state.image.width, g_state.image.height);
        setControlText(g_controls.pixels, pixels);

        wchar_t ppi[160] = {};
        std::swprintf(ppi, std::size(ppi), L"%.2f (%ls)", g_state.detectedPpi, g_state.ppiSource.c_str());
        setControlText(g_controls.ppi, ppi);

        wchar_t sourceSize[96] = {};
        std::swprintf(sourceSize, std::size(sourceSize), L"%.2f x %.2f in", static_cast<double>(g_state.image.width) / g_state.detectedPpi, static_cast<double>(g_state.image.height) / g_state.detectedPpi);
        setControlText(g_controls.sourceSize, sourceSize);
    }
    else
    {
        setControlText(g_controls.status, L"No image loaded");
        setControlText(g_controls.pixels, L"-");
        setControlText(g_controls.ppi, L"-");
        setControlText(g_controls.sourceSize, L"-");
    }

    wchar_t imageSummary[160] = {};
    std::swprintf(imageSummary, std::size(imageSummary), L"%.2f x %.2f in @ %.2f PPI", g_state.imageWidthIn, g_state.imageHeightIn, g_state.imagePpi);
    setControlText(g_controls.imageSummary, imageSummary);

    wchar_t canvasSummary[128] = {};
    std::swprintf(canvasSummary, std::size(canvasSummary), L"%.2f x %.2f in", g_state.canvasWidthIn, g_state.canvasHeightIn);
    setControlText(g_controls.canvasSummary, canvasSummary);

    const int cols = static_cast<int>(std::floor(g_state.canvasWidthIn / std::max(0.001, g_state.gridSizeIn)));
    const int rows = static_cast<int>(std::floor(g_state.canvasHeightIn / std::max(0.001, g_state.gridSizeIn)));
    wchar_t gridSummary[180] = {};
    std::swprintf(gridSummary, std::size(gridSummary), L"%.2f in cells, %d x %d full cells", g_state.gridSizeIn, cols, rows);
    setControlText(g_controls.gridSummary, gridSummary);

    setControlText(g_controls.cursorSummary, g_state.cursorSummary);

    wchar_t displaySummary[180] = {};
    std::swprintf(displaySummary, std::size(displaySummary), L"%.2f px/in, fit %.2f, zoom %.2fx", g_layout.pxPerIn, g_layout.fitPxPerIn, g_state.zoomScale);
    setControlText(g_controls.displaySummary, displaySummary);
}

void syncControlsFromState()
{
    g_syncingControls = true;
    setControlText(g_controls.imageWidth, formatDouble(g_state.imageWidthIn));
    setControlText(g_controls.imageHeight, formatDouble(g_state.imageHeightIn));
    setControlText(g_controls.imagePpi, formatDouble(g_state.imagePpi));
    setControlText(g_controls.canvasWidth, formatDouble(g_state.canvasWidthIn));
    setControlText(g_controls.canvasHeight, formatDouble(g_state.canvasHeightIn));
    setControlText(g_controls.gridSize, formatDouble(g_state.gridSizeIn));
    setControlText(g_controls.majorEvery, formatInt(g_state.majorEvery));
    setControlText(g_controls.offsetX, formatDouble(g_state.offsetXIn));
    setControlText(g_controls.offsetY, formatDouble(g_state.offsetYIn));
    setControlText(g_controls.zoom, formatDouble(g_state.zoomScale));
    setControlText(g_controls.paletteCount, formatInt(g_state.paletteCount));
    g_syncingControls = false;

    updateStatusControls();
    if (g_rightPanel.hwnd)
        InvalidateRect(g_rightPanel.hwnd, nullptr, FALSE);
}

void readControlsToState()
{
    if (g_syncingControls)
        return;

    g_state.imageWidthIn = readControlDouble(g_controls.imageWidth, g_state.imageWidthIn, 0.01, 10000.0);
    g_state.imageHeightIn = readControlDouble(g_controls.imageHeight, g_state.imageHeightIn, 0.01, 10000.0);
    g_state.imagePpi = readControlDouble(g_controls.imagePpi, g_state.imagePpi, 1.0, 5000.0);
    g_state.canvasWidthIn = readControlDouble(g_controls.canvasWidth, g_state.canvasWidthIn, 0.01, 10000.0);
    g_state.canvasHeightIn = readControlDouble(g_controls.canvasHeight, g_state.canvasHeightIn, 0.01, 10000.0);
    g_state.gridSizeIn = readControlDouble(g_controls.gridSize, g_state.gridSizeIn, 0.01, 10000.0);
    g_state.majorEvery = readControlInt(g_controls.majorEvery, g_state.majorEvery, 0, 10000);
    g_state.offsetXIn = snapQuarter(readControlDouble(g_controls.offsetX, g_state.offsetXIn, -10000.0, 10000.0));
    g_state.offsetYIn = snapQuarter(readControlDouble(g_controls.offsetY, g_state.offsetYIn, -10000.0, 10000.0));
    g_state.zoomScale = readControlDouble(g_controls.zoom, g_state.zoomScale, 1.0, kMaxZoom);
    const int previousPaletteCount = g_state.paletteCount;
    g_state.paletteCount = readControlInt(g_controls.paletteCount, g_state.paletteCount, 8, 64);
    if (previousPaletteCount != g_state.paletteCount)
        computePalette();
}

void requestCanvasRender()
{
    RECT client{};
    if (g_parent && GetClientRect(g_parent, &client))
        g_layout = computeLayout(static_cast<std::uint32_t>(client.right - client.left), static_cast<std::uint32_t>(client.bottom - client.top));

    updateStatusControls();
    saveCanvasCaliperConfig();
    requestRender();
}

void createLeftPanelControls(HWND parent)
{
    int y = kHeaderHeight + 8;
    makeStatic(parent, L"PNG Source", 10, y, 210, 14);
    y += 18;
    g_controls.status = makeStatic(parent, L"No image loaded", 12, y, 216, 16);
    y += 18;
    g_controls.pixels = makeStatic(parent, L"-", 12, y, 216, 16);
    y += 18;
    g_controls.ppi = makeStatic(parent, L"-", 12, y, 216, 16);
    y += 18;
    g_controls.sourceSize = makeStatic(parent, L"-", 12, y, 216, 16);
    y += 26;

    auto addEditRow = [&](const wchar_t* label, HWND& target, UINT id) {
        makeStatic(parent, label, 10, y + 4, 134, 18);
        target = makeEdit(parent, id, 150, y, 74, 23);
        y += 28;
    };

    makeStatic(parent, L"Reference Image Size", 10, y, 210, 14);
    y += 20;
    addEditRow(L"Image width (in)", g_controls.imageWidth, IDC_CC_IMAGE_WIDTH);
    addEditRow(L"Image height (in)", g_controls.imageHeight, IDC_CC_IMAGE_HEIGHT);
    addEditRow(L"Image PPI", g_controls.imagePpi, IDC_CC_IMAGE_PPI);

    y += 8;
    makeStatic(parent, L"Canvas / Grid Bounds", 10, y, 210, 14);
    y += 20;
    addEditRow(L"Canvas width (in)", g_controls.canvasWidth, IDC_CC_CANVAS_WIDTH);
    addEditRow(L"Canvas height (in)", g_controls.canvasHeight, IDC_CC_CANVAS_HEIGHT);
    addEditRow(L"Grid size (in)", g_controls.gridSize, IDC_CC_GRID_SIZE);
    addEditRow(L"Major every", g_controls.majorEvery, IDC_CC_MAJOR_EVERY);

    y += 8;
    makeStatic(parent, L"Image Offset / Zoom", 10, y, 210, 14);
    y += 20;
    addEditRow(L"Offset X (in)", g_controls.offsetX, IDC_CC_OFFSET_X);
    addEditRow(L"Offset Y (in)", g_controls.offsetY, IDC_CC_OFFSET_Y);
    addEditRow(L"Zoom scale", g_controls.zoom, IDC_CC_ZOOM);
    addEditRow(L"Swatches", g_controls.paletteCount, IDC_CC_PALETTE_COUNT);

    y += 8;
    makeButton(parent, IDC_CC_OPEN, L"Open PNG", 10, y, 104, 25);
    makeButton(parent, IDC_CC_USE_PNG, L"Use PNG", 122, y, 104, 25);
    y += 31;
    makeButton(parent, IDC_CC_RESET_OFFSET, L"Reset Offset", 10, y, 104, 25);
    makeButton(parent, IDC_CC_FIT_VIEW, L"Fit View", 122, y, 104, 25);
    y += 31;
    makeButton(parent, IDC_CC_ZOOM_OUT, L"Zoom -", 10, y, 104, 25);
    makeButton(parent, IDC_CC_ZOOM_IN, L"Zoom +", 122, y, 104, 25);
    y += 31;
    makeButton(parent, IDC_CC_CLEAR, L"Clear", 10, y, 104, 25);
    makeButton(parent, IDC_CC_SAVE, L"Save", 122, y, 104, 25);
    y += 35;

    makeStatic(parent, L"View", 10, y, 210, 14);
    y += 18;
    g_controls.imageSummary = makeStatic(parent, L"-", 12, y, 216, 16);
    y += 18;
    g_controls.canvasSummary = makeStatic(parent, L"-", 12, y, 216, 16);
    y += 18;
    g_controls.gridSummary = makeStatic(parent, L"-", 12, y, 216, 16);
    y += 18;
    g_controls.cursorSummary = makeStatic(parent, L"-", 12, y, 216, 16);
    y += 18;
    g_controls.displaySummary = makeStatic(parent, L"-", 12, y, 216, 16);
}

void paintRightPanel(HWND hwnd, HDC dc, const RECT& client)
{
    drawPanelText(dc, L"Palette", 10, kHeaderHeight + 8, client.right - 20, 20, RGB(242, 244, 247));

    wchar_t info[128] = {};
    if (g_state.palette.empty())
        wcscpy_s(info, std::size(info), L"Load a PNG to generate swatches.");
    else
        std::swprintf(info, std::size(info), L"%zu computed swatches", g_state.palette.size());
    drawPanelText(dc, info, 10, kHeaderHeight + 32, client.right - 20, 20, RGB(152, 162, 173));

    const int columns = 4;
    const int gap = 8;
    const int swatchWidth = (client.right - 20 - gap * (columns - 1)) / columns;
    const int swatchHeight = 72;
    int x = 10;
    int y = kHeaderHeight + 60;

    const std::uint32_t total = std::max<std::uint32_t>(1, [&]() {
        std::uint32_t value = 0;
        for (const PaletteColor& color : g_state.palette)
            value += color.count;
        return value;
    }());

    for (std::size_t index = 0; index < g_state.palette.size(); ++index)
    {
        const PaletteColor& color = g_state.palette[index];
        RECT colorRect{x, y, x + swatchWidth, y + 42};
        HBRUSH brush = CreateSolidBrush(RGB(color.red, color.green, color.blue));
        FillRect(dc, &colorRect, brush);
        DeleteObject(brush);

        RECT metaRect{x, y + 42, x + swatchWidth, y + swatchHeight};
        FillRect(dc, &metaRect, g_editBrush);
        drawPanelText(dc, formatHex(color), x + 5, y + 46, swatchWidth - 10, 13, RGB(242, 244, 247));

        wchar_t percent[32] = {};
        std::swprintf(percent, std::size(percent), L"%.1f%%", (static_cast<double>(color.count) / static_cast<double>(total)) * 100.0);
        drawPanelText(dc, percent, x + 5, y + 59, swatchWidth - 10, 12, RGB(180, 185, 192));

        x += swatchWidth + gap;
        if ((index + 1) % columns == 0)
        {
            x = 10;
            y += swatchHeight + gap;
        }
        if (y + swatchHeight > client.bottom - 122)
            break;
    }

    RECT probeRect{10, client.bottom - 108, client.right - 10, client.bottom - 10};
    FillRect(dc, &probeRect, g_editBrush);
    drawPanelText(dc, L"Pixel Color", probeRect.left + 82, probeRect.top + 14, probeRect.right - probeRect.left - 92, 18, RGB(242, 244, 247));
    drawPanelText(dc, g_state.probeText, probeRect.left + 82, probeRect.top + 36, probeRect.right - probeRect.left - 92, 42, RGB(152, 162, 173), DT_LEFT | DT_TOP | DT_WORDBREAK);

    RECT colorBox{probeRect.left + 12, probeRect.top + 12, probeRect.left + 68, probeRect.bottom - 12};
    const COLORREF probeColor = g_state.probeValid ? RGB(g_state.probeColor.red, g_state.probeColor.green, g_state.probeColor.blue) : RGB(0, 0, 0);
    HBRUSH probeBrush = CreateSolidBrush(probeColor);
    FillRect(dc, &colorBox, probeBrush);
    DeleteObject(probeBrush);
}

void paintPanel(HWND hwnd, ToolPanel& panel)
{
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    RECT client{};
    GetClientRect(hwnd, &client);

    FillRect(dc, &client, g_panelBrush);
    SetBkMode(dc, TRANSPARENT);
    if (g_uiFont)
        SelectObject(dc, g_uiFont);

    RECT header{0, 0, client.right, kHeaderHeight};
    HBRUSH headerBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &header, headerBrush);
    DeleteObject(headerBrush);

    const std::wstring title = panel.kind == ToolbarKind::Left ? L"CanvasCaliper" : L"Palette";
    drawPanelText(dc, title, 10, 8, client.right - 20, 18, RGB(242, 244, 247));
    drawPanelText(dc, L"::", client.right - 26, 8, 16, 18, RGB(152, 162, 173), DT_RIGHT | DT_TOP | DT_SINGLELINE);

    if (panel.kind == ToolbarKind::Right)
        paintRightPanel(hwnd, dc, client);

    EndPaint(hwnd, &paint);
}

void moveFloatingToolPanel(ToolPanel& panel, const RECT& rect)
{
    panel.dock = ToolbarDock::Floating;
    panel.floatingRect = rect;
    if (panel.hwnd)
    {
        MoveWindow(
            panel.hwnd,
            rect.left,
            rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top,
            TRUE);
    }
}

void snapToolPanel(ToolPanel& panel)
{
    if (!g_parent || !panel.hwnd)
        return;

    RECT client{};
    GetClientRect(g_parent, &client);
    RECT rect{};
    GetWindowRect(panel.hwnd, &rect);
    POINT topLeft{rect.left, rect.top};
    POINT bottomRight{rect.right, rect.bottom};
    ScreenToClient(g_parent, &topLeft);
    ScreenToClient(g_parent, &bottomRight);
    rect = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};

    if (rect.left <= kSnapDistance)
    {
        panel.dock = ToolbarDock::Left;
    }
    else if (client.right - rect.right <= kSnapDistance)
    {
        panel.dock = ToolbarDock::Right;
    }
    else
    {
        panel.dock = ToolbarDock::Floating;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        rect.left = std::clamp<LONG>(rect.left, 0, std::max<LONG>(0, client.right - width));
        rect.top = std::clamp<LONG>(rect.top, 0, std::max<LONG>(0, client.bottom - height));
        rect.right = rect.left + width;
        rect.bottom = rect.top + height;
        panel.floatingRect = rect;
    }

    layoutCanvasCaliperChrome();
    requestCanvasRender();
}

void beginPanelDrag(ToolPanel& panel)
{
    if (!panel.hwnd || !g_parent)
        return;

    panel.dragging = true;
    GetCursorPos(&panel.dragStartScreen);
    GetWindowRect(panel.hwnd, &panel.dragStartRect);
    SetCapture(panel.hwnd);

    if (panel.dock != ToolbarDock::Floating)
    {
        POINT topLeft{panel.dragStartRect.left, panel.dragStartRect.top};
        ScreenToClient(g_parent, &topLeft);
        RECT rect{topLeft.x, topLeft.y, topLeft.x + panel.preferredSize.cx, topLeft.y + panel.preferredSize.cy};
        moveFloatingToolPanel(panel, rect);
        GetWindowRect(panel.hwnd, &panel.dragStartRect);
    }
}

void updatePanelDrag(ToolPanel& panel)
{
    if (!panel.dragging || !panel.hwnd || !g_parent)
        return;

    POINT cursor{};
    GetCursorPos(&cursor);
    const int dx = cursor.x - panel.dragStartScreen.x;
    const int dy = cursor.y - panel.dragStartScreen.y;
    RECT moved = panel.dragStartRect;
    OffsetRect(&moved, dx, dy);

    POINT topLeft{moved.left, moved.top};
    POINT bottomRight{moved.right, moved.bottom};
    ScreenToClient(g_parent, &topLeft);
    ScreenToClient(g_parent, &bottomRight);
    RECT clientRect{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    moveFloatingToolPanel(panel, clientRect);
    requestCanvasRender();

    // Present immediately instead of waiting for the next message-loop tick: the
    // parent's Vulkan surface must redraw the region the panel just vacated before
    // DWM composites the next frame, or the drag leaves trailing ghost rectangles
    // behind the panel (an old-school "cascading window" artifact).
    renderFrame();
}

void finishPanelDrag(ToolPanel& panel)
{
    if (!panel.dragging)
        return;

    panel.dragging = false;
    ReleaseCapture();
    snapToolPanel(panel);
}

LRESULT CALLBACK ToolPanelProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    ToolPanel* panel = reinterpret_cast<ToolPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message)
    {
    case WM_NCCREATE:
    {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }

    case WM_LBUTTONDOWN:
        if (panel && GET_Y_LPARAM(lParam) < kHeaderHeight)
        {
            beginPanelDrag(*panel);
            return 0;
        }
        break;

    case WM_MOUSEMOVE:
        if (panel && panel->dragging)
        {
            updatePanelDrag(*panel);
            return 0;
        }
        break;

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (panel && panel->dragging)
        {
            finishPanelDrag(*panel);
            return 0;
        }
        break;

    case WM_COMMAND:
        if (handlePanelCommand(wParam, lParam))
            return 0;
        break;

    case WM_CTLCOLORSTATIC:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(210, 214, 220));
        SetBkColor(dc, RGB(8, 10, 14));
        return reinterpret_cast<LRESULT>(g_panelBrush);
    }

    case WM_CTLCOLOREDIT:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(242, 244, 247));
        SetBkColor(dc, RGB(24, 27, 33));
        return reinterpret_cast<LRESULT>(g_editBrush);
    }

    case WM_PAINT:
        if (panel)
        {
            paintPanel(hwnd, *panel);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool registerPanelClass()
{
    if (g_registeredPanelClass)
        return true;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = ToolPanelProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kPanelClassName;
    windowClass.hbrBackground = g_panelBrush;

    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    g_registeredPanelClass = true;
    return true;
}

void createToolPanel(ToolPanel& panel)
{
    RECT rect = panel.floatingRect;
    DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    panel.hwnd = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kPanelClassName,
        L"",
        style,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        g_parent,
        nullptr,
        GetModuleHandleW(nullptr),
        &panel);

    if (panel.kind == ToolbarKind::Left && panel.hwnd)
        createLeftPanelControls(panel.hwnd);
}

bool handlePanelCommand(WPARAM wParam, LPARAM lParam)
{
    const UINT id = LOWORD(wParam);
    const UINT notification = HIWORD(wParam);

    switch (id)
    {
    case IDC_CC_OPEN:
        if (notification == BN_CLICKED)
            openCanvasCaliperPngDialog(g_parent);
        return true;
    case IDC_CC_USE_PNG:
        if (notification == BN_CLICKED)
        {
            applyPngSpecsToState();
            syncControlsFromState();
            requestCanvasRender();
        }
        return true;
    case IDC_CC_RESET_OFFSET:
        if (notification == BN_CLICKED)
        {
            g_state.offsetXIn = 0.0;
            g_state.offsetYIn = 0.0;
            syncControlsFromState();
            requestCanvasRender();
        }
        return true;
    case IDC_CC_FIT_VIEW:
        if (notification == BN_CLICKED)
        {
            g_state.zoomScale = 1.0;
            syncControlsFromState();
            requestCanvasRender();
        }
        return true;
    case IDC_CC_ZOOM_OUT:
        if (notification == BN_CLICKED)
        {
            g_state.zoomScale = clampDouble(g_state.zoomScale / kZoomStep, 1.0, kMaxZoom);
            syncControlsFromState();
            requestCanvasRender();
        }
        return true;
    case IDC_CC_ZOOM_IN:
        if (notification == BN_CLICKED)
        {
            g_state.zoomScale = clampDouble(g_state.zoomScale * kZoomStep, 1.0, kMaxZoom);
            syncControlsFromState();
            requestCanvasRender();
        }
        return true;
    case IDC_CC_CLEAR:
        if (notification == BN_CLICKED)
            clearCanvasCaliperImage();
        return true;
    case IDC_CC_SAVE:
        if (notification == BN_CLICKED)
        {
            readControlsToState();
            syncControlsFromState();
            requestCanvasRender();
        }
        return true;
    case IDC_CC_IMAGE_WIDTH:
    case IDC_CC_IMAGE_HEIGHT:
    case IDC_CC_IMAGE_PPI:
    case IDC_CC_CANVAS_WIDTH:
    case IDC_CC_CANVAS_HEIGHT:
    case IDC_CC_GRID_SIZE:
    case IDC_CC_MAJOR_EVERY:
    case IDC_CC_OFFSET_X:
    case IDC_CC_OFFSET_Y:
    case IDC_CC_ZOOM:
    case IDC_CC_PALETTE_COUNT:
        if (notification == EN_CHANGE && lParam != 0 && !g_syncingControls)
        {
            readControlsToState();
            requestCanvasRender();
            if (g_rightPanel.hwnd)
                InvalidateRect(g_rightPanel.hwnd, nullptr, FALSE);
        }
        return true;
    default:
        break;
    }

    return false;
}

void loadToolConfig(const nlohmann::json& object, const char* key, ToolPanel& panel)
{
    if (!object.contains(key) || !object[key].is_object())
        return;

    const nlohmann::json& tool = object[key];
    if (tool.contains("dock") && tool["dock"].is_string())
        panel.dock = dockFromString(tool["dock"].get<std::string>(), panel.dock);

    const int x = jsonInt(tool, "x", panel.floatingRect.left);
    const int y = jsonInt(tool, "y", panel.floatingRect.top);
    panel.floatingRect = {x, y, x + panel.preferredSize.cx, y + panel.preferredSize.cy};
}

void saveToolConfig(nlohmann::json& object, const char* key, const ToolPanel& panel)
{
    object[key] = {
        {"dock", dockToString(panel.dock)},
        {"x", panel.floatingRect.left},
        {"y", panel.floatingRect.top}
    };
}
}

void loadCanvasCaliperConfig()
{
    const nlohmann::json empty = nlohmann::json::object();
    const nlohmann::json& cc = (config.contains("canvasCaliper") && config["canvasCaliper"].is_object()) ? config["canvasCaliper"] : empty;

    g_state.imageWidthIn = clampDouble(jsonDouble(cc, "imageWidthIn", g_state.imageWidthIn), 0.01, 10000.0);
    g_state.imageHeightIn = clampDouble(jsonDouble(cc, "imageHeightIn", g_state.imageHeightIn), 0.01, 10000.0);
    g_state.imagePpi = clampDouble(jsonDouble(cc, "imagePpi", g_state.imagePpi), 1.0, 5000.0);
    g_state.canvasWidthIn = clampDouble(jsonDouble(cc, "canvasWidthIn", g_state.canvasWidthIn), 0.01, 10000.0);
    g_state.canvasHeightIn = clampDouble(jsonDouble(cc, "canvasHeightIn", g_state.canvasHeightIn), 0.01, 10000.0);
    g_state.gridSizeIn = clampDouble(jsonDouble(cc, "gridSizeIn", g_state.gridSizeIn), 0.01, 10000.0);
    g_state.offsetXIn = snapQuarter(jsonDouble(cc, "offsetXIn", g_state.offsetXIn));
    g_state.offsetYIn = snapQuarter(jsonDouble(cc, "offsetYIn", g_state.offsetYIn));
    g_state.zoomScale = clampDouble(jsonDouble(cc, "zoomScale", g_state.zoomScale), 1.0, kMaxZoom);
    g_state.majorEvery = std::clamp(jsonInt(cc, "majorEvery", g_state.majorEvery), 0, 10000);
    g_state.paletteCount = std::clamp(jsonInt(cc, "paletteCount", g_state.paletteCount), 8, 64);
    g_state.showGrid = jsonBool(cc, "showGrid", g_state.showGrid);
    g_state.showRulers = jsonBool(cc, "showRulers", g_state.showRulers);

    loadToolConfig(cc, "leftToolbar", g_leftPanel);
    loadToolConfig(cc, "rightToolbar", g_rightPanel);
}

void saveCanvasCaliperConfig()
{
    nlohmann::json cc = {
        {"imageWidthIn", g_state.imageWidthIn},
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
        {"showRulers", g_state.showRulers}
    };
    saveToolConfig(cc, "leftToolbar", g_leftPanel);
    saveToolConfig(cc, "rightToolbar", g_rightPanel);
    config["canvasCaliper"] = cc;
}

void initializeCanvasCaliper(HWND hwnd)
{
    g_parent = hwnd;
    loadCanvasCaliperConfig();

    g_uiFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    g_panelBrush = CreateSolidBrush(RGB(8, 10, 14));
    g_editBrush = CreateSolidBrush(RGB(24, 27, 33));

    if (!registerPanelClass())
        return;

    createToolPanel(g_leftPanel);
    createToolPanel(g_rightPanel);
    DragAcceptFiles(hwnd, TRUE);
    layoutCanvasCaliperChrome();
    syncControlsFromState();
    requestCanvasRender();
}

void shutdownCanvasCaliper()
{
    DragAcceptFiles(g_parent, FALSE);
    clearVulkanCanvasImage();

    if (g_leftPanel.hwnd)
        DestroyWindow(g_leftPanel.hwnd);
    if (g_rightPanel.hwnd)
        DestroyWindow(g_rightPanel.hwnd);

    g_leftPanel.hwnd = nullptr;
    g_rightPanel.hwnd = nullptr;
    g_parent = nullptr;

    if (g_panelBrush)
    {
        DeleteObject(g_panelBrush);
        g_panelBrush = nullptr;
    }
    if (g_editBrush)
    {
        DeleteObject(g_editBrush);
        g_editBrush = nullptr;
    }
}

void layoutCanvasCaliperChrome()
{
    if (!g_parent)
        return;

    RECT client{};
    GetClientRect(g_parent, &client);
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;

    auto layoutPanel = [&](ToolPanel& panel) {
        if (!panel.hwnd)
            return;

        if (panel.dock == ToolbarDock::Left)
        {
            MoveWindow(panel.hwnd, 0, 0, panel.preferredSize.cx, clientHeight, TRUE);
        }
        else if (panel.dock == ToolbarDock::Right)
        {
            MoveWindow(panel.hwnd, clientWidth - panel.preferredSize.cx, 0, panel.preferredSize.cx, clientHeight, TRUE);
        }
        else
        {
            RECT rect = panel.floatingRect;
            const int width = rect.right - rect.left;
            const int height = rect.bottom - rect.top;
            rect.left = std::clamp<LONG>(rect.left, 0, std::max<LONG>(0, clientWidth - width));
            rect.top = std::clamp<LONG>(rect.top, 0, std::max<LONG>(0, clientHeight - height));
            rect.right = rect.left + width;
            rect.bottom = rect.top + height;
            panel.floatingRect = rect;
            MoveWindow(panel.hwnd, rect.left, rect.top, width, height, TRUE);
        }
    };

    layoutPanel(g_leftPanel);
    layoutPanel(g_rightPanel);
    g_layout = computeLayout(static_cast<std::uint32_t>(clientWidth), static_cast<std::uint32_t>(clientHeight));
}

void openCanvasCaliperPngDialog(HWND owner)
{
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW open{};
    open.lStructSize = sizeof(open);
    open.hwndOwner = owner;
    open.lpstrFilter = L"PNG Images (*.png)\0*.png\0All Files (*.*)\0*.*\0";
    open.lpstrFile = filename;
    open.nMaxFile = static_cast<DWORD>(std::size(filename));
    open.lpstrTitle = L"Open PNG";
    open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    open.lpstrDefExt = L"png";

    if (GetOpenFileNameW(&open))
        loadCanvasCaliperPngPath(owner, filename);
}

bool loadCanvasCaliperPngPath(HWND owner, const wchar_t* path)
{
    if (!path || !*path)
        return false;

    const std::filesystem::path pngPath(path);
    if (!isPngPath(pngPath))
    {
        MessageBoxW(owner, L"Please choose a PNG file.", L"SIMP", MB_OK | MB_ICONWARNING);
        return false;
    }

    DecodedImage decoded;
    if (!decodePngFileBGRA(pngPath, decoded))
    {
        MessageBoxW(owner, L"Could not decode that PNG.", L"SIMP", MB_OK | MB_ICONERROR);
        return false;
    }

    g_state.image = std::move(decoded);
    g_state.hasImage = true;
    g_state.imagePath = pngPath.wstring();
    g_state.imageName = filenameOnly(pngPath);

    const PngPhysicalInfo physical = parsePngPhysicalInfo(pngPath);
    if (physical.found)
    {
        g_state.detectedPpi = (physical.ppiX + physical.ppiY) * 0.5;
        wchar_t source[96] = {};
        std::swprintf(source, std::size(source), L"PNG pHYs %.2f x %.2f", physical.ppiX, physical.ppiY);
        g_state.ppiSource = source;
    }
    else
    {
        g_state.detectedPpi = g_state.imagePpi;
        g_state.ppiSource = L"manual";
    }

    applyPngSpecsToState();
    computePalette();
    setVulkanCanvasImage(g_state.image);
    syncControlsFromState();
    requestCanvasRender();
    return true;
}

void clearCanvasCaliperImage()
{
    g_state.hasImage = false;
    g_state.image = {};
    g_state.imagePath.clear();
    g_state.imageName.clear();
    g_state.detectedPpi = g_state.imagePpi;
    g_state.ppiSource = L"manual";
    g_state.palette.clear();
    g_state.probeValid = false;
    g_state.probeText = L"Move over image";
    clearVulkanCanvasImage();
    syncControlsFromState();
    requestCanvasRender();
}

bool handleCanvasCaliperCommand(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
    return handlePanelCommand(wParam, lParam);
}

bool handleCanvasCaliperWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    switch (message)
    {
    case WM_DROPFILES:
    {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path))))
            loadCanvasCaliperPngPath(hwnd, path);
        DragFinish(drop);
        result = 0;
        return true;
    }

    case WM_MOUSEMOVE:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        if (g_drag.panning)
        {
            const double dxIn = (static_cast<double>(x - g_drag.startPoint.x)) / std::max(1.0, g_layout.pxPerIn);
            const double dyIn = (static_cast<double>(y - g_drag.startPoint.y)) / std::max(1.0, g_layout.pxPerIn);
            g_state.offsetXIn = snapQuarter(g_drag.startOffsetXIn + dxIn);
            g_state.offsetYIn = snapQuarter(g_drag.startOffsetYIn + dyIn);
            syncControlsFromState();
            requestCanvasRender();
        }
        updateCursorSummary(x, y);
        updateProbe(x, y);
        break;
    }

    case WM_RBUTTONDOWN:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        if (pointInsideImage(x, y))
        {
            g_drag.panning = true;
            g_drag.startPoint = {x, y};
            g_drag.startOffsetXIn = g_state.offsetXIn;
            g_drag.startOffsetYIn = g_state.offsetYIn;
            SetCapture(hwnd);
            result = 0;
            return true;
        }
        break;
    }

    case WM_RBUTTONUP:
    case WM_CAPTURECHANGED:
        if (g_drag.panning)
        {
            g_drag.panning = false;
            if (message == WM_RBUTTONUP)
                ReleaseCapture();
            result = 0;
            return true;
        }
        break;

    case WM_MOUSEWHEEL:
        if ((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0)
        {
            const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            g_state.zoomScale = clampDouble(delta > 0 ? g_state.zoomScale * kZoomStep : g_state.zoomScale / kZoomStep, 1.0, kMaxZoom);
            syncControlsFromState();
            requestCanvasRender();
            result = 0;
            return true;
        }
        break;

    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        {
            if (wParam == '0')
            {
                g_state.zoomScale = 1.0;
                syncControlsFromState();
                requestCanvasRender();
                result = 0;
                return true;
            }
            if (wParam == VK_OEM_PLUS || wParam == VK_ADD)
            {
                g_state.zoomScale = clampDouble(g_state.zoomScale * kZoomStep, 1.0, kMaxZoom);
                syncControlsFromState();
                requestCanvasRender();
                result = 0;
                return true;
            }
            if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT)
            {
                g_state.zoomScale = clampDouble(g_state.zoomScale / kZoomStep, 1.0, kMaxZoom);
                syncControlsFromState();
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

    CheckMenuItem(menu, IDM_VIEW_GRID, MF_BYCOMMAND | (g_state.showGrid ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_VIEW_RULERS, MF_BYCOMMAND | (g_state.showRulers ? MF_CHECKED : MF_UNCHECKED));
}

CanvasRenderData buildCanvasRenderData(std::uint32_t width, std::uint32_t height)
{
    CanvasRenderData data;
    g_layout = computeLayout(width, height);

    if (g_state.hasImage)
    {
        data.hasImage = true;
        data.imageX = static_cast<float>(g_layout.originX + g_state.offsetXIn * g_layout.pxPerIn);
        data.imageY = static_cast<float>(g_layout.originY + g_state.offsetYIn * g_layout.pxPerIn);
        data.imageWidth = static_cast<float>(g_state.imageWidthIn * g_layout.pxPerIn);
        data.imageHeight = static_cast<float>(g_state.imageHeightIn * g_layout.pxPerIn);
    }

    buildOverlayGeometry(data, width, height, g_layout);
    return data;
}

bool canvasCaliperGridVisible()
{
    return g_state.showGrid;
}

bool canvasCaliperRulersVisible()
{
    return g_state.showRulers;
}

void setCanvasCaliperGridVisible(bool visible)
{
    g_state.showGrid = visible;
    updateCanvasCaliperMenu(GetMenu(g_parent));
    requestCanvasRender();
}

void setCanvasCaliperRulersVisible(bool visible)
{
    g_state.showRulers = visible;
    updateCanvasCaliperMenu(GetMenu(g_parent));
    requestCanvasRender();
}
