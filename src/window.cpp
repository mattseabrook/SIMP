// window.cpp

#include <Windows.h>
#include <windowsx.h>

#include "../resource.h"

#include "window.h"
#include "vulkan.h"
#include "d2d.h"
#include "config.h"
#include "game.h"
#include "audio.h"
#include "menu.h"
#include "map_overlay.h"
#include "raycast.h"

//
// Debug logging macro - writes to DebugView/debugger output
//
#define DBG_LOG(msg) OutputDebugStringA("[Phantom:Window] " msg "\n")
#define DBG_LOGF(fmt, ...) do { char _dbg_buf[512]; snprintf(_dbg_buf, sizeof(_dbg_buf), "[Phantom:Window] " fmt "\n", __VA_ARGS__); OutputDebugStringA(_dbg_buf); } while(0)

HWND g_hwnd = nullptr;
bool g_menuActive = false;
HHOOK g_mouseHook = NULL;
bool g_userIsResizing = false;
static int g_pendingResizeW = 0;
static int g_pendingResizeH = 0;
static bool g_pendingResize = false;
static bool g_modeSwitching = false;
RendererType renderer;
bool g_rendererInitialized = false;
float scaleFactor = 1.0f;
DWORD g_windowedStyle = WS_OVERLAPPEDWINDOW;
WINDOWPLACEMENT g_windowedPlacement = {sizeof(WINDOWPLACEMENT)};

static int g_minClientWidth = MIN_CLIENT_WIDTH;
static int g_minClientHeight = MIN_CLIENT_HEIGHT;

void setMinimumClientSize(int minClientWidth, int minClientHeight)
{
	if (minClientWidth > 0)
		g_minClientWidth = minClientWidth;
	if (minClientHeight > 0)
		g_minClientHeight = minClientHeight;
}

// Use arrays indexed by enum for O(1) lookup instead of std::map O(log n)
// RendererType::VULKAN = 0, RendererType::DIRECTX = 1
using RenderFunc = void(*)();
static constexpr RenderFunc initializeRendererFuncs[] = { initializeVulkan, initializeD2D };
static constexpr RenderFunc renderFrameFuncsArr[] = { renderFrameVk, renderFrameD2D };
static constexpr RenderFunc renderRaycastFuncsArr[] = { renderFrameRaycastVk, renderFrameRaycast };
static constexpr RenderFunc renderMenuFuncsArr[] = { renderFrameMenuVk, renderFrameMenu };
static constexpr RenderFunc cleanupFuncsArr[] = { cleanupVulkan, cleanupD2D };

LRESULT HandleMove(HWND hwnd)
{
	if (!state.ui.enabled)
		return 0;
	RECT rect;
	GetWindowRect(hwnd, &rect);
	HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEX info = {sizeof(MONITORINFOEX)};
	if (GetMonitorInfo(mon, &info))
	{
		state.ui.x = rect.left - info.rcMonitor.left;
		state.ui.y = rect.top - info.rcMonitor.top;
		if (!config["fullscreen"].get<bool>())
		{
			config["x"] = state.ui.x;
			config["y"] = state.ui.y;
		}
		for (const auto &disp : state.ui.displays)
		{
			if (EqualRect(&disp.bounds, &info.rcMonitor))
				config["display"] = disp.number;
		}
	}
	return 0;
}

void HandleSizing(WPARAM wParam, LPARAM lParam)
{
	RECT *rect = reinterpret_cast<RECT *>(lParam);
	RECT frame = {0, 0, g_minClientWidth, g_minClientHeight};
	AdjustWindowRectEx(&frame, GetWindowLong(g_hwnd, GWL_STYLE), TRUE, GetWindowLong(g_hwnd, GWL_EXSTYLE));
	int minW = frame.right - frame.left;
	if (rect->right - rect->left < minW)
	{
		(wParam == WMSZ_LEFT || wParam == WMSZ_TOPLEFT || wParam == WMSZ_BOTTOMLEFT) ? rect->left = rect->right - minW : rect->right = rect->left + minW;
	}
	int clientW = rect->right - rect->left - (frame.right - frame.left - g_minClientWidth);
	int reqH = (clientW * 9) / 16 + (frame.bottom - frame.top - g_minClientHeight);
	(wParam == WMSZ_TOP || wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT) ? rect->top = rect->bottom - reqH : rect->bottom = rect->top + reqH;
}

