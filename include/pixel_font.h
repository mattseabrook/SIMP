#ifndef SIMP_PIXEL_FONT_H
#define SIMP_PIXEL_FONT_H
#include <array>
#include <cstdint>
// Original 5 x 7 UI alphabet. Rows are emitted as horizontal GPU rectangles.
inline std::array<std::uint8_t,7> pixelGlyph(wchar_t c)
{
    if(c >= L'a' && c <= L'z') c -= L'a'-L'A';
    switch(c)
    {
    case L'A': return {14,17,17,31,17,17,17};
    case L'B': return {30,17,17,30,17,17,30};
    case L'C': return {14,17,16,16,16,17,14};
    case L'D': return {30,17,17,17,17,17,30};
    case L'E': return {31,16,16,30,16,16,31};
    case L'F': return {31,16,16,30,16,16,16};
    case L'G': return {14,17,16,23,17,17,15};
    case L'H': return {17,17,17,31,17,17,17};
    case L'I': return {14,4,4,4,4,4,14};
    case L'J': return {7,2,2,2,18,18,12};
    case L'K': return {17,18,20,24,20,18,17};
    case L'L': return {16,16,16,16,16,16,31};
    case L'M': return {17,27,21,21,17,17,17};
    case L'N': return {17,25,25,21,19,19,17};
    case L'O': return {14,17,17,17,17,17,14};
    case L'P': return {30,17,17,30,16,16,16};
    case L'Q': return {14,17,17,17,21,18,13};
    case L'R': return {30,17,17,30,20,18,17};
    case L'S': return {15,16,16,14,1,1,30};
    case L'T': return {31,4,4,4,4,4,4};
    case L'U': return {17,17,17,17,17,17,14};
    case L'V': return {17,17,17,17,17,10,4};
    case L'W': return {17,17,17,21,21,27,17};
    case L'X': return {17,17,10,4,10,17,17};
    case L'Y': return {17,17,10,4,4,4,4};
    case L'Z': return {31,1,2,4,8,16,31};
    case L'0': return {14,17,19,21,25,17,14};
    case L'1': return {4,12,4,4,4,4,14};
    case L'2': return {14,17,1,2,4,8,31};
    case L'3': return {30,1,1,14,1,1,30};
    case L'4': return {2,6,10,18,31,2,2};
    case L'5': return {31,16,16,30,1,1,30};
    case L'6': return {14,16,16,30,17,17,14};
    case L'7': return {31,1,2,4,8,8,8};
    case L'8': return {14,17,17,14,17,17,14};
    case L'9': return {14,17,17,15,1,1,14};
    case L' ': return {};
    case L'.': return {0,0,0,0,0,6,6};
    case L',': return {0,0,0,0,6,6,4};
    case L':': return {0,6,6,0,6,6,0};
    case L'-': return {0,0,0,31,0,0,0};
    case L'+': return {0,4,4,31,4,4,0};
    case L'/': return {1,2,2,4,8,8,16};
    case L'|': return {4,4,4,4,4,4,4};
    case L'[': return {14,8,8,8,8,8,14};
    case L']': return {14,2,2,2,2,2,14};
    case L'(': return {2,4,8,8,8,4,2};
    case L')': return {8,4,2,2,2,4,8};
    case L'#': return {10,10,31,10,31,10,10};
    case L'%': return {25,25,2,4,8,19,19};
    case L'_': return {0,0,0,0,0,0,31};
    default: return {14,17,1,2,4,0,4};
    }
}
#endif
