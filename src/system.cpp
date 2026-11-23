#include "system.h"

#include "config.h"
#include "cpu_pixels.h"
#include "cpu_work.h"
#include "editor_theme.h"
#include "splash_shadow.h"
#include "splash_video.h"

#include "../resource.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <process.h>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>

#include <dwmapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>

#include "stb/stb_image.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

namespace
{
constexpr UINT kSplashTimerId = 1;
constexpr UINT kSplashFrameIntervalMs = 1000 / 30;
constexpr UINT kSplashFrameReadyMessage = WM_APP + 18;
constexpr int kSplashHorizontalPadding = 72;
constexpr int kSplashBottomPadding = 64;
constexpr int kSplashTextBlockAnchorYPercent = 66;
const wchar_t* kSplashWindowClassName = L"SimpSplashWindowClass";
constexpr wchar_t kSplashAuthorUrl[] = L"https://www.lakeshoretechnical.com/";
constexpr wchar_t kSplashVulkanUrl[] = L"https://www.vulkan.org/";
constexpr std::wstring_view kSplashVersionText = L"v. " APP_VERSION_WSTRING;
constexpr std::array<std::wstring_view, 4> kSplashTitleWords = {
    L"Simple",
    L"Image",
    L"Manipulation",
    L"Program"
};

IWICImagingFactory* g_wicFactory = nullptr;
bool g_comInitialized = false;
ThemePalette g_themePalette;
HWND g_splashWindow = nullptr;

template <typename T>
void safeRelease(T*& value)
{
    if (value)
    {
        value->Release();
        value = nullptr;
    }
}

struct SplashFrame
{
    UINT width = 0;
    UINT height = 0;
    PixelBuffer pixels;
};

struct SplashBranding
{
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousObject = nullptr;
    RECT source{};
    RECT destination{};
    RECT hyperlink{};
    int labelX = 0;
    std::uint8_t* pixels = nullptr;
    UINT stride = 0;
};

struct SplashTitleLayout
{
    std::array<int, 4> capitalWidths{};
    std::array<int, 4> remainderWidths{};
    int naturalWidth = 0;
    int spaceWidth = 0;
    TEXTMETRICW boldMetrics{};
    TEXTMETRICW regularMetrics{};
    TEXTMETRICW subtitleMetrics{};
};

struct SplashState
{
    std::deque<SplashFrame> frames;
    std::condition_variable frameConsumed;
    std::mutex framesMutex;
    HANDLE loaderThread = nullptr;
    std::atomic<bool> stopLoader = false;
    std::atomic<HWND> hwnd = nullptr;
    HDC backBufferDC = nullptr;
    HBITMAP backBufferBitmap = nullptr;
    HGDIOBJ backBufferPreviousObject = nullptr;
    void* backBufferPixels = nullptr;
    UINT backBufferWidth = 0;
    UINT backBufferHeight = 0;
    SplashBranding vulkan, author, shadow;
    HFONT titleBoldFont = nullptr;
    HFONT titleRegularFont = nullptr;
    HFONT subtitleFont = nullptr;
    SplashTitleLayout titleLayout;
    int brandingLabelY = 0;
};

BITMAPINFO makeBitmapInfo(UINT width, UINT height);

ThemePalette queryThemePalette()
{
    const COLORREF windowColor = GetSysColor(COLOR_WINDOW);
    ThemePalette palette;
    palette.red = GetRValue(windowColor);
    palette.green = GetGValue(windowColor);
    palette.blue = GetBValue(windowColor);
    palette.alpha = 255;

    const int luminance = static_cast<int>(palette.red) * 2126 + static_cast<int>(palette.green) * 7152 + static_cast<int>(palette.blue) * 722;
    palette.darkMode = luminance < (128 * 10000);
    return palette;
}

void destroyFont(HFONT& font)
{
    if (font)
    {
        DeleteObject(font);
        font = nullptr;
    }
}

HFONT createArialFont(int pixelHeight, LONG weight, int widthPercent = 100)
{
    LOGFONTW font{};
    font.lfHeight = -pixelHeight;
    font.lfWeight = weight;
    font.lfCharSet = DEFAULT_CHARSET;
#ifdef CLEARTYPE_NATURAL_QUALITY
    font.lfQuality = CLEARTYPE_NATURAL_QUALITY;
#else
    font.lfQuality = CLEARTYPE_QUALITY;
#endif
    wcscpy_s(font.lfFaceName, LF_FACESIZE, L"Arial");
    HFONT result = CreateFontIndirectW(&font);
    if (result && widthPercent != 100)
    {
        HDC dc = GetDC(nullptr);
        if (dc)
        {
            const auto previous = SelectObject(dc, result);
            TEXTMETRICW metrics{};
            GetTextMetricsW(dc, &metrics);
            SelectObject(dc, previous);
            ReleaseDC(nullptr, dc);
            font.lfWidth = std::max(1, MulDiv(metrics.tmAveCharWidth, widthPercent, 100));
            DeleteObject(result);
            result = CreateFontIndirectW(&font);
        }
    }
    return result;
}

SIZE measureTextExtent(HDC deviceContext, HFONT font, std::wstring_view text)
{
    SIZE size{};
    if (!font || text.empty())
        return size;

    HGDIOBJ previousFont = SelectObject(deviceContext, font);
    GetTextExtentPoint32W(deviceContext, text.data(), static_cast<int>(text.size()), &size);
    SelectObject(deviceContext, previousFont);
    return size;
}

TEXTMETRICW getFontMetrics(HDC deviceContext, HFONT font)
{
    TEXTMETRICW metrics{};
    if (!font)
        return metrics;

    HGDIOBJ previousFont = SelectObject(deviceContext, font);
    GetTextMetricsW(deviceContext, &metrics);
    SelectObject(deviceContext, previousFont);
    return metrics;
}

int measureTitleNaturalWidth(HDC deviceContext, HFONT titleBoldFont, HFONT titleRegularFont)
{
    int totalWidth = measureTextExtent(deviceContext, titleRegularFont, L" ").cx * static_cast<int>(kSplashTitleWords.size() - 1);
    for (std::wstring_view word : kSplashTitleWords)
    {
        totalWidth += measureTextExtent(deviceContext, titleBoldFont, word.substr(0, 1)).cx;
        totalWidth += measureTextExtent(deviceContext, titleRegularFont, word.substr(1)).cx;
    }
    return totalWidth;
}

bool createSplashFonts(SplashState& state, int splashWidth)
{
    HDC deviceContext = GetDC(nullptr);
    if (!deviceContext)
        return false;

    const int availableWidth = std::max(256, splashWidth - (kSplashHorizontalPadding * 2));
    int titleBoldHeight = 132;
    int titleRegularHeight = 96;

    while (true)
    {
        HFONT candidateBold = createArialFont(titleBoldHeight, FW_BOLD, 78);
        HFONT candidateRegular = createArialFont(titleRegularHeight, FW_SEMIBOLD, 78);
        if (!candidateBold || !candidateRegular)
        {
            destroyFont(candidateBold);
            destroyFont(candidateRegular);
            ReleaseDC(nullptr, deviceContext);
            return false;
        }

        if (measureTitleNaturalWidth(deviceContext, candidateBold, candidateRegular) <= availableWidth || titleBoldHeight <= 64)
        {
            state.titleBoldFont = candidateBold;
            state.titleRegularFont = candidateRegular;
            break;
        }

        destroyFont(candidateBold);
        destroyFont(candidateRegular);
        --titleBoldHeight;
        --titleRegularHeight;
    }

    constexpr int subtitleHeight = 20;
    state.subtitleFont = createArialFont(subtitleHeight, FW_SEMIBOLD);

    const bool success = state.subtitleFont != nullptr;
    ReleaseDC(nullptr, deviceContext);
    if (!success)
    {
        destroyFont(state.titleBoldFont);
        destroyFont(state.titleRegularFont);
        destroyFont(state.subtitleFont);
    }

    return success;
}

SplashTitleLayout measureSplashTitleLayout(HDC deviceContext, const SplashState& state)
{
    SplashTitleLayout layout{};
    layout.boldMetrics = getFontMetrics(deviceContext, state.titleBoldFont);
    layout.regularMetrics = getFontMetrics(deviceContext, state.titleRegularFont);
    layout.subtitleMetrics = getFontMetrics(deviceContext, state.subtitleFont);
    layout.spaceWidth = measureTextExtent(deviceContext, state.titleRegularFont, L" ").cx;
    layout.naturalWidth = layout.spaceWidth * static_cast<int>(kSplashTitleWords.size() - 1);

    for (std::size_t index = 0; index < kSplashTitleWords.size(); ++index)
    {
        const std::wstring_view word = kSplashTitleWords[index];
        layout.capitalWidths[index] = measureTextExtent(deviceContext, state.titleBoldFont, word.substr(0, 1)).cx;
        layout.remainderWidths[index] = measureTextExtent(deviceContext, state.titleRegularFont, word.substr(1)).cx;
        layout.naturalWidth += layout.capitalWidths[index] + layout.remainderWidths[index];
    }

    return layout;
}

void drawTextRun(HDC deviceContext, HFONT font, int x, int y, std::wstring_view text, COLORREF color)
{
    if (!font || text.empty()) return;
    const auto previous = SelectObject(deviceContext, font);
    SetTextColor(deviceContext, color);
    TextOutW(deviceContext, x, y, text.data(), static_cast<int>(text.size()));
    SelectObject(deviceContext, previous);
}

bool createSplashBitmap(SplashBranding& branding, const DecodedImage& image)
{
    branding.dc = CreateCompatibleDC(nullptr);
    if (!branding.dc) return false;
    BITMAPINFO info = makeBitmapInfo(image.width, image.height);
    void* pixels = nullptr;
    branding.bitmap = CreateDIBSection(branding.dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!branding.bitmap || !pixels) return false;
    std::memcpy(pixels, image.pixels.data(), image.pixels.size());
    branding.previousObject = SelectObject(branding.dc, branding.bitmap);
    branding.pixels = static_cast<std::uint8_t*>(pixels);
    branding.stride = image.width * 4;
    branding.source = {0, 0, LONG(image.width), LONG(image.height)};
    return true;
}

bool createSplashBranding(SplashBranding& branding, int resourceId)
{
    DecodedImage logo;
    if (!decodePngResourceBGRA(resourceId, logo))
        return false;

    // Crop transparent padding for placement, without changing source pixels.
    RECT bounds{static_cast<LONG>(logo.width), static_cast<LONG>(logo.height), 0, 0};
    auto* data = logo.pixels.data();
    for (UINT y = 0; y < logo.height; ++y)
        for (UINT x = 0; x < logo.width; ++x)
        {
            auto* pixel = data + (static_cast<std::size_t>(y) * logo.width + x) * 4;
            const unsigned alpha = pixel[3];
            if (alpha)
            {
                bounds.left = std::min(bounds.left, static_cast<LONG>(x));
                bounds.top = std::min(bounds.top, static_cast<LONG>(y));
                bounds.right = std::max(bounds.right, static_cast<LONG>(x + 1));
                bounds.bottom = std::max(bounds.bottom, static_cast<LONG>(y + 1));
            }
            for (int channel = 0; channel < 3; ++channel)
                pixel[channel] = static_cast<std::uint8_t>((pixel[channel] * alpha + 127) / 255);
        }
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return false;

    if (!createSplashBitmap(branding, logo)) return false;
    branding.source = bounds;
    return true;
}

void destroySplashBranding(SplashBranding& branding)
{
    if (branding.dc && branding.previousObject)
        SelectObject(branding.dc, branding.previousObject);
    if (branding.bitmap)
        DeleteObject(branding.bitmap);
    if (branding.dc)
        DeleteDC(branding.dc);
    branding = {};
}

void layoutSplashOverlay(SplashState& state, int width, int height)
{
    state.titleLayout = measureSplashTitleLayout(state.backBufferDC, state);
    const int labelHeight = state.titleLayout.subtitleMetrics.tmHeight;
    const int vulkanWidth = MulDiv(state.vulkan.source.right - state.vulkan.source.left, 85, 100);
    const int logoHeight = MulDiv(state.vulkan.source.bottom - state.vulkan.source.top, 85, 100);
    constexpr int gap = 32, labelGap = 8, hitPadding = 6;
    const int authorLimit = width - 2 * kSplashHorizontalPadding - vulkanWidth - gap;
    const int authorSourceWidth = state.author.source.right - state.author.source.left;
    const int authorSourceHeight = state.author.source.bottom - state.author.source.top;
    const int authorWidth = std::max(1, std::min(authorLimit, MulDiv(logoHeight, authorSourceWidth, authorSourceHeight)));
    const int authorHeight = std::max(1, MulDiv(authorWidth, authorSourceHeight, authorSourceWidth));
    const int authorColumn = std::max<int>(authorWidth, measureTextExtent(state.backBufferDC, state.subtitleFont, L"Authored by").cx);
    const int bottom = height - kSplashBottomPadding;
    const int authorX = width - kSplashHorizontalPadding - authorColumn;
    const int vulkanX = kSplashHorizontalPadding;
    state.brandingLabelY = bottom - logoHeight - labelGap - labelHeight;
    // Both branding blocks are 15% smaller; original bitmap pixels stay untouched.
    state.vulkan.destination = {vulkanX, bottom - logoHeight, vulkanX + vulkanWidth, bottom};
    const int authorY = bottom - logoHeight + (logoHeight - authorHeight) / 2;
    state.author.destination = {authorX + (authorColumn - authorWidth) / 2, authorY,
                               authorX + (authorColumn + authorWidth) / 2, authorY + authorHeight};
    state.vulkan.hyperlink = {vulkanX, state.brandingLabelY, vulkanX + vulkanWidth, bottom};
    state.author.hyperlink = {authorX, state.brandingLabelY, authorX + authorColumn, bottom};
    const int poweredLabelWidth = measureTextExtent(state.backBufferDC, state.subtitleFont, L"Powered by").cx;
    const int authorLabelWidth = measureTextExtent(state.backBufferDC, state.subtitleFont, L"Authored by").cx;
    state.vulkan.labelX = vulkanX + (vulkanWidth - poweredLabelWidth) / 2;
    state.author.labelX = authorX + (authorColumn - authorLabelWidth) / 2;
    InflateRect(&state.vulkan.hyperlink, hitPadding, hitPadding);
    InflateRect(&state.author.hyperlink, hitPadding, hitPadding);
}

void drawSplashOverlay(HDC deviceContext, SplashState& state, UINT width, UINT height, bool mask = false)
{
    if (!state.titleBoldFont || !state.titleRegularFont || !state.subtitleFont)
        return;

    const SplashTitleLayout& layout = state.titleLayout;
    const int availableWidth = std::max(1, static_cast<int>(width) - (kSplashHorizontalPadding * 2));
    const int gapCount = static_cast<int>(kSplashTitleWords.size() - 1);
    const int freeWidth = std::max(0, availableWidth - layout.naturalWidth);
    const int gapBonus = gapCount > 0 ? freeWidth / gapCount : 0;
    const int gapRemainder = gapCount > 0 ? freeWidth % gapCount : 0;
    const int titleBaseline = (static_cast<int>(height) * kSplashTextBlockAnchorYPercent) / 100;
    const int capitalY = titleBaseline - layout.boldMetrics.tmAscent;
    const int remainderY = titleBaseline - layout.regularMetrics.tmAscent;
    const COLORREF titleColor = mask ? RGB(255, 255, 255) : RGB(248, 248, 248);
    const COLORREF subtitleColor = mask ? RGB(255, 255, 255) : RGB(236, 236, 236);
    if (!mask && state.shadow.dc)
    {
        const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        AlphaBlend(deviceContext, 0, 0, width, height, state.shadow.dc, 0, 0, width, height, blend);
    }

    const int previousBkMode = SetBkMode(deviceContext, TRANSPARENT);

    int x = kSplashHorizontalPadding;
    for (std::size_t index = 0; index < kSplashTitleWords.size(); ++index)
    {
        const std::wstring_view word = kSplashTitleWords[index];
        drawTextRun(deviceContext, state.titleBoldFont, x, capitalY, word.substr(0, 1), titleColor);
        x += layout.capitalWidths[index];
        drawTextRun(deviceContext, state.titleRegularFont, x, remainderY, word.substr(1), titleColor);
        x += layout.remainderWidths[index];

        if (index + 1 < kSplashTitleWords.size())
            x += layout.spaceWidth + gapBonus + (static_cast<int>(index) < gapRemainder ? 1 : 0);
    }

    drawTextRun(deviceContext, state.subtitleFont, 64, 56, kSplashVersionText, subtitleColor);

    const auto drawBranding = [&](const SplashBranding& branding, std::wstring_view label) {
        if (!branding.dc || !branding.bitmap)
            return;
        const RECT& target = branding.destination;
        const RECT& source = branding.source;
        drawTextRun(deviceContext, state.subtitleFont, branding.labelX,
                    state.brandingLabelY, label, subtitleColor);
        if (mask) return;
        const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        AlphaBlend(deviceContext, target.left, target.top, target.right - target.left, target.bottom - target.top,
                   branding.dc, source.left, source.top, source.right - source.left, source.bottom - source.top, blend);
    };
    drawBranding(state.vulkan, L"Powered by");
    drawBranding(state.author, L"Authored by");

    SetBkMode(deviceContext, previousBkMode);
}

bool createSplashShadow(SplashState& state)
{
    const int width = int(state.backBufferWidth), height = int(state.backBufferHeight);
    DecodedImage image{state.backBufferWidth, state.backBufferHeight, {}};
    image.pixels.resize(std::size_t(width) * height * 4);
    if (!createSplashBitmap(state.shadow, image)) return false;
    drawSplashOverlay(state.shadow.dc, state, width, height, true);
    GdiFlush(); // GDI text writes RGB coverage, not a usable DIB alpha channel.
    std::vector<std::uint8_t> coverage(std::size_t(width) * height);
    for (std::size_t i = 0; i < coverage.size(); ++i)
    {
        const auto* pixel = state.shadow.pixels + i * 4;
        coverage[i] = std::max({pixel[0], pixel[1], pixel[2]});
    }
    // Use original logo alpha, including opaque black artwork. Luminance would
    // lose the black portions of the Lakeshore logo and weaken the Vulkan red.
    const auto addLogo = [&](const SplashBranding& logo) {
        const auto& at = logo.destination;
        const auto& source = logo.source;
        for (int y = std::max(0L, at.top); y < std::min<LONG>(height, at.bottom); ++y)
            for (int x = std::max(0L, at.left); x < std::min<LONG>(width, at.right); ++x)
            {
                const auto sx = source.left + std::int64_t(x - at.left) * (source.right - source.left) / (at.right - at.left);
                const auto sy = source.top + std::int64_t(y - at.top) * (source.bottom - source.top) / (at.bottom - at.top);
                auto& alpha = coverage[std::size_t(y) * width + x];
                alpha = std::max(alpha, logo.pixels[std::size_t(sy) * logo.stride + sx * 4 + 3]);
            }
    };
    addLogo(state.vulkan);
    addLogo(state.author);
    const auto shadow = makeSplashShadow(coverage, width, height);
    std::memcpy(state.shadow.pixels, shadow.data(), shadow.size());
    return true;
}

bool createSplashBackBuffer(SplashState& state, UINT width, UINT height)
{
    BITMAPINFO bitmapInfo = makeBitmapInfo(width, height);

    HDC screenDc = GetDC(nullptr);
    if (!screenDc)
        return false;

    HDC backBufferDc = CreateCompatibleDC(screenDc);
    ReleaseDC(nullptr, screenDc);
    if (!backBufferDc)
        return false;

    void* backBufferPixels = nullptr;
    HBITMAP backBufferBitmap = CreateDIBSection(backBufferDc, &bitmapInfo, DIB_RGB_COLORS, &backBufferPixels, nullptr, 0);
    if (!backBufferBitmap || !backBufferPixels)
    {
        if (backBufferBitmap)
            DeleteObject(backBufferBitmap);
        DeleteDC(backBufferDc);
        return false;
    }

    state.backBufferPreviousObject = SelectObject(backBufferDc, backBufferBitmap);
    state.backBufferDC = backBufferDc;
    state.backBufferBitmap = backBufferBitmap;
    state.backBufferPixels = backBufferPixels;
    state.backBufferWidth = width;
    state.backBufferHeight = height;
    return true;
}

void destroySplashBackBuffer(SplashState& state)
{
    if (state.backBufferDC)
    {
        if (state.backBufferPreviousObject)
        {
            SelectObject(state.backBufferDC, state.backBufferPreviousObject);
            state.backBufferPreviousObject = nullptr;
        }

        if (state.backBufferBitmap)
        {
            DeleteObject(state.backBufferBitmap);
            state.backBufferBitmap = nullptr;
        }

        DeleteDC(state.backBufferDC);
        state.backBufferDC = nullptr;
    }

    state.backBufferPixels = nullptr;
    state.backBufferWidth = 0;
    state.backBufferHeight = 0;
}

BITMAPINFO makeBitmapInfo(UINT width, UINT height)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return info;
}

