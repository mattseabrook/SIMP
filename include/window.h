#ifndef SIMP_WINDOW_H
#define SIMP_WINDOW_H

#include <vector>

#include <windows.h>

struct DisplayInfo
{
    int number = 0;
    RECT bounds{};
    RECT workArea{};
    bool isPrimary = false;
};

extern HWND g_hwnd;
extern bool g_menuActive;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC monitorDc, LPRECT monitorRect, LPARAM userData);

void initWindow(HINSTANCE instance, bool showWindowNow = true);
void showMainWindow();
bool processEvents();
bool shouldRender();
void renderFrame();
void cleanupWindow();
void requestRender();
void setMinimumClientSize(int minClientWidth, int minClientHeight);
void updateConfigFromWindow();
RECT getPreferredMonitorBounds();
void toggleFullscreen();

#endif