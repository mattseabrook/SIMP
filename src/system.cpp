#include "system.h"

#include "config.h"

#include "../resource.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <process.h>
#include <string>
#include <string_view>
#include <utility>

#include <dwmapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>

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
constexpr int kSplashTextBlockAnchorYPercent = 80;
constexpr DWORD kDwmColorDefault = 0xFFFFFFFFu;
const wchar_t* kSplashWindowClassName = L"SimpSplashWindowClass";
constexpr wchar_t kSplashAuthorUrl[] = L"https://www.mattseabrook.net";
constexpr std::array<std::wstring_view, 4> kSplashTitleWords = {
    L"Simple",
    L"Image",
    L"Manipulation",
    L"Program"
};
constexpr std::wstring_view kSplashAuthorName = L"Matt Seabrook";

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
    std::vector<std::uint8_t> pixels;
    BITMAPINFO bitmapInfo{};
};

struct SplashState
{
    std::vector<SplashFrame> frames;
    std::size_t currentFrame = 0;
    std::mutex framesMutex;
    HANDLE loaderThread = nullptr;
    std::atomic<bool> stopLoader = false;
    std::atomic<HWND> hwnd = nullptr;
    HHOOK mouseHook = nullptr;
    HDC backBufferDC = nullptr;
    HBITMAP backBufferBitmap = nullptr;
    HGDIOBJ backBufferPreviousObject = nullptr;
    void* backBufferPixels = nullptr;
    UINT backBufferWidth = 0;
    UINT backBufferHeight = 0;
    HFONT titleBoldFont = nullptr;
    HFONT titleRegularFont = nullptr;
    HFONT subtitleFont = nullptr;
    HFONT linkFont = nullptr;
    RECT hyperlinkRect{};
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

HFONT createArialFont(int pixelHeight, LONG weight, bool underline = false)
{
    LOGFONTW font{};
    font.lfHeight = -pixelHeight;
    font.lfWeight = weight;
    font.lfUnderline = underline ? TRUE : FALSE;
    font.lfCharSet = DEFAULT_CHARSET;
#ifdef CLEARTYPE_NATURAL_QUALITY
    font.lfQuality = CLEARTYPE_NATURAL_QUALITY;
#else
    font.lfQuality = CLEARTYPE_QUALITY;
#endif
    wcscpy_s(font.lfFaceName, LF_FACESIZE, L"Arial");
    return CreateFontIndirectW(&font);
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
    int titleBoldHeight = 112;
    int titleRegularHeight = 80;

    while (true)
    {
        HFONT candidateBold = createArialFont(titleBoldHeight, FW_BOLD);
        HFONT candidateRegular = createArialFont(titleRegularHeight, FW_NORMAL);
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

    const int subtitleHeight = std::max(18, titleRegularHeight / 3);
    state.subtitleFont = createArialFont(subtitleHeight, FW_NORMAL);
    state.linkFont = createArialFont(subtitleHeight, FW_NORMAL, true);

    const bool success = state.subtitleFont != nullptr && state.linkFont != nullptr;
    ReleaseDC(nullptr, deviceContext);
    if (!success)
    {
        destroyFont(state.titleBoldFont);
        destroyFont(state.titleRegularFont);
        destroyFont(state.subtitleFont);
        destroyFont(state.linkFont);
    }

    return success;
}

struct SplashTitleLayout
{
    std::array<int, 4> capitalWidths{};
    std::array<int, 4> remainderWidths{};
    int naturalWidth = 0;
    int spaceWidth = 0;
    TEXTMETRICW boldMetrics{};
    TEXTMETRICW regularMetrics{};
    TEXTMETRICW subtitleMetrics{};
    TEXTMETRICW linkMetrics{};
};

SplashTitleLayout measureSplashTitleLayout(HDC deviceContext, const SplashState& state)
{
    SplashTitleLayout layout{};
    layout.boldMetrics = getFontMetrics(deviceContext, state.titleBoldFont);
    layout.regularMetrics = getFontMetrics(deviceContext, state.titleRegularFont);
    layout.subtitleMetrics = getFontMetrics(deviceContext, state.subtitleFont);
    layout.linkMetrics = getFontMetrics(deviceContext, state.linkFont);
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

void drawTextRun(HDC deviceContext, HFONT font, int x, int y, std::wstring_view text, COLORREF textColor, COLORREF shadowColor, int shadowOffset)
{
    if (!font || text.empty())
        return;

    HGDIOBJ previousFont = SelectObject(deviceContext, font);
    SetTextColor(deviceContext, shadowColor);
    TextOutW(deviceContext, x + shadowOffset, y + shadowOffset, text.data(), static_cast<int>(text.size()));
    SetTextColor(deviceContext, textColor);
    TextOutW(deviceContext, x, y, text.data(), static_cast<int>(text.size()));
    SelectObject(deviceContext, previousFont);
}

void drawSplashOverlay(HDC deviceContext, SplashState& state, UINT width, UINT height)
{
    if (!state.titleBoldFont || !state.titleRegularFont || !state.subtitleFont || !state.linkFont)
        return;

    const SplashTitleLayout layout = measureSplashTitleLayout(deviceContext, state);
    const int availableWidth = std::max(1, static_cast<int>(width) - (kSplashHorizontalPadding * 2));
    const int gapCount = static_cast<int>(kSplashTitleWords.size() - 1);
    const int freeWidth = std::max(0, availableWidth - layout.naturalWidth);
    const int gapBonus = gapCount > 0 ? freeWidth / gapCount : 0;
    const int gapRemainder = gapCount > 0 ? freeWidth % gapCount : 0;
    const int titleBaseline = (static_cast<int>(height) * kSplashTextBlockAnchorYPercent) / 100;
    const int capitalY = titleBaseline - layout.boldMetrics.tmAscent;
    const int remainderY = titleBaseline - layout.regularMetrics.tmAscent;
    const int titleBottom = std::max(capitalY + layout.boldMetrics.tmHeight, remainderY + layout.regularMetrics.tmHeight);
    const int subtitleY = titleBottom + 12;
    const COLORREF titleColor = RGB(248, 248, 248);
    const COLORREF subtitleColor = RGB(236, 236, 236);
    COLORREF linkColor = GetSysColor(COLOR_HOTLIGHT);
    if (linkColor == 0)
        linkColor = RGB(86, 156, 214);
    const COLORREF shadowColor = RGB(12, 12, 12);

    const int previousBkMode = SetBkMode(deviceContext, TRANSPARENT);

    int x = kSplashHorizontalPadding;
    for (std::size_t index = 0; index < kSplashTitleWords.size(); ++index)
    {
        const std::wstring_view word = kSplashTitleWords[index];
        drawTextRun(deviceContext, state.titleBoldFont, x, capitalY, word.substr(0, 1), titleColor, shadowColor, 2);
        x += layout.capitalWidths[index];
        drawTextRun(deviceContext, state.titleRegularFont, x, remainderY, word.substr(1), titleColor, shadowColor, 2);
        x += layout.remainderWidths[index];

        if (index + 1 < kSplashTitleWords.size())
            x += layout.spaceWidth + gapBonus + (static_cast<int>(index) < gapRemainder ? 1 : 0);
    }

    const std::wstring subtitlePrefix = std::wstring(L"Version ") + APP_VERSION_WSTRING + L"    Authored by: ";
    const int prefixWidth = measureTextExtent(deviceContext, state.subtitleFont, subtitlePrefix).cx;
    const int authorWidth = measureTextExtent(deviceContext, state.linkFont, kSplashAuthorName).cx;
    const int subtitleX = kSplashHorizontalPadding;

    drawTextRun(deviceContext, state.subtitleFont, subtitleX, subtitleY, subtitlePrefix, subtitleColor, shadowColor, 1);
    drawTextRun(deviceContext, state.linkFont, subtitleX + prefixWidth, subtitleY, kSplashAuthorName, linkColor, shadowColor, 1);

    state.hyperlinkRect.left = subtitleX + prefixWidth - 2;
    state.hyperlinkRect.top = subtitleY - 2;
    state.hyperlinkRect.right = subtitleX + prefixWidth + authorWidth + 2;
    state.hyperlinkRect.bottom = subtitleY + std::max(layout.subtitleMetrics.tmHeight, layout.linkMetrics.tmHeight) + 2;

    SetBkMode(deviceContext, previousBkMode);
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

    state->stopLoader.store(true);

    if (state->mouseHook)
        UnhookWindowsHookEx(state->mouseHook);

    if (state->loaderThread)
    {
        WaitForSingleObject(state->loaderThread, INFINITE);
        CloseHandle(state->loaderThread);
        state->loaderThread = nullptr;
    }

    destroySplashBackBuffer(*state);
    destroyFont(state->titleBoldFont);
    destroyFont(state->titleRegularFont);
    destroyFont(state->subtitleFont);
    destroyFont(state->linkFont);

    delete state;
}

LRESULT CALLBACK SplashMouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code >= 0 && g_splashWindow)
    {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN)
        {
            const auto* hookData = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
            RECT splashRect{};
            if (!GetWindowRect(g_splashWindow, &splashRect) || !PtInRect(&splashRect, hookData->pt))
                PostMessageW(g_splashWindow, WM_CLOSE, 0, 0);
        }
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
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

bool decodePngFileBGRAWithFactory(IWICImagingFactory* factory, const std::filesystem::path& filename, DecodedImage& image)
{
    image = {};

    if (!factory || filename.empty())
        return false;

    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    HRESULT result = factory->CreateDecoderFromFilename(
        filename.wstring().c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);

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

    return SUCCEEDED(result);
}

std::size_t countSplashFrameResources()
{
    std::size_t count = 0;
    HMODULE module = GetModuleHandleW(nullptr);
    for (int resourceId = IDR_SPLASH_FRAME_FIRST; FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA); ++resourceId)
        ++count;

    return count;
}

unsigned __stdcall SplashLoaderThreadProc(void* parameter)
{
    auto* state = static_cast<SplashState*>(parameter);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
        return 0;

    IWICImagingFactory* threadFactory = nullptr;
    const HRESULT factoryResult = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&threadFactory));

    if (FAILED(factoryResult))
    {
        if (uninitializeCom)
            CoUninitialize();
        return 0;
    }

    for (int resourceId = IDR_SPLASH_FRAME_FIRST + 1; !state->stopLoader.load(); ++resourceId)
    {
        DecodedImage image;
        if (!decodePngResourceBGRAWithFactory(threadFactory, resourceId, image))
            break;

        SplashFrame frame;
        frame.width = image.width;
        frame.height = image.height;
        frame.pixels = std::move(image.pixels);
        frame.bitmapInfo = makeBitmapInfo(frame.width, frame.height);

        {
            std::scoped_lock lock(state->framesMutex);
            state->frames.push_back(std::move(frame));
        }

        HWND hwnd = state->hwnd.load();
        if (hwnd)
            PostMessageW(hwnd, kSplashFrameReadyMessage, 0, 0);
    }

    safeRelease(threadFactory);
    if (uninitializeCom)
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
                state->currentFrame = (state->currentFrame + 1) % frameCount;
        }

        if (frameCount > 1)
            InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case kSplashFrameReadyMessage:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
    {
        SplashState* state = getSplashState(hwnd);
        POINT clickPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (state && PtInRect(&state->hyperlinkRect, clickPoint))
            ShellExecuteW(hwnd, L"open", kSplashAuthorUrl, nullptr, nullptr, SW_SHOWNORMAL);

        DestroyWindow(hwnd);
        return 0;
    }

    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_KEYDOWN:
        DestroyWindow(hwnd);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            SplashState* state = getSplashState(hwnd);
            POINT cursorPoint{};
            GetCursorPos(&cursorPoint);
            ScreenToClient(hwnd, &cursorPoint);
            if (state && PtInRect(&state->hyperlinkRect, cursorPoint))
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
                    state->currentFrame = std::min(state->currentFrame, state->frames.size() - 1);
                    const SplashFrame& frame = state->frames[state->currentFrame];
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

