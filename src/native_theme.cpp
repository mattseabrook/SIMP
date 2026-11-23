#include "native_theme.h"
#include "editor_theme.h"
#include "system.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
using namespace EditorTheme;
constexpr UINT_PTR kThemeSubclass = 0x53494d50;
constexpr wchar_t kHover[] = L"SIMP.ThemeHover";
constexpr wchar_t kStyled[] = L"SIMP.Themed";
constexpr wchar_t kOwnCombo[] = L"SIMP.OwnCombo";
HHOOK g_hook = nullptr;
using AllowDarkWindow = bool(WINAPI *)(HWND, bool);
AllowDarkWindow g_allowDarkWindow = nullptr;
std::unordered_map<COLORREF, HBRUSH> g_brushes;
std::unordered_map<UINT, HFONT> g_menuFonts;
struct MenuItem
{
    std::wstring text;
    bool bar = false, separator = false;
};
std::vector<std::unique_ptr<MenuItem>> g_menuItems;

HBRUSH brush(COLORREF color)
{
    auto [at, inserted] = g_brushes.try_emplace(color, nullptr);
    if (inserted)
        at->second = CreateSolidBrush(color);
    return at->second;
}
int dpiScale(HWND window, int value)
{
    return MulDiv(value, GetDpiForWindow(window), 96);
}
HFONT menuFont(HWND window)
{
    const UINT dpi = GetDpiForWindow(window);
    auto [at, inserted] = g_menuFonts.try_emplace(dpi, nullptr);
    if (inserted)
    {
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
        at->second = CreateFontIndirectW(&metrics.lfMenuFont);
    }
    return at->second ? at->second : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}
