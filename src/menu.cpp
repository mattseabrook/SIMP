#include "menu.h"

#include "app.h"
#include "config.h"
#include "system.h"
#include "window.h"

#include "../resource.h"

#include "native_theme.h"

namespace
{
void centerDialogOverOwner(HWND dialog)
{
    HWND owner = GetParent(dialog);
    RECT dialogRect{};
    GetWindowRect(dialog, &dialogRect);

    RECT targetRect{};
    if (!owner || !GetWindowRect(owner, &targetRect))
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
} // namespace

void initMenu(HWND hwnd)
{
    HMENU menuBar = CreateMenu();
    HMENU fileMenu = CreatePopupMenu();
    HMENU optionsMenu = CreatePopupMenu();
    HMENU viewMenu = CreatePopupMenu();
    HMENU helpMenu = CreatePopupMenu();

    AppendMenuW(fileMenu, MF_STRING, IDM_FILE_NEW, L"&New\tCtrl+N");
    AppendMenuW(fileMenu, MF_STRING, IDM_FILE_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(fileMenu, MF_STRING, IDM_FILE_SAVE, L"&Save\tCtrl+S");
    AppendMenuW(fileMenu, MF_STRING, IDM_FILE_SAVE_DOCUMENT_AS, L"Save &As...\tCtrl+Shift+S");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, IDM_FILE_EXPORT_PNG, L"Export to &PNG...");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, IDM_FILE_EXIT, L"E&xit");
    HMENU editMenu = CreatePopupMenu();
    AppendMenuW(editMenu, MF_STRING, IDM_EDIT_UNDO, L"&Undo\tCtrl+Z");
    AppendMenuW(editMenu, MF_STRING, IDM_EDIT_REDO, L"&Redo\tCtrl+Y");
    AppendMenuW(viewMenu, MF_STRING, IDM_VIEW_GRID, L"Grid");
    AppendMenuW(viewMenu, MF_STRING, IDM_VIEW_RULERS, L"Rulers");
    AppendMenuW(viewMenu, MF_STRING, IDM_VIEW_LINES, L"Lines");
    AppendMenuW(optionsMenu, MF_STRING, IDM_OPTIONS_GRID, L"Grid...");
    AppendMenuW(optionsMenu, MF_STRING, IDM_OPTIONS_RULERS, L"Rulers...");
    AppendMenuW(optionsMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(optionsMenu, MF_STRING, IDM_OPTIONS_IMAGE, L"Image Settings...");
    AppendMenuW(helpMenu, MF_STRING, IDM_HELP_ABOUT, L"About SIMP");

    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"&File");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(editMenu), L"&Edit");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(optionsMenu), L"&Options");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), L"&View");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu), L"&Help");
    updateCanvasCaliperMenu(menuBar);

    themeNativeMenu(menuBar);
    SetMenu(hwnd, menuBar);
    DrawMenuBar(hwnd);
}

INT_PTR CALLBACK AboutDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM)
{
    switch (message)
    {
    case WM_INITDIALOG: {
        const std::wstring aboutText = L"SIMP\nSimple Image Manipulation Program\nVersion " APP_VERSION_WSTRING;
        SetDlgItemTextW(dialog, IDC_ABOUT_TEXT, aboutText.c_str());
        applyWindowTheme(dialog);
        centerDialogOverOwner(dialog);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(dialog, 0);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(dialog, 0);
        return TRUE;
    }

    return FALSE;
}

LRESULT handleMenuCommand(HWND hwnd, WPARAM wParam)
{
    switch (LOWORD(wParam))
    {
    case IDM_FILE_NEW:
        showCanvasSettingsDialog(hwnd, IDM_FILE_NEW);
        return 0;
    case IDM_FILE_OPEN:
        openCanvasCaliperDocumentDialog(hwnd);
        return 0;

    case IDM_FILE_SAVE_DOCUMENT_AS:
        saveCanvasCaliperDocumentAsDialog(hwnd);
        return 0;

    case IDM_FILE_SAVE:
        saveCanvasCaliperDocument(hwnd);
        return 0;
    case IDM_FILE_EXPORT_PNG:
        exportCanvasCaliperPngDialog(hwnd);
        return 0;
    case IDM_EDIT_UNDO:
        editCanvasCaliperHistory(hwnd, false);
        return 0;
    case IDM_EDIT_REDO:
        editCanvasCaliperHistory(hwnd, true);
        return 0;
    case IDM_FILE_EXIT:
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;

    case IDM_OPTIONS_GRID:
    case IDM_OPTIONS_RULERS:
    case IDM_OPTIONS_IMAGE:
        showCanvasSettingsDialog(hwnd, LOWORD(wParam));
        return 0;

    case IDM_VIEW_GRID:
    case IDM_VIEW_RULERS:
    case IDM_VIEW_LINES:
        toggleCanvasView(hwnd, LOWORD(wParam));
        return 0;

    case IDM_HELP_ABOUT:
        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ABOUT_DIALOG), hwnd, AboutDialogProc, 0);
        requestRender();
        return 0;
    }

    return 0;
}