COLORREF getThemeColorRef()
{
    return RGB(g_themePalette.red, g_themePalette.green, g_themePalette.blue);
}

void applyWindowTheme(HWND hwnd)
{
    if (!hwnd)
        return;

    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &kDwmColorDefault, sizeof(kDwmColorDefault));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &kDwmColorDefault, sizeof(kDwmColorDefault));
    RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
}

bool decodePngResourceBGRA(int resourceID, DecodedImage& image)
{
    return decodePngResourceBGRAWithFactory(g_wicFactory, resourceID, image);
}

bool decodePngFileBGRA(const std::filesystem::path& filename, DecodedImage& image)
{
    return decodePngFileBGRAWithFactory(g_wicFactory, filename, image);
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
    if (!decodePngResourceBGRA(IDR_SPLASH_FRAME_FIRST, image))
        return false;

    auto* state = new SplashState();
    state->frames.reserve(std::max<std::size_t>(countSplashFrameResources(), 1));

    SplashFrame firstFrame;
    firstFrame.width = image.width;
    firstFrame.height = image.height;
    firstFrame.pixels = std::move(image.pixels);
    firstFrame.bitmapInfo = makeBitmapInfo(firstFrame.width, firstFrame.height);
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
    state->mouseHook = SetWindowsHookExW(WH_MOUSE_LL, SplashMouseHookProc, instance, 0);
    state->loaderThread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, SplashLoaderThreadProc, state, 0, nullptr));

    ShowWindow(splashWindow, SW_SHOWNOACTIVATE);
    UpdateWindow(splashWindow);
    SetTimer(splashWindow, kSplashTimerId, kSplashFrameIntervalMs, nullptr);

    return true;
}