void line(HDC dc, int x, int y, int x2, int y2, COLORREF color)
{
    auto old = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, color);
    MoveToEx(dc, x, y, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, old);
}
void frame(HDC dc, RECT box, bool pressed = false, bool focused = false)
{
    FillRect(dc, &box, brush(pressed ? kPressed : kControl));
    line(dc, box.left, box.top, box.right, box.top, pressed ? kEdgeDark : kEdgeLight);
    line(dc, box.left, box.top, box.left, box.bottom, pressed ? kEdgeDark : kEdgeLight);
    line(dc, box.left, box.bottom - 1, box.right, box.bottom - 1, pressed ? kEdgeLight : kEdgeDark);
    line(dc, box.right - 1, box.top, box.right - 1, box.bottom, pressed ? kEdgeLight : kEdgeDark);
    if (focused)
    {
        InflateRect(&box, -1, -1);
        FrameRect(dc, &box, brush(kAccent));
    }
}
std::wstring windowText(HWND hwnd)
{
    std::wstring result(GetWindowTextLengthW(hwnd) + 1, L'\0');
    result.resize(GetWindowTextW(hwnd, result.data(), int(result.size())));
    return result;
}
UINT textFlags(HWND hwnd)
{
    return (SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0;
}
void paintButton(HWND hwnd, HDC dc)
{
    RECT box{};
    GetClientRect(hwnd, &box);
    auto font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    auto oldFont = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_TYPEMASK;
    const auto state = SendMessageW(hwnd, BM_GETSTATE, 0, 0);
    const bool pressed = (state & BST_PUSHED) != 0, checked = (state & BST_CHECKED) != 0;
    const bool focus = GetFocus() == hwnd, disabled = !IsWindowEnabled(hwnd);
    const auto text = windowText(hwnd);
    const bool check = style == BS_CHECKBOX || style == BS_AUTOCHECKBOX || style == BS_3STATE || style == BS_AUTO3STATE;
    const bool radio = style == BS_RADIOBUTTON || style == BS_AUTORADIOBUTTON;
    if (check || radio)
    {
        FillRect(dc, &box, brush(kPanel));
        const int side = dpiScale(hwnd, 13), top = (box.bottom - side) / 2;
        RECT mark{0, top, side, top + side};
        frame(dc, mark, true, focus);
        if (checked)
        {
            if (radio)
            {
                auto pen = SelectObject(dc, GetStockObject(NULL_PEN));
                auto fill = SelectObject(dc, brush(kAccent));
                Ellipse(dc, side / 3, top + side / 3, side - side / 3, top + side - side / 3);
                SelectObject(dc, fill);
                SelectObject(dc, pen);
            }
            else
            {
                line(dc, side / 4, top + side / 2, side / 2, top + side * 3 / 4, kAccent);
                line(dc, side / 2, top + side * 3 / 4, side * 4 / 5, top + side / 4, kAccent);
            }
        }
        box.left += side + dpiScale(hwnd, 7);
    }
    else if (style == BS_GROUPBOX)
    {
        FillRect(dc, &box, brush(kPanel));
        RECT border = box;
        border.top += dpiScale(hwnd, 6);
        FrameRect(dc, &border, brush(kEdgeLight));
        box.left += dpiScale(hwnd, 8);
        box.bottom = box.top + dpiScale(hwnd, 16);
    }
    else
        frame(dc, box, pressed, focus || style == BS_DEFPUSHBUTTON);
    SetTextColor(dc, disabled ? kRulerText : (focus || pressed || GetPropW(hwnd, kHover) ? kAccent : kText));
    DrawTextW(dc, text.c_str(), int(text.size()), &box,
              DT_SINGLELINE | DT_VCENTER | (check || radio || style == BS_GROUPBOX ? DT_LEFT : DT_CENTER) |
                  textFlags(hwnd));
    SelectObject(dc, oldFont);
}
void paintCombo(HWND hwnd, HDC dc)
{
    RECT box{};
    GetClientRect(hwnd, &box);
    frame(dc, box, false, GetFocus() == hwnd);
    RECT text = box;
    text.left += dpiScale(hwnd, 6);
    text.right -= dpiScale(hwnd, 22);
    auto font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    auto old = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, IsWindowEnabled(hwnd) ? kText : kRulerText);
    const auto label = windowText(hwnd);
    DrawTextW(dc, label.c_str(), int(label.size()), &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    const int x = box.right - dpiScale(hwnd, 12), y = box.bottom / 2;
    line(dc, x - 3, y - 1, x, y + 2, kMutedText);
    line(dc, x, y + 2, x + 4, y - 2, kMutedText);
    SelectObject(dc, old);
}
LRESULT CALLBACK controlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR kind)
{
    if (message == WM_PAINT || message == WM_PRINTCLIENT)
    {
        PAINTSTRUCT paint{};
        HDC dc = message == WM_PAINT ? BeginPaint(hwnd, &paint) : reinterpret_cast<HDC>(wParam);
        if (kind == 1)
            paintButton(hwnd, dc);
        else
            paintCombo(hwnd, dc);
        if (message == WM_PAINT)
            EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND)
        return 1;
    if (message == WM_MOUSEMOVE && !GetPropW(hwnd, kHover))
    {
        SetPropW(hwnd, kHover, reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (message == WM_MOUSELEAVE)
    {
        RemovePropW(hwnd, kHover);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (message == WM_NCDESTROY)
    {
        RemovePropW(hwnd, kHover);
        RemovePropW(hwnd, kOwnCombo);
        RemovePropW(hwnd, kStyled);
        RemoveWindowSubclass(hwnd, controlProc, kThemeSubclass);
    }
    const auto result = DefSubclassProc(hwnd, message, wParam, lParam);
    if (message == BM_SETCHECK || message == BM_SETSTATE || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
        message == WM_ENABLE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_KEYDOWN ||
        message == WM_KEYUP)
        InvalidateRect(hwnd, nullptr, FALSE);
    return result;
}
void styleControl(HWND hwnd)
{
    if (GetPropW(hwnd, kStyled))
        return;
    SetPropW(hwnd, kStyled, reinterpret_cast<HANDLE>(1));
    if (g_allowDarkWindow)
        g_allowDarkWindow(hwnd, true);
    wchar_t name[64]{};
    GetClassNameW(hwnd, name, 64);
    SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
    if (!_wcsicmp(name, L"Button"))
    {
        const auto type = GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_TYPEMASK;
        if (type != BS_OWNERDRAW && !(GetWindowLongPtrW(hwnd, GWL_STYLE) & (BS_ICON | BS_BITMAP)))
            SetWindowSubclass(hwnd, controlProc, kThemeSubclass, 1);
    }
    else if (!_wcsicmp(name, L"ComboBox") && (GetWindowLongPtrW(hwnd, GWL_STYLE) & 3) == CBS_DROPDOWNLIST)
    {
        SetWindowSubclass(hwnd, controlProc, kThemeSubclass, 2);
        if (GetDlgCtrlID(hwnd) >= 200 && GetDlgCtrlID(hwnd) <= 300)
            SetPropW(hwnd, kOwnCombo, reinterpret_cast<HANDLE>(1));
        COMBOBOXINFO info{sizeof(info)};
        if (GetComboBoxInfo(hwnd, &info) && info.hwndList)
            SetWindowTheme(info.hwndList, L"DarkMode_Explorer", nullptr);
    }
    else if (!_wcsicmp(name, WC_LISTVIEWW))
    {
        ListView_SetBkColor(hwnd, kPressed);
        ListView_SetTextBkColor(hwnd, kPressed);
        ListView_SetTextColor(hwnd, kText);
    }
    else if (!_wcsicmp(name, WC_TREEVIEWW))
    {
        TreeView_SetBkColor(hwnd, kPressed);
        TreeView_SetTextColor(hwnd, kText);
    }
}
BOOL CALLBACK styleChild(HWND hwnd, LPARAM)
{
    styleControl(hwnd);
    return TRUE;
}
LRESULT CALLBACK dialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_INITDIALOG)
    {
        const auto result = DefSubclassProc(hwnd, message, wParam, lParam);
        applyWindowTheme(hwnd);
        EnumChildWindows(hwnd, styleChild, 0);
        return result;
    }
    if (message == WM_PARENTNOTIFY && LOWORD(wParam) == WM_CREATE)
        PostMessageW(hwnd, WM_APP + 89, 0, 0);
    if (message == WM_APP + 89)
    {
        EnumChildWindows(hwnd, styleChild, 0);
        return 0;
    }
    if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC)
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const bool input = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
        SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lParam)) ? kText : kRulerText);
        SetBkColor(dc, input ? kPressed : kPanel);
        SetBkMode(dc, input ? OPAQUE : TRANSPARENT);
        return reinterpret_cast<LRESULT>(brush(input ? kPressed : kPanel));
    }
    if (message == WM_ERASEBKGND)
    {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, brush(kPanel));
        return 1;
    }
    if (message == WM_DRAWITEM)
    {
        const auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
        if (item && item->CtlType == ODT_COMBOBOX && GetPropW(item->hwndItem, kOwnCombo))
        {
            const bool selected = (item->itemState & ODS_SELECTED) != 0;
            FillRect(item->hDC, &item->rcItem, brush(selected ? kPressed : kControl));
            if (item->itemID != UINT(-1))
            {
                const auto length = SendMessageW(item->hwndItem, CB_GETLBTEXTLEN, item->itemID, 0);
                if (length < 0 || length > 32768)
                    return TRUE;
                std::wstring label(std::size_t(length) + 1, L'\0');
                SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, reinterpret_cast<LPARAM>(label.data()));
                auto box = item->rcItem;
                box.left += dpiScale(hwnd, 6);
                SetTextColor(item->hDC, selected ? kAccent : kText);
                SetBkMode(item->hDC, TRANSPARENT);
                DrawTextW(item->hDC, label.c_str(), -1, &box, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            }
            return TRUE;
        }
    }
    if (message == WM_MEASUREITEM)
    {
        auto *item = reinterpret_cast<MEASUREITEMSTRUCT *>(lParam);
        if (item && item->CtlType == ODT_COMBOBOX && item->CtlID >= 200 && item->CtlID <= 300)
        {
            item->itemHeight = dpiScale(hwnd, 20);
            return TRUE;
        }
    }
    if (message == WM_NCDESTROY)
    {
        RemovePropW(hwnd, kStyled);
        RemoveWindowSubclass(hwnd, dialogProc, kThemeSubclass);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}