static void ApplyResize(int newW, int newH)
{
	if (newW <= 0 || newH <= 0)
		return;
	if (g_rendererInitialized && state.ui.width == newW && state.ui.height == newH)
		return;

	scaleFactor = state.raycast.enabled ? 1.0f : static_cast<float>(newW) / CONTENT_WIDTH;

	if (g_rendererInitialized)
	{
		if (renderer == RendererType::DIRECTX)
		{
			if (d2dCtx.dc)
			{
				d2dCtx.dc->SetTarget(nullptr);
				d2dCtx.dc->Flush();
			}
			for (auto &rtv : d2dCtx.backbufferRTVs)
				rtv.Reset();
			for (auto &bmp : d2dCtx.backbufferBitmaps)
				bmp.Reset();
			if (d2dCtx.d3dContext)
				d2dCtx.d3dContext->Flush();

			HRESULT hr = d2dCtx.swapchain->ResizeBuffers(
				2, static_cast<UINT>(newW), static_cast<UINT>(newH), DXGI_FORMAT_UNKNOWN,
				DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
			if (FAILED(hr))
			{
				// Retry once after extra release/flush (mirrors v64tng robustness).
				if (d2dCtx.dc)
				{
					d2dCtx.dc->SetTarget(nullptr);
					d2dCtx.dc->Flush();
				}
				for (auto &rtv : d2dCtx.backbufferRTVs)
					rtv.Reset();
				for (auto &bmp : d2dCtx.backbufferBitmaps)
					bmp.Reset();
				if (d2dCtx.d3dContext)
					d2dCtx.d3dContext->Flush();
				hr = d2dCtx.swapchain->ResizeBuffers(
					2, static_cast<UINT>(newW), static_cast<UINT>(newH), DXGI_FORMAT_UNKNOWN,
					DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
				if (FAILED(hr))
					throw std::runtime_error("Failed ResizeBuffers in ApplyResize");
			}

			if (d2dCtx.frameLatencyWaitableObject)
			{
				CloseHandle(d2dCtx.frameLatencyWaitableObject);
				d2dCtx.frameLatencyWaitableObject = nullptr;
			}
			d2dCtx.swapchain->SetMaximumFrameLatency(1);
			d2dCtx.frameLatencyWaitableObject = d2dCtx.swapchain->GetFrameLatencyWaitableObject();

			recreateBackbufferTargets();
			// Preserve the current content texture dimensions.  If a non-standard
			// PVX is playing (e.g. 640x320), we must keep the texture at those
			// dimensions so the renderer reads pixel data with the correct stride.
			UINT texW, texH;
			if (state.raycast.enabled) {
				texW = static_cast<UINT>(newW);
				texH = static_cast<UINT>(newH);
			} else if (d2dCtx.textureWidth > 0 && d2dCtx.textureHeight > 0) {
				texW = d2dCtx.textureWidth;
				texH = d2dCtx.textureHeight;
			} else {
				texW = CONTENT_WIDTH;
				texH = CONTENT_HEIGHT;
			}
			resizeTexture(texW, texH);
		}
		else
		{
			recreateSwapchain(newW, newH);
			if (state.raycast.enabled)
				resizeVulkanTexture(newW, newH);
			else if (vkCtx.textureWidth > 0 && vkCtx.textureHeight > 0)
				resizeVulkanTexture(vkCtx.textureWidth, vkCtx.textureHeight);
		}
	}

	if (state.ui.width != newW || state.ui.height != newH)
	{
		state.ui.width = newW;
		state.ui.height = newH;
		if (!config["fullscreen"].get<bool>())
			config["width"] = newW;
		// Rendering from inside WM_SIZE can stall mode switches on Vulkan.
		// Mark dirty and let the main loop present on the next tick.
		if (g_rendererInitialized && renderer == RendererType::VULKAN)
			state.frameTiming.dirtyFrame = true;
		else
			maybeRenderFrame(true);
	}
}

LRESULT HandleSize(HWND hwnd, WPARAM wParam)
{
	// Allow resizing even before UI is fully enabled so intros scale correctly
	if (wParam == SIZE_MINIMIZED)
		return 0;
	RECT client;
	GetClientRect(hwnd, &client);
	int newW = client.right, newH = client.bottom;

	// Defer heavy resize work while dragging or while toggling fullscreen/windowed.
	if (g_userIsResizing || g_modeSwitching)
	{
		g_pendingResizeW = newW;
		g_pendingResizeH = newH;
		g_pendingResize = true;
		return 0;
	}

	ApplyResize(newW, newH);
	return 0;
}

LRESULT HandleTimer(HWND hwnd, WPARAM wParam)
{
	if (wParam == 0x7C0B)
	{
		KillTimer(hwnd, 0x7C0B);
		g_menuActive = false;
		return 0;
	}
	return 0;
}

LRESULT HandlePaint(HWND hwnd)
{
	PAINTSTRUCT ps;
	BeginPaint(hwnd, &ps);
	maybeRenderFrame(true);
	EndPaint(hwnd, &ps);
	return 0;
}

LRESULT HandleSetCursor(LPARAM lParam)
{
	if (LOWORD(lParam) != HTCLIENT)
		return DefWindowProc(g_hwnd, WM_SETCURSOR, WPARAM(g_hwnd), lParam);
	SetCursor(LoadCursor(nullptr, IDC_ARROW));
	return TRUE;
}

LRESULT HandleNCHitTest(HWND hwnd, LPARAM lParam)
{
	// Rely on the default system hit-testing to avoid misclassifying client area as resize borders.
	// This matches v64tng behavior and prevents stray resize cursors interfering with raycast mouse-look.
	return DefWindowProc(hwnd, WM_NCHITTEST, 0, lParam);
}

LRESULT HandleMouseMove(LPARAM /*lParam*/)
{
	if (state.raycast.enabled)
		handleRaycastMouseMove();
	return 0;
}

LRESULT HandleLButtonDown(LPARAM lParam)
{
	POINT pt = {LOWORD(lParam), HIWORD(lParam)};
	
	// Handle menu clicks
	if (state.menu.enabled)
	{
		float menuY = static_cast<float>(state.ui.height) * 0.57f;
		float itemHeight = 100.0f;
		const int itemCount = 4; // NEW GAME, MULTIPLAYER, OPTIONS, EXIT
		
		for (int i = 0; i < itemCount; ++i)
		{
			float topY = menuY + i * itemHeight;
			float bottomY = menuY + (i + 1) * itemHeight;
			
			if (pt.y >= topY && pt.y <= bottomY)
			{
				state.menu.selectedItem = i;
				state.menu.itemClicked = true;
				return 0;
			}
		}
		return 0;
	}
	
	float nx = static_cast<float>(pt.x) / state.ui.width * 100.0f;
	float ny = static_cast<float>(pt.y) / state.ui.height * 100.0f;
	if (state.animation.isPlaying || state.transient_animation.isPlaying || !state.currentPVX || !state.view)
		return 0;
	int maxZ = -1;
	size_t tgtIdx = 0;
	enum class Tgt
	{
		None,
		Nav,
		Hot
	} tgt = Tgt::None;
	for (size_t i = 0; i < state.view->navigations.size(); ++i)
	{
		const auto &area = state.view->navigations[i].area;
		if (nx >= area.x && nx <= area.x + area.width && ny >= area.y && ny <= area.y + area.height && area.z_index > maxZ)
		{
			maxZ = area.z_index;
			tgtIdx = i;
			tgt = Tgt::Nav;
		}
	}
	for (size_t i = 0; i < state.view->hotspots.size(); ++i)
	{
		const auto &area = state.view->hotspots[i].area;
		if (nx >= area.x && nx <= area.x + area.width && ny >= area.y && ny <= area.y + area.height && area.z_index > maxZ)
		{
			maxZ = area.z_index;
			tgtIdx = i;
			tgt = Tgt::Hot;
		}
	}
	if (tgt == Tgt::Nav)
	{
		state.current_view = state.view->navigations[tgtIdx].next_view;
		state.animation_sequence.clear();
	}
	else if (tgt == Tgt::Hot && state.view->hotspots[tgtIdx].action)
	{
		state.view->hotspots[tgtIdx].action();
	}
	return 0;
}

LRESULT HandleMenuLoop(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	DBG_LOG("HandleMenuLoop() entered");
	g_menuActive = true;
	DBG_LOG("Calling wavPause()...");
	wavPause();
	DBG_LOG("wavPause() returned");
	SetCursor(LoadCursor(nullptr, IDC_ARROW));
	DBG_LOG("HandleMenuLoop() calling DefWindowProc");
	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT HandleExitMenuLoop(HWND hwnd)
{
	SetTimer(hwnd, 0x7C0B, 50, NULL);
	wavResume();
	return DefWindowProc(hwnd, WM_EXITMENULOOP, 0, 0);
}

LRESULT HandleDestroy()
{
	if (g_mouseHook)
		UnhookWindowsHookEx(g_mouseHook), g_mouseHook = NULL;
	PostQuitMessage(0);
	return 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_COMMAND:
		return HandleMenuCommand(hwnd, wParam);
	case WM_MOVE:
		return HandleMove(hwnd);
	case WM_SIZING:
		HandleSizing(wParam, lParam);
		return TRUE;
	case WM_SIZE:
		return HandleSize(hwnd, wParam);
	case WM_TIMER:
		return HandleTimer(hwnd, wParam);
	case WM_PAINT:
		return HandlePaint(hwnd);
	case WM_SETCURSOR:
		return HandleSetCursor(lParam);
	case WM_NCHITTEST:
		return HandleNCHitTest(hwnd, lParam);
	case WM_MOUSEMOVE:
		return HandleMouseMove(lParam);
	case WM_LBUTTONDOWN:
		return HandleLButtonDown(lParam);
	case WM_MOUSEWHEEL:
		if (state.raycast.enabled && state.raycast.showMapOverlay)
		{
			const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
			float zoomStep = 0.3f;
			if (wheelDelta > 0)
				state.raycast.mapOverlayZoom = std::min(state.raycast.mapOverlayZoom + zoomStep, 12.0f);
			else if (wheelDelta < 0)
				state.raycast.mapOverlayZoom = std::max(state.raycast.mapOverlayZoom - zoomStep, 1.0f);
			state.frameTiming.dirtyFrame = true;
			return 0;
		}
		if (state.menu.enabled)
		{
			auto moveSelection = [&](int delta)
			{
				int count = 4; // NEW GAME, MULTIPLAYER, OPTIONS, EXIT
				state.menu.selectedItem = (state.menu.selectedItem + delta + count) % count;
				state.frameTiming.dirtyFrame = true;
			};

			const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
			int steps = wheelDelta / WHEEL_DELTA;
			if (steps > 0)
			{
				for (int i = 0; i < steps; ++i)
					moveSelection(-1);
				return 0;
			}
			if (steps < 0)
			{
				for (int i = 0; i < -steps; ++i)
					moveSelection(1);
				return 0;
			}
			return 0;
		}
		break;
	case WM_KEYDOWN:
		if (state.menu.enabled)
		{
			auto moveSelection = [&](int delta)
			{
				int count = 4; // NEW GAME, MULTIPLAYER, OPTIONS, EXIT
				state.menu.selectedItem = (state.menu.selectedItem + delta + count) % count;
				state.frameTiming.dirtyFrame = true;
			};

			if (wParam == VK_UP || wParam == 'W' || wParam == 'w')
			{
				moveSelection(-1);
				return 0;
			}
			if (wParam == VK_DOWN || wParam == 'S' || wParam == 's')
			{
				moveSelection(1);
				return 0;
			}
			if (wParam == VK_SPACE || wParam == VK_RETURN)
			{
				state.menu.itemClicked = true;
				return 0;
			}
		}
		else if (state.raycast.enabled)
		{
			if (wParam == 'M' || wParam == 'm')
				state.raycast.showMapOverlay = !state.raycast.showMapOverlay;
			else
				raycastKeyDown(wParam);
		}
		return 0;
	case WM_KEYUP:
		if (state.raycast.enabled)
			raycastKeyUp(wParam);
		return 0;
	case WM_SYSKEYDOWN:
		if (wParam == VK_RETURN)
		{
			toggleFullscreen();
			return 0;
		}
		if (state.menu.enabled && wParam == VK_F4)
		{
			save_config("config.json");
			::PostQuitMessage(0);
			return 0;
		}
		break;
	case WM_SYSCHAR:
		if (wParam == VK_RETURN)
			return 0;
		break;
	case WM_ENTERSIZEMOVE:
		g_userIsResizing = true;
		wavPause();
		break;
	case WM_EXITSIZEMOVE:
		g_userIsResizing = false;
		if (g_pendingResize)
		{
			ApplyResize(g_pendingResizeW, g_pendingResizeH);
			g_pendingResize = false;
		}
		wavResume();
		break;
	case WM_ENTERMENULOOP:
	case WM_INITMENU:
	case WM_INITMENUPOPUP:
		return HandleMenuLoop(hwnd, uMsg, wParam, lParam);
	case WM_EXITMENULOOP:
		return HandleExitMenuLoop(hwnd);
	case WM_CLOSE:
		save_config("config.json");
		wavStop();
		return DefWindowProc(hwnd, uMsg, wParam, lParam);
	case WM_DESTROY:
		wavStop();
		return HandleDestroy();
	}
	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode >= 0 && g_menuActive && wParam == WM_MOUSEMOVE)
		SetCursor(LoadCursor(NULL, IDC_ARROW));
	return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

void initHandlers() {}

BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM)
{
	static int count = 0;
	MONITORINFOEX info = {sizeof(MONITORINFOEX)};
	if (GetMonitorInfo(hMonitor, &info))
		state.ui.displays.push_back({++count, info.rcMonitor, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
	return TRUE;
}

void toggleFullscreen()
{
	g_modeSwitching = true;
	g_pendingResize = false;

	bool entering = !config["fullscreen"].get<bool>();
	config["fullscreen"] = entering;
	if (entering)
	{
		g_windowedStyle = GetWindowLong(g_hwnd, GWL_STYLE);
		g_windowedPlacement.length = sizeof(WINDOWPLACEMENT);
		GetWindowPlacement(g_hwnd, &g_windowedPlacement);

		// Use the monitor the window is currently on, not the config display setting
		HMONITOR currentMonitor = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFOEX monitorInfo = {sizeof(MONITORINFOEX)};
		const DisplayInfo *sel = nullptr;

		if (GetMonitorInfo(currentMonitor, &monitorInfo))
		{
			// Find the matching display info
			for (const auto &disp : state.ui.displays)
			{
				if (EqualRect(&disp.bounds, &monitorInfo.rcMonitor))
				{
					sel = &disp;
					break;
				}
			}
		}

		// Fallback to config display if current monitor not found
		if (!sel)
		{
			int tgt = config["display"];
			for (const auto &disp : state.ui.displays)
			{
				if (disp.number == tgt || (!sel && disp.isPrimary))
					sel = &disp;
			}
		}

			if (!sel && !state.ui.displays.empty())
				sel = &state.ui.displays.front();

			RECT targetBounds = {};
			if (sel)
			{
				targetBounds = sel->bounds;
			}
			else if (GetMonitorInfo(currentMonitor, &monitorInfo))
			{
				targetBounds = monitorInfo.rcMonitor;
			}
			else
			{
				targetBounds.left = 0;
				targetBounds.top = 0;
				targetBounds.right = GetSystemMetrics(SM_CXSCREEN);
				targetBounds.bottom = GetSystemMetrics(SM_CYSCREEN);
			}

		SetWindowLong(g_hwnd, GWL_STYLE, g_windowedStyle & ~WS_OVERLAPPEDWINDOW);
		SetWindowPos(g_hwnd, HWND_TOP,
					 targetBounds.left, targetBounds.top,
					 targetBounds.right - targetBounds.left,
					 targetBounds.bottom - targetBounds.top,
					 SWP_FRAMECHANGED);
		ShowWindow(g_hwnd, SW_SHOW);
		SetMenu(g_hwnd, NULL);
	}
	else
	{
		SetWindowLong(g_hwnd, GWL_STYLE, g_windowedStyle);
		SetWindowPos(g_hwnd, NULL, g_windowedPlacement.rcNormalPosition.left, g_windowedPlacement.rcNormalPosition.top,
					 g_windowedPlacement.rcNormalPosition.right - g_windowedPlacement.rcNormalPosition.left,
					 g_windowedPlacement.rcNormalPosition.bottom - g_windowedPlacement.rcNormalPosition.top, SWP_FRAMECHANGED);
		ShowWindow(g_hwnd, SW_SHOWNORMAL);
		initMenu(g_hwnd);
	}

	g_modeSwitching = false;

	// Apply exactly one resize after mode switch to avoid double swapchain churn.
	if (g_pendingResize)
	{
		const int pendingW = g_pendingResizeW;
		const int pendingH = g_pendingResizeH;
		g_pendingResize = false;
		ApplyResize(pendingW, pendingH);
	}
	else
	{
		RECT client = {};
		if (GetClientRect(g_hwnd, &client))
			ApplyResize(client.right, client.bottom);
	}
}

void initWindow()
{
	DBG_LOG("initWindow() entered");
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // DPI opt
	DBG_LOG("DPI awareness set");
	initHandlers();
	DBG_LOG("Handlers initialized");
	state.ui.displays.clear();
	EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
	DBG_LOGF("Found %zu displays", state.ui.displays.size());
	int tgtDisp = config["display"];
	const DisplayInfo *sel = nullptr;
	for (const auto &disp : state.ui.displays)
	{
		if (disp.number == tgtDisp || (!sel && disp.isPrimary))
			sel = &disp;
	}
	if (config["fullscreen"])
	{
		state.ui.width = GetSystemMetrics(SM_CXSCREEN);
		state.ui.height = GetSystemMetrics(SM_CYSCREEN);
		DBG_LOGF("Fullscreen mode: %dx%d", state.ui.width, state.ui.height);
	}
	else
	{
		if (config["width"].get<int>() & 1)
			config["width"] = config["width"].get<int>() + 1;
		state.ui.width = config["width"];
		state.ui.height = (state.ui.width * 9) / 16;
		DBG_LOGF("Windowed mode: %dx%d", state.ui.width, state.ui.height);
	}
	state.ui.x = config["x"];
	state.ui.y = config["y"];
	POINT pos = sel ? POINT{sel->bounds.left + state.ui.x, sel->bounds.top + state.ui.y} : POINT{CW_USEDEFAULT, CW_USEDEFAULT};
	DBG_LOG("Loading icon...");
	HICON icon = static_cast<HICON>(LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR));
	DBG_LOG("Registering window class...");
	WNDCLASSEX wc = {sizeof(WNDCLASSEX), 0, WindowProc, 0, 0, GetModuleHandle(nullptr), icon, nullptr, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)), nullptr, L"PhantomEngineClass", icon};
	if (!RegisterClassEx(&wc))
	{
		DBG_LOG("FAILED to register window class!");
		throw std::runtime_error("Failed register class");
	}
	DBG_LOG("Window class registered");
	RECT rect = {0, 0, state.ui.width, state.ui.height};
	DWORD style = config["fullscreen"].get<bool>() ? WS_POPUP : WS_OVERLAPPEDWINDOW;
	if (!config["fullscreen"].get<bool>())
	{
		// Use AdjustWindowRectEx to be precise, matching CreateWindowEx default (0 exStyle)
		// Pass TRUE for bMenu because we call initMenu() immediately after creation
		AdjustWindowRectEx(&rect, style, TRUE, 0);
	}
	DBG_LOG("Creating window...");
	g_hwnd = CreateWindowEx(0, wc.lpszClassName, std::wstring(windowTitle.begin(), windowTitle.end()).c_str(), style, pos.x, pos.y, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
	if (!g_hwnd)
	{
		DBG_LOG("FAILED to create window!");
		throw std::runtime_error("Failed create window");
	}
	DBG_LOGF("Window created: HWND=%p", (void*)g_hwnd);
	if (icon)
	{
		DBG_LOG("Setting window icons...");
		SendMessage(g_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
		SendMessage(g_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
		DBG_LOG("Window icons set");
	}
	if (!config["fullscreen"])
	{
		DBG_LOG("Windowed mode: initializing menu...");
		initMenu(g_hwnd);
		DBG_LOG("Menu initialized");
	}
	renderer = (config["renderer"] == "VULKAN") ? RendererType::VULKAN : RendererType::DIRECTX;
	DBG_LOGF("Renderer type: %s", (renderer == RendererType::VULKAN) ? "VULKAN" : "DIRECTX");
	// Vulkan initialization can take noticeably longer on some drivers; show the
	// window first so startup remains responsive instead of appearing frozen.
	if (renderer == RendererType::VULKAN)
	{
		ShowWindow(g_hwnd, SW_SHOW);
		UpdateWindow(g_hwnd);
		DBG_LOG("Window shown before Vulkan init");
	}
	DBG_LOG("Initializing renderer...");
	initializeRendererFuncs[static_cast<int>(renderer)]();
	g_rendererInitialized = true;
	DBG_LOG("Renderer initialized");
	if (renderer == RendererType::DIRECTX)
	{
		ShowWindow(g_hwnd, SW_SHOW);
		DBG_LOG("Window shown");
	}
	g_mouseHook = SetWindowsHookEx(WH_MOUSE, MouseHookProc, NULL, GetCurrentThreadId());
	if (state.raycast.enabled)
	{
		// Raycast mode: cursor is hidden/captured by raycast init.
		// Avoid registering raw input here since we don't handle WM_INPUT in this WindowProc.
		while (ShowCursor(FALSE) >= 0)
			;
	}
	DBG_LOG("initWindow() complete");
}

bool processEvents()
{
	MSG msg;
	while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT)
			return false;
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return true;
}

void renderFrame()
{
	if (!g_rendererInitialized)
		return; // Don't render before renderer is initialized
	int idx = static_cast<int>(renderer);
	if (state.raycast.enabled)
		renderRaycastFuncsArr[idx]();
	else if (state.menu.enabled)
		renderMenuFuncsArr[idx]();
	else
		renderFrameFuncsArr[idx]();
}

void cleanupWindow()
{
	// Ensure the OS cursor is visible again on shutdown
	while (ShowCursor(TRUE) < 0)
		;
	cleanupFuncsArr[static_cast<int>(renderer)]();
}

void resizeContentTexture(uint32_t width, uint32_t height)
{
	if (renderer == RendererType::DIRECTX)
		resizeTexture(width, height);
	else
		resizeVulkanTexture(width, height);
}

int getDisplayRefreshRate()
{
#ifdef _WIN32
    if (!g_hwnd) return 60;
    HMONITOR hMonitor = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hMonitor) return 60;
    
    MONITORINFOEXA mi;
    mi.cbSize = sizeof(MONITORINFOEXA);
    if (GetMonitorInfoA(hMonitor, &mi))
    {
        DEVMODEA dm;
        dm.dmSize = sizeof(DEVMODEA);
        dm.dmDriverExtra = 0;
        if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        {
            if (dm.dmDisplayFrequency > 1)
            {
                return static_cast<int>(dm.dmDisplayFrequency);
            }
        }
    }
#endif
    return 60; // Safe fallback
}