RECT centerRectOnBounds(const RECT& bounds, int width, int height)
{
    RECT rect{};
    rect.left = bounds.left + ((bounds.right - bounds.left) - width) / 2;
    rect.top = bounds.top + ((bounds.bottom - bounds.top) - height) / 2;
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    return rect;
}

SplashState* getSplashState(HWND hwnd)
{
    return reinterpret_cast<SplashState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void destroySplashState(SplashState* state)
{
    if (!state)
        return;

    {
        std::scoped_lock lock(state->framesMutex);
        state->stopLoader.store(true);
    }
    state->frameConsumed.notify_all();

    if (state->loaderThread)
    {
        WaitForSingleObject(state->loaderThread, INFINITE);
        CloseHandle(state->loaderThread);
        state->loaderThread = nullptr;
    }

    destroySplashBackBuffer(*state);
    destroySplashBranding(state->vulkan);
    destroySplashBranding(state->author);
    destroySplashBranding(state->shadow);
    destroyFont(state->titleBoldFont);
    destroyFont(state->titleRegularFont);
    destroyFont(state->subtitleFont);

    delete state;
}

bool decodePngResourceBGRAWithFactory(IWICImagingFactory* factory, int resourceID, DecodedImage& image)
{
    image = {};

    if (!factory)
        return false;

    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceID), RT_RCDATA);
    if (!resource)
        return false;

    HGLOBAL resourceHandle = LoadResource(module, resource);
    if (!resourceHandle)
        return false;

    void* memory = LockResource(resourceHandle);
    const DWORD resourceSize = SizeofResource(module, resource);
    if (!memory || resourceSize == 0)
        return false;

    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    HRESULT result = factory->CreateStream(&stream);
    if (SUCCEEDED(result))
        result = stream->InitializeFromMemory(static_cast<BYTE*>(memory), resourceSize);
    if (SUCCEEDED(result))
        result = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(result))
        result = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(result))
        result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result))
    {
        result = converter->Initialize(
            frame,
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
    }

    if (FAILED(result))
    {
        safeRelease(converter);
        safeRelease(frame);
        safeRelease(decoder);
        safeRelease(stream);
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    converter->GetSize(&width, &height);
    if (width == 0 || height == 0)
    {
        safeRelease(converter);
        safeRelease(frame);
        safeRelease(decoder);
        safeRelease(stream);
        return false;
    }

    const UINT stride = width * 4;
    const UINT bufferSize = stride * height;
    image.width = width;
    image.height = height;
    image.pixels.resize(bufferSize);

    result = converter->CopyPixels(nullptr, stride, bufferSize, image.pixels.data());

    safeRelease(converter);
    safeRelease(frame);
    safeRelease(decoder);
    safeRelease(stream);

    return SUCCEEDED(result);
}

// User-opened image files go through stb_image rather than WIC: it's a single
// lightweight header with no COM lifetime to manage, and it covers PNG, JPEG,
// BMP, TGA, GIF, PSD (flattened), HDR, PIC, and PNM in one path. Splash-screen
// branding and the static fallback poster use the WIC resource decoder above.
bool decodeImageFileBGRAImpl(const std::filesystem::path& filename, DecodedImage& image)
{
    image = {};

    if (filename.empty())
        return false;

    std::ifstream input(filename,std::ios::binary|std::ios::ate);
    if(!input) return false;
    const auto fileSize=input.tellg();
    if(fileSize<=0 || fileSize>512*1024*1024) return false;
    input.seekg(0);
    std::vector<std::uint8_t> fileBytes(static_cast<std::size_t>(fileSize));
    input.read(reinterpret_cast<char*>(fileBytes.data()),fileSize);
    if(!input) return false;
    int width=0,height=0,channelsInFile=0;
    if(!stbi_info_from_memory(fileBytes.data(),static_cast<int>(fileBytes.size()),&width,&height,&channelsInFile) ||
        width<=0 || height<=0 || width>16384 || height>16384 || std::uint64_t(width)*height>64000000) return false;
    stbi_uc* decoded=stbi_load_from_memory(fileBytes.data(),static_cast<int>(fileBytes.size()),&width,&height,&channelsInFile,4);
    if(!decoded) return false;
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decodedOwner(decoded, stbi_image_free);
    if(width<=0 || height<=0 || width>16384 || height>16384 || std::uint64_t(width)*height>64000000)
    {return false;}
    image.width=static_cast<UINT>(width);image.height=static_cast<UINT>(height);
    image.pixels.resize(static_cast<std::size_t>(width)*height*4);

    auto* destination=image.pixels.data();
    const auto count=std::size_t(width)*height;
    parallelCpuRange(count,262144,[&](std::size_t begin,std::size_t end)
    {convertRgbaToBgra(destination+begin*4,decoded+begin*4,end-begin);});

    return true;
}

unsigned __stdcall SplashLoaderThreadProc(void* parameter)
{
    auto* state = static_cast<SplashState*>(parameter);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult)) return 0;
    SetThreadDescription(GetCurrentThread(), L"SIMP splash video decode");
    try
    {
        SplashVideo video(GetModuleHandleW(nullptr), IDR_SPLASH_VIDEO,
                          state->backBufferWidth, state->backBufferHeight);
        DecodedImage first;
        if (!video.read(first)) throw std::runtime_error("Splash video has no frames.");
        first = {}; // The original PNG poster already supplies frame zero.
        while (!state->stopLoader.load())
        {
            {
                std::unique_lock lock(state->framesMutex);
                state->frameConsumed.wait(lock, [state] {
                    return state->stopLoader.load() || state->frames.size() < 3;
                });
                if (state->stopLoader.load()) break;
            }
            DecodedImage image;
            if (!video.read(image) || state->stopLoader.load()) break;
            SplashFrame frame;
            frame.width = image.width;
            frame.height = image.height;
            frame.pixels = std::move(image.pixels);
            {
                std::scoped_lock lock(state->framesMutex);
                state->frames.push_back(std::move(frame));
            }
            if (const auto hwnd = state->hwnd.load())
                PostMessageW(hwnd, kSplashFrameReadyMessage, 0, 0);
        }
    }
    catch (...)
    {
        // Keep the poster/last decoded frame, hyperlinks and click dismissal if
        // optional Windows codecs are missing or playback fails.
    }
    CoUninitialize();
    return 0;
}

