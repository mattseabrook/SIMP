#pragma once
#include <windows.h>
#include "menu_commands.h"

void initMenu(HWND hwnd);
LRESULT handleMenuCommand(HWND hwnd, WPARAM wParam);
INT_PTR CALLBACK AboutDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
