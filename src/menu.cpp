#include "menu.h"

#include "app.h"
#include "config.h"
#include "system.h"
#include "window.h"

#include "../resource.h"

#include <uxtheme.h>

namespace
{
HBRUSH g_dialogBrush = nullptr;

HBRUSH dialogBrush()
{
    if (!g_dialogBrush)
        g_dialogBrush = CreateSolidBrush(RGB(8, 10, 14));
    return g_dialogBrush;
}

void centerDialogOverOwner(HWND dialog)
{
    HWND owner = GetParent(dialog);
    RECT dialogRect{};
    GetWindowRect(dialog, &dialogRect);

    RECT targetRect{};
    if (owner && GetWindowRect(owner, &targetRect))
    {
    }
    else
    {
        targetRect.left = 0;
        targetRect.top = 0;
        targetRect.right = GetSystemMetrics(SM_CXSCREEN);
        targetRect.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    const int dialogWidth = dialogRect.right - dialogRect.left;
    const int dialogHeight = dialogRect.bottom - dialogRect.top;
    const int x = targetRect.left + ((targetRect.right - targetRect.left) - dialogWidth) / 2;
    const int y = targetRect.top + ((targetRect.bottom - targetRect.top) - dialogHeight) / 2;

    SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}
}

void initMenu(HWND hwnd)
{
    HMENU menuBar = CreateMenu();
    HMENU fileMenu = CreatePopupMenu();
    HMENU viewMenu = CreatePopupMenu();
    HMENU helpMenu = CreatePopupMenu();

    AppendMenuW(fileMenu, MF_STRING, IDM_FILE_OPEN, L"Open PNG...");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, IDM_FILE_EXIT, L"Exit");
    AppendMenuW(viewMenu, MF_STRING, IDM_VIEW_GRID, L"Grid");
    AppendMenuW(viewMenu, MF_STRING, IDM_VIEW_RULERS, L"Rulers");
    AppendMenuW(helpMenu, MF_STRING, IDM_HELP_ABOUT, L"About SIMP");

    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"File");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), L"View");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu), L"Help");
    updateCanvasCaliperMenu(menuBar);

    SetMenu(hwnd, menuBar);
    DrawMenuBar(hwnd);
}

INT_PTR CALLBACK AboutDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM)
{
    switch (message)
    {
    case WM_INITDIALOG:
    {
        const std::wstring aboutText = L"SIMP\nSimple Image Manipulation Program\nVersion " APP_VERSION_WSTRING;
        g_menuActive = true;
        SetDlgItemTextW(dialog, IDC_ABOUT_TEXT, aboutText.c_str());
        applyWindowTheme(dialog);
        SetWindowTheme(dialog, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(GetDlgItem(dialog, IDOK), L"DarkMode_Explorer", nullptr);
        centerDialogOverOwner(dialog);
        return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(242, 244, 247));
        SetBkColor(dc, RGB(8, 10, 14));
        return reinterpret_cast<INT_PTR>(dialogBrush());
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            g_menuActive = false;
            EndDialog(dialog, 0);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        g_menuActive = false;
        EndDialog(dialog, 0);
        return TRUE;
    }

    return FALSE;
}

LRESULT handleMenuCommand(HWND hwnd, WPARAM wParam)
{
    switch (LOWORD(wParam))
    {
    case IDM_FILE_OPEN:
        openCanvasCaliperImageDialog(hwnd);
        return 0;

    case IDM_FILE_EXIT:
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;

    case IDM_VIEW_GRID:
        setCanvasCaliperGridVisible(!canvasCaliperGridVisible());
        DrawMenuBar(hwnd);
        return 0;

    case IDM_VIEW_RULERS:
        setCanvasCaliperRulersVisible(!canvasCaliperRulersVisible());
        DrawMenuBar(hwnd);
        return 0;

    case IDM_HELP_ABOUT:
        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ABOUT_DIALOG), hwnd, AboutDialogProc, 0);
        requestRender();
        return 0;
    }

    return 0;
}
