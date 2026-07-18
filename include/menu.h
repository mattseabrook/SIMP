#ifndef SIMP_MENU_H
#define SIMP_MENU_H

#include <windows.h>

enum MenuCommand : UINT
{
    IDM_FILE_OPEN = 40001,
    IDM_FILE_EXIT = 40002,
    IDM_VIEW_GRID = 40003,
    IDM_VIEW_RULERS = 40004,
    IDM_HELP_ABOUT = 40005,
    IDM_FILE_OPEN_DOCUMENT = 40006,
    IDM_FILE_SAVE_DOCUMENT_AS = 40007
};

void initMenu(HWND hwnd);
LRESULT handleMenuCommand(HWND hwnd, WPARAM wParam);
INT_PTR CALLBACK AboutDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);

#endif
