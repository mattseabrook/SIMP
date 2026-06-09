#include "window.h"

#include "app.h"
#include "config.h"
#include "menu.h"
#include "system.h"
#include "vulkan.h"

#include <algorithm>
#include <stdexcept>
#include <string>

HWND g_hwnd = nullptr;
bool g_menuActive = false;

namespace
{
const wchar_t* kWindowClassName = L"SimpMainWindowClass";
constexpr UINT kWakeRenderMessage = WM_APP + 1;
constexpr UINT_PTR kInteractiveRenderTimerId = 1;
constexpr UINT kInteractiveRenderIntervalMs = 16;

std::vector<DisplayInfo> g_displays;
bool g_rendererInitialized = false;
bool g_needsRender = true;
bool g_userIsResizing = false;
int g_minClientWidth = MIN_CLIENT_WIDTH;
int g_minClientHeight = MIN_CLIENT_HEIGHT;
DWORD g_windowedStyle = WS_OVERLAPPEDWINDOW;
WINDOWPLACEMENT g_windowedPlacement{sizeof(WINDOWPLACEMENT)};
int g_initialShowCommand = SW_SHOWNORMAL;

void refreshDisplayInfo()
{
    g_displays.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
}

const DisplayInfo* findPrimaryDisplay()
{
    for (const DisplayInfo& display : g_displays)
    {
        if (display.isPrimary)
            return &display;
    }

    return g_displays.empty() ? nullptr : &g_displays.front();
}

const DisplayInfo* findDisplayByNumber(int number)
{
    for (const DisplayInfo& display : g_displays)
    {
        if (display.number == number)
            return &display;
    }

    return findPrimaryDisplay();
}

const DisplayInfo* findDisplayByBounds(const RECT& bounds)
{
    for (const DisplayInfo& display : g_displays)
    {
        if (EqualRect(&display.bounds, &bounds))
            return &display;
    }

    return nullptr;
}

RECT fallbackMonitorBounds()
{
    RECT bounds{};
    bounds.left = 0;
    bounds.top = 0;
    bounds.right = GetSystemMetrics(SM_CXSCREEN);
    bounds.bottom = GetSystemMetrics(SM_CYSCREEN);
    return bounds;
}

RECT getPreferredMonitorWorkArea()
{
    refreshDisplayInfo();

    const DisplayInfo* display = findDisplayByNumber(config.value("display", 1));
    if (display)
        return display->workArea;

    return fallbackMonitorBounds();
}

RECT clampWindowRectToWorkArea(const RECT& rect, const RECT& workArea)
{
    RECT clamped = rect;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    if (clamped.left < workArea.left)
        OffsetRect(&clamped, workArea.left - clamped.left, 0);
    if (clamped.top < workArea.top)
        OffsetRect(&clamped, 0, workArea.top - clamped.top);
    if (clamped.right > workArea.right)
        OffsetRect(&clamped, workArea.right - clamped.right, 0);
    if (clamped.bottom > workArea.bottom)
        OffsetRect(&clamped, 0, workArea.bottom - clamped.bottom);

    if (width > (workArea.right - workArea.left))
    {
        clamped.left = workArea.left;
        clamped.right = workArea.right;
    }

    if (height > (workArea.bottom - workArea.top))
    {
        clamped.top = workArea.top;
        clamped.bottom = workArea.bottom;
    }

    return clamped;
}

RECT buildInitialWindowRect()
{
    const bool fullscreen = config.value("fullscreen", false);
    const RECT monitorBounds = getPreferredMonitorBounds();
    if (fullscreen)
        return monitorBounds;

    RECT rect{};
    rect.left = 0;
    rect.top = 0;
    rect.right = std::max(MIN_CLIENT_WIDTH, config.value("width", DEFAULT_CLIENT_WIDTH));
    rect.bottom = std::max(MIN_CLIENT_HEIGHT, config.value("height", DEFAULT_CLIENT_HEIGHT));
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, TRUE, 0);

    OffsetRect(
        &rect,
        monitorBounds.left + config.value("x", 96),
        monitorBounds.top + config.value("y", 96));

    return clampWindowRectToWorkArea(rect, getPreferredMonitorWorkArea());
}

