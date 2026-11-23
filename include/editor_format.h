#pragma once
#include "editor_ui.h"
#include <cwchar>
#include <iterator>
inline std::wstring formatDouble(double value, int precision = 2)
{
    wchar_t buffer[64] = {};
    std::swprintf(buffer, std::size(buffer), precision == 3 ? L"%.3f" : L"%.2f", value);
    return buffer;
}

inline std::wstring formatNumber(double value)
{
    wchar_t buffer[64]{};
    std::swprintf(buffer, 64, L"%.9g", value);
    return buffer;
}

inline std::wstring formatInt(int value)
{
    wchar_t buffer[32] = {};
    std::swprintf(buffer, std::size(buffer), L"%d", value);
    return buffer;
}

inline std::wstring formatHex(const PaletteColor &color)
{
    wchar_t buffer[16] = {};
    std::swprintf(buffer, std::size(buffer), L"#%02X%02X%02X", color.red, color.green, color.blue);
    return buffer;
}