LRESULT CALLBACK themeHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HCBT_CREATEWND || code == HCBT_ACTIVATE)
    {
        HWND hwnd = reinterpret_cast<HWND>(wParam);
        wchar_t name[64]{};
        GetClassNameW(hwnd, name, 64);
        if (!_wcsicmp(name, L"#32770"))
        {
            if (!GetPropW(hwnd, kStyled))
            {
                SetPropW(hwnd, kStyled, reinterpret_cast<HANDLE>(1));
                if (g_allowDarkWindow)
                    g_allowDarkWindow(hwnd, true);
                SetWindowSubclass(hwnd, dialogProc, kThemeSubclass, 0);
            }
            if (code == HCBT_ACTIVATE)
            {
                applyWindowTheme(hwnd);
                EnumChildWindows(hwnd, styleChild, 0);
            }
        }
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}
bool ownMenuItem(ULONG_PTR value)
{
    return std::any_of(g_menuItems.begin(), g_menuItems.end(),
                       [value](const auto &item) { return reinterpret_cast<ULONG_PTR>(item.get()) == value; });
}
void drawMenuItem(HWND hwnd, HDC dc, RECT box, const MenuItem &item, UINT state)
{
    const bool selected = (state & (ODS_SELECTED | ODS_HOTLIGHT)) != 0,
               disabled = (state & (ODS_DISABLED | ODS_GRAYED)) != 0;
    FillRect(dc, &box, brush(selected ? kPressed : kPanel));
    if (item.separator)
    {
        const int y = (box.top + box.bottom) / 2;
        line(dc, box.left + dpiScale(hwnd, 8), y, box.right - dpiScale(hwnd, 8), y, kEdgeLight);
        return;
    }
    auto old = SelectObject(dc, menuFont(hwnd));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? kRulerText : selected ? kAccent : kText);
    const auto tab = item.text.find(L'\t');
    const auto label = item.text.substr(0, tab), shortcut = tab == std::wstring::npos ? L"" : item.text.substr(tab + 1);
    auto text = box;
    text.left += dpiScale(hwnd, item.bar ? 8 : 28);
    text.right -= dpiScale(hwnd, 12);
    DrawTextW(dc, label.c_str(), int(label.size()), &text,
              DT_SINGLELINE | DT_VCENTER | ((state & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
    if (!shortcut.empty())
        DrawTextW(dc, shortcut.c_str(), int(shortcut.size()), &text,
                  DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    if ((state & ODS_CHECKED) && !item.bar)
    {
        const int x = box.left + dpiScale(hwnd, 9), y = (box.top + box.bottom) / 2;
        line(dc, x, y, x + 3, y + 3, kAccent);
        line(dc, x + 3, y + 3, x + 9, y - 4, kAccent);
    }
    SelectObject(dc, old);
}
void paintMenuBar(HWND hwnd)
{
    HMENU menu = GetMenu(hwnd);
    if (!menu)
        return;
    MENUBARINFO info{sizeof(info)};
    if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &info))
        return;
    RECT window{};
    GetWindowRect(hwnd, &window);
    RECT bar = info.rcBar;
    OffsetRect(&bar, -window.left, -window.top);
    HDC dc = GetWindowDC(hwnd);
    if (!dc)
        return;
    FillRect(dc, &bar, brush(kPanel));
    for (int i = 0; i < GetMenuItemCount(menu); ++i)
    {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_DATA | MIIM_STATE;
        RECT rect{};
        if (GetMenuItemInfoW(menu, i, TRUE, &item) && ownMenuItem(item.dwItemData) &&
            GetMenuItemRect(hwnd, menu, i, &rect))
        {
            OffsetRect(&rect, -window.left, -window.top);
            drawMenuItem(hwnd, dc, rect, *reinterpret_cast<MenuItem *>(item.dwItemData),
                         (item.fState & MFS_HILITE) ? ODS_SELECTED : 0);
        }
    }
    ReleaseDC(hwnd, dc);
}
void styleMenu(HMENU menu, bool bar)
{
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = brush(kPanel);
    SetMenuInfo(menu, &info);
    for (int i = 0; i < GetMenuItemCount(menu); ++i)
    {
        wchar_t label[256]{};
        MENUITEMINFOW current{sizeof(current)};
        current.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_SUBMENU;
        current.dwTypeData = label;
        current.cch = 256;
        GetMenuItemInfoW(menu, i, TRUE, &current);
        auto item = std::make_unique<MenuItem>();
        item->text = label;
        item->bar = bar;
        item->separator = (current.fType & MFT_SEPARATOR) != 0;
        MENUITEMINFOW changed{sizeof(changed)};
        changed.fMask = MIIM_FTYPE | MIIM_DATA;
        changed.fType = current.fType | MFT_OWNERDRAW;
        changed.dwItemData = reinterpret_cast<ULONG_PTR>(item.get());
        SetMenuItemInfoW(menu, i, TRUE, &changed);
        g_menuItems.push_back(std::move(item));
        if (current.hSubMenu)
            styleMenu(current.hSubMenu, false);
    }
}
} // namespace
void initializeNativeTheme()
{
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    // Optional process-local dark opt-in used by Windows' own shell dialogs.
    // These exports are not a stable public API: gate the post-1903 signature,
    // check each export, and keep all application drawing independent of them.
    using VersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    auto versionFn = reinterpret_cast<VersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW version{sizeof(version)};
    if (versionFn && versionFn(&version) == 0 && version.dwMajorVersion == 10 && version.dwBuildNumber >= 18362)
    {
        HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
        using PreferredFn = int(WINAPI *)(int);
        if (auto preferred = reinterpret_cast<PreferredFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135))))
            preferred(2);
        g_allowDarkWindow = reinterpret_cast<AllowDarkWindow>(GetProcAddress(ux, MAKEINTRESOURCEA(133)));
        using FlushFn = void(WINAPI *)();
        if (auto flush = reinterpret_cast<FlushFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136))))
            flush();
    }
    g_hook = SetWindowsHookExW(WH_CBT, themeHook, nullptr, GetCurrentThreadId());
}
void shutdownNativeTheme()
{
    if (g_hook)
        UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    for (auto [color, value] : g_brushes)
        DeleteObject(value);
    for (auto [dpi, font] : g_menuFonts)
        if (font)
            DeleteObject(font);
    g_brushes.clear();
    g_menuFonts.clear();
    g_menuItems.clear();
}
void themeNativeMenu(HMENU menu)
{
    styleMenu(menu, true);
}
bool handleNativeThemeMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT &result)
{
    if (message == WM_MEASUREITEM)
    {
        auto *value = reinterpret_cast<MEASUREITEMSTRUCT *>(lParam);
        if (!value || value->CtlType != ODT_MENU || !ownMenuItem(value->itemData))
            return false;
        const auto &item = *reinterpret_cast<MenuItem *>(value->itemData);
        HDC dc = GetDC(hwnd);
        auto old = SelectObject(dc, menuFont(hwnd));
        SIZE size{};
        GetTextExtentPoint32W(dc, item.text.c_str(), int(item.text.size()), &size);
        SelectObject(dc, old);
        ReleaseDC(hwnd, dc);
        value->itemWidth = size.cx + dpiScale(hwnd, item.bar ? 12 : 52);
        value->itemHeight = item.separator ? dpiScale(hwnd, 8) : size.cy + dpiScale(hwnd, item.bar ? 6 : 12);
        result = TRUE;
        return true;
    }
    if (message == WM_DRAWITEM)
    {
        const auto *value = reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
        if (!value || value->CtlType != ODT_MENU || !ownMenuItem(value->itemData))
            return false;
        drawMenuItem(hwnd, value->hDC, value->rcItem, *reinterpret_cast<MenuItem *>(value->itemData), value->itemState);
        result = TRUE;
        return true;
    }
    if (message == WM_MENUCHAR)
    {
        HMENU menu = reinterpret_cast<HMENU>(lParam);
        int selected = -1;
        for (int i = 0; i < GetMenuItemCount(menu); ++i)
        {
            MENUITEMINFOW value{sizeof(value)};
            value.fMask = MIIM_DATA | MIIM_STATE;
            if (!GetMenuItemInfoW(menu, i, TRUE, &value) || !ownMenuItem(value.dwItemData) ||
                (value.fState & MFS_DISABLED))
                continue;
            const auto &text = reinterpret_cast<MenuItem *>(value.dwItemData)->text;
            const auto marker = text.find(L'&');
            const auto key = marker != std::wstring::npos && marker + 1 < text.size()
                                 ? text[marker + 1]
                                 : (text.empty() ? L'\0' : text.front());
            if (std::towupper(key) == std::towupper(LOWORD(wParam)))
            {
                selected = i;
                break;
            }
        }
        result = MAKELRESULT(selected < 0 ? 0 : selected, selected < 0 ? MNC_IGNORE : MNC_EXECUTE);
        return true;
    }
    if (message == WM_NCPAINT || message == WM_NCACTIVATE)
    {
        result = DefWindowProcW(hwnd, message, wParam, lParam);
        paintMenuBar(hwnd);
        return true;
    }
    return false;
}
