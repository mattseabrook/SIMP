#pragma once
#include <windows.h>

void initializeNativeTheme();
void shutdownNativeTheme();
void themeNativeMenu(HMENU menu);
bool handleNativeThemeMessage(HWND, UINT, WPARAM, LPARAM, LRESULT &);