void applyResize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;

    resizeVulkan(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));

    if (!config.value("fullscreen", false) && (!g_hwnd || !IsZoomed(g_hwnd)))
    {
        config["width"] = std::max(width, g_minClientWidth);
        config["height"] = std::max(height, g_minClientHeight);
    }

    g_needsRender = true;
}
}

BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);

    if (GetMonitorInfoW(monitor, &info))
    {
        DisplayInfo display;
        display.number = static_cast<int>(g_displays.size()) + 1;
        display.bounds = info.rcMonitor;
        display.workArea = info.rcWork;
        display.isPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        g_displays.push_back(display);
    }

    return TRUE;
}

RECT getPreferredMonitorBounds()
{
    refreshDisplayInfo();

    const DisplayInfo* display = findDisplayByNumber(config.value("display", 1));
    if (display)
        return display->bounds;

    return fallbackMonitorBounds();
}

void setMinimumClientSize(int minClientWidth, int minClientHeight)
{
    g_minClientWidth = std::max(1, minClientWidth);
    g_minClientHeight = std::max(1, minClientHeight);
}

void requestRender()
{
    g_needsRender = true;
    if (g_hwnd)
        PostMessageW(g_hwnd, kWakeRenderMessage, 0, 0);
}

bool shouldRender()
{
    return g_needsRender;
}

void renderFrame()
{
    if (!g_rendererInitialized || !g_hwnd || IsIconic(g_hwnd))
        return;

    renderFrameVk();
    g_needsRender = false;
}

void updateConfigFromWindow()
{
    if (!g_hwnd || !IsWindow(g_hwnd))
        return;

    refreshDisplayInfo();

    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    if (!GetWindowPlacement(g_hwnd, &placement))
        return;

    config["renderer"] = "VULKAN";
    config["maximized"] = placement.showCmd == SW_SHOWMAXIMIZED;

    RECT referenceRect = placement.rcNormalPosition;
    HMONITOR monitor = MonitorFromRect(&referenceRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, &monitorInfo))
    {
        config["x"] = referenceRect.left - monitorInfo.rcMonitor.left;
        config["y"] = referenceRect.top - monitorInfo.rcMonitor.top;

        if (const DisplayInfo* display = findDisplayByBounds(monitorInfo.rcMonitor))
            config["display"] = display->number;
    }

    if (!config.value("fullscreen", false) && placement.showCmd != SW_SHOWMAXIMIZED)
    {
        RECT clientRect{};
        if (GetClientRect(g_hwnd, &clientRect))
        {
            const int clientWidth = static_cast<int>(clientRect.right - clientRect.left);
            const int clientHeight = static_cast<int>(clientRect.bottom - clientRect.top);
            config["width"] = std::max(g_minClientWidth, clientWidth);
            config["height"] = std::max(g_minClientHeight, clientHeight);
        }
    }

    saveCanvasCaliperConfig();
}