LRESULT CALLBACK SplashWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_NCCREATE:
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
    {
        SplashState* state = getSplashState(hwnd);
        if (!state || wParam != kSplashTimerId)
            return 0;

        std::size_t frameCount = 0;
        {
            std::scoped_lock lock(state->framesMutex);
            frameCount = state->frames.size();
            if (frameCount > 1)
                state->frames.pop_front();
        }

        if (frameCount > 1)
        {
            state->frameConsumed.notify_one();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case kSplashFrameReadyMessage:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
    {
        SplashState* state = getSplashState(hwnd);
        POINT clickPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const wchar_t* url = nullptr;
        if (state && PtInRect(&state->vulkan.hyperlink, clickPoint))
            url = kSplashVulkanUrl;
        else if (state && PtInRect(&state->author.hyperlink, clickPoint))
            url = kSplashAuthorUrl;
        if (url)
            ShellExecuteW(hwnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);

        DestroyWindow(hwnd);
        return 0;
    }

    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        DestroyWindow(hwnd);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            SplashState* state = getSplashState(hwnd);
            POINT cursorPoint{};
            GetCursorPos(&cursorPoint);
            ScreenToClient(hwnd, &cursorPoint);
            if (state && (PtInRect(&state->vulkan.hyperlink, cursorPoint) ||
                          PtInRect(&state->author.hyperlink, cursorPoint)))
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }

            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC deviceContext = BeginPaint(hwnd, &paint);
        SplashState* state = getSplashState(hwnd);
        if (state && state->backBufferDC && state->backBufferPixels)
        {
            UINT frameWidth = 0;
            UINT frameHeight = 0;

            {
                std::scoped_lock lock(state->framesMutex);
                if (!state->frames.empty())
                {
                    const SplashFrame& frame = state->frames.front();
                    std::memcpy(state->backBufferPixels, frame.pixels.data(), frame.pixels.size());
                    frameWidth = frame.width;
                    frameHeight = frame.height;
                }
            }

            if (frameWidth != 0 && frameHeight != 0)
            {
                drawSplashOverlay(state->backBufferDC, *state, frameWidth, frameHeight);
                BitBlt(deviceContext, 0, 0, static_cast<int>(frameWidth), static_cast<int>(frameHeight), state->backBufferDC, 0, 0, SRCCOPY);
            }
        }
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
    {
        SplashState* state = getSplashState(hwnd);
        if (state)
            state->hwnd.store(nullptr);
        KillTimer(hwnd, kSplashTimerId);
        g_splashWindow = nullptr;
        return 0;
    }

    case WM_NCDESTROY:
    {
        SplashState* state = getSplashState(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        destroySplashState(state);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}

bool initializeSystemServices()
{
    if (g_wicFactory)
        return true;

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(comResult))
        g_comInitialized = true;
    else if (comResult != RPC_E_CHANGED_MODE)
        return false;

    const HRESULT factoryResult = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&g_wicFactory));

    if (FAILED(factoryResult))
        return false;

    refreshThemePalette();
    return true;
}

void shutdownSystemServices()
{
    closeSplashScreen();
    safeRelease(g_wicFactory);

    if (g_comInitialized)
    {
        CoUninitialize();
        g_comInitialized = false;
    }
}

void refreshThemePalette()
{
    g_themePalette = queryThemePalette();
}

const ThemePalette& getThemePalette()
{
    return g_themePalette;
}

void applyWindowTheme(HWND hwnd)
{
    if (!hwnd)
        return;

    // SIMP's chrome is always dark (Photoshop/DeluxePaint-style), independent of the
    // OS theme, so every top-level window and dialog gets the immersive dark frame.
    const BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
    const COLORREF captionColor = EditorTheme::kCaption;
    const COLORREF textColor = EditorTheme::kText;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));
    RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
}

bool decodePngResourceBGRA(int resourceID, DecodedImage& image)
{
    return decodePngResourceBGRAWithFactory(g_wicFactory, resourceID, image);
}

bool decodeImageFileBGRA(const std::filesystem::path& filename, DecodedImage& image)
{
    return decodeImageFileBGRAImpl(filename, image);
}

bool showSplashScreen(HINSTANCE instance, HWND owner, const RECT& monitorBounds)
{
    static bool classRegistered = false;
    if (!classRegistered)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = SplashWindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kSplashWindowClassName;

        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;

        classRegistered = true;
    }

    DecodedImage image;
    if (!decodePngResourceBGRA(IDR_SPLASH_POSTER, image))
        return false;

    auto* state = new SplashState();

    SplashFrame firstFrame;
    firstFrame.width = image.width;
    firstFrame.height = image.height;
    firstFrame.pixels = std::move(image.pixels);
    state->frames.push_back(std::move(firstFrame));

    const int splashWidth = static_cast<int>(state->frames.front().width);
    const int splashHeight = static_cast<int>(state->frames.front().height);
    if (!createSplashFonts(*state, splashWidth))
    {
        destroySplashState(state);
        return false;
    }

    if (!createSplashBackBuffer(*state, state->frames.front().width, state->frames.front().height))
    {
        destroySplashState(state);
        return false;
    }

    if (!createSplashBranding(state->vulkan, IDR_BRANDING_VULKAN) ||
        !createSplashBranding(state->author, IDR_BRANDING_LAKESHORE))
    {
        destroySplashState(state);
        return false;
    }

    layoutSplashOverlay(*state, splashWidth, splashHeight);
    try
    {
        if (!createSplashShadow(*state))
        {
            destroySplashState(state);
            return false;
        }
    }
    catch (...)
    {
        destroySplashState(state);
        throw;
    }
    const RECT splashRect = centerRectOnBounds(monitorBounds, splashWidth, splashHeight);

    HWND splashWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kSplashWindowClassName,
        L"SIMP Splash",
        WS_POPUP,
        splashRect.left,
        splashRect.top,
        splashWidth,
        splashHeight,
        owner,
        nullptr,
        instance,
        state);

    if (!splashWindow)
    {
        destroySplashState(state);
        return false;
    }

    g_splashWindow = splashWindow;
    state->hwnd.store(splashWindow);
    state->loaderThread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, SplashLoaderThreadProc, state, 0, nullptr));

    ShowWindow(splashWindow, SW_SHOWNOACTIVATE);
    UpdateWindow(splashWindow);
    SetTimer(splashWindow, kSplashTimerId, kSplashFrameIntervalMs, nullptr);

    return true;
}