void toggleFullscreen()
{
    if (!g_hwnd)
        return;

    const bool enteringFullscreen = !config.value("fullscreen", false);
    config["fullscreen"] = enteringFullscreen;

    if (enteringFullscreen)
    {
        g_windowedStyle = static_cast<DWORD>(GetWindowLongPtrW(g_hwnd, GWL_STYLE));
        g_windowedPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(g_hwnd, &g_windowedPlacement);

        SetMenu(g_hwnd, nullptr);
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);

        HMONITOR monitor = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        RECT bounds = fallbackMonitorBounds();
        if (GetMonitorInfoW(monitor, &info))
            bounds = info.rcMonitor;

        SetWindowPos(
            g_hwnd,
            HWND_TOP,
            bounds.left,
            bounds.top,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    else
    {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, g_windowedStyle);
        SetWindowPlacement(g_hwnd, &g_windowedPlacement);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        initMenu(g_hwnd);
        updateCanvasCaliperMenu(GetMenu(g_hwnd));
        DrawMenuBar(g_hwnd);
    }

    requestRender();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_COMMAND:
        if (handleCanvasCaliperCommand(hwnd, wParam, lParam))
            return 0;
        return handleMenuCommand(hwnd, wParam);

    case kWakeRenderMessage:
        return 0;

    case WM_CLOSE:
        try
        {
            save_config(getConfigPath());
        }
        catch (...)
        {
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kInteractiveRenderTimerId);
        PostQuitMessage(0);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        requestRender();
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_ENTERSIZEMOVE:
        g_userIsResizing = true;
        SetTimer(hwnd, kInteractiveRenderTimerId, kInteractiveRenderIntervalMs, nullptr);
        requestRender();
        return 0;

    case WM_EXITSIZEMOVE:
    {
        g_userIsResizing = false;
        KillTimer(hwnd, kInteractiveRenderTimerId);

        RECT clientRect{};
        if (GetClientRect(hwnd, &clientRect))
            applyResize(static_cast<int>(clientRect.right - clientRect.left), static_cast<int>(clientRect.bottom - clientRect.top));

        requestRender();
        return 0;
    }

    case WM_TIMER:
        if (wParam == kInteractiveRenderTimerId)
        {
            requestRender();
            return 0;
        }
        break;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;

        applyResize(LOWORD(lParam), HIWORD(lParam));
        layoutCanvasCaliperChrome();
        return 0;

    case WM_MOVE:
        requestRender();
        return 0;

    case WM_GETMINMAXINFO:
    {
        auto* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT frameRect{0, 0, g_minClientWidth, g_minClientHeight};
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        AdjustWindowRectEx(&frameRect, style, GetMenu(hwnd) != nullptr, exStyle);

        minMaxInfo->ptMinTrackSize.x = frameRect.right - frameRect.left;
        minMaxInfo->ptMinTrackSize.y = frameRect.bottom - frameRect.top;
        return 0;
    }

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        refreshThemePalette();
        applyWindowTheme(hwnd);
        requestRender();
        return 0;

    case WM_DISPLAYCHANGE:
        refreshDisplayInfo();
        requestRender();
        return 0;

    case WM_DPICHANGED:
        if (lParam)
        {
            RECT* suggestedRect = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(
                hwnd,
                nullptr,
                suggestedRect->left,
                suggestedRect->top,
                suggestedRect->right - suggestedRect->left,
                suggestedRect->bottom - suggestedRect->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        requestRender();
        return 0;

    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN && (HIWORD(lParam) & KF_ALTDOWN) != 0)
        {
            toggleFullscreen();
            return 0;
        }
        break;
    }

    LRESULT canvasResult = 0;
    if (handleCanvasCaliperWindowMessage(hwnd, message, wParam, lParam, canvasResult))
        return canvasResult;

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void initWindow(HINSTANCE instance, bool showWindowNow)
{
    refreshDisplayInfo();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Failed to register the SIMP window class.");

    const RECT windowRect = buildInitialWindowRect();
    const bool fullscreen = config.value("fullscreen", false);
    const DWORD style = fullscreen ? (WS_POPUP | WS_VISIBLE) : WS_OVERLAPPEDWINDOW;

    const std::wstring title(windowTitle.begin(), windowTitle.end());
    g_hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        title.c_str(),
        style,
        windowRect.left,
        windowRect.top,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!g_hwnd)
        throw std::runtime_error("Failed to create the SIMP main window.");

    if (!fullscreen)
        initMenu(g_hwnd);

    refreshThemePalette();
    applyWindowTheme(g_hwnd);
    initializeVulkan(g_hwnd);
    initializeCanvasCaliper(g_hwnd);
    updateCanvasCaliperMenu(GetMenu(g_hwnd));
    DrawMenuBar(g_hwnd);
    g_rendererInitialized = true;

    g_initialShowCommand = fullscreen ? SW_SHOWNORMAL : (config.value("maximized", false) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);

    if (showWindowNow)
        showMainWindow();
}

void showMainWindow()
{
    if (!g_hwnd)
        return;

    ShowWindow(g_hwnd, g_initialShowCommand);
    UpdateWindow(g_hwnd);
    requestRender();
}

bool processEvents()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
            return false;

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return true;
}

void cleanupWindow()
{
    if (g_rendererInitialized)
    {
        shutdownCanvasCaliper();
        cleanupVulkan();
        g_rendererInitialized = false;
    }

    g_hwnd = nullptr;
}