void closeSplashScreen()
{
    if (g_splashWindow) DestroyWindow(g_splashWindow);
}

bool encodePngFileBGRA(const std::filesystem::path& filename,const DecodedImage& image,double ppi,bool interlace,bool adaptiveFilter)
{
    if(!image.width || !image.height || image.pixels.size()!=std::size_t(image.width)*image.height*4) return false;
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(initialized) && initialized!=RPC_E_CHANGED_MODE) return false;
    IWICImagingFactory* factory=nullptr;
    static std::atomic<std::uint64_t> exportId{0};
    auto temporary = filename;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                 std::to_wstring(exportId.fetch_add(1)) + L".tmp";
    IWICStream* stream=nullptr;IWICBitmapEncoder* encoder=nullptr;IWICBitmapFrameEncode* frame=nullptr;
    IPropertyBag2* options=nullptr;
    HRESULT result=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    if(SUCCEEDED(result)) result=factory->CreateStream(&stream);
    if(SUCCEEDED(result)) result=stream->InitializeFromFilename(temporary.c_str(),GENERIC_WRITE);
    if(SUCCEEDED(result)) result=factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder);
    if(SUCCEEDED(result)) result=encoder->Initialize(stream,WICBitmapEncoderNoCache);
    if(SUCCEEDED(result)) result=encoder->CreateNewFrame(&frame,&options);
    if(SUCCEEDED(result))
    {
        PROPBAG2 properties[2]{};properties[0].pstrName=const_cast<wchar_t*>(L"InterlaceOption");properties[1].pstrName=const_cast<wchar_t*>(L"FilterOption");
        VARIANT values[2]{};values[0].vt=VT_BOOL;values[0].boolVal=interlace?VARIANT_TRUE:VARIANT_FALSE;
        values[1].vt=VT_UI1;values[1].bVal=BYTE(adaptiveFilter?WICPngFilterAdaptive:WICPngFilterNone);
        result=options->Write(2,properties,values);
    }
    if(SUCCEEDED(result)) result=frame->Initialize(options);
    if(SUCCEEDED(result)) result=frame->SetSize(image.width,image.height);
    if(SUCCEEDED(result)) result=frame->SetResolution(ppi,ppi);
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    if(SUCCEEDED(result)) result=frame->SetPixelFormat(&format);
    if(SUCCEEDED(result) && !IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA)) result=E_FAIL;
    if(SUCCEEDED(result)) result=frame->WritePixels(image.height,image.width*4,UINT(image.pixels.size()),const_cast<BYTE*>(image.pixels.data()));
    if(SUCCEEDED(result)) result=frame->Commit();
    if(SUCCEEDED(result)) result=encoder->Commit();
    safeRelease(options);safeRelease(frame);safeRelease(encoder);safeRelease(stream);safeRelease(factory);if(SUCCEEDED(initialized)) CoUninitialize();
    if(SUCCEEDED(result) && MoveFileExW(temporary.c_str(),filename.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return true;
    std::error_code ignored;std::filesystem::remove(temporary,ignored);return false;
}
