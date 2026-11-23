#pragma once
#include <cstdint>

// Display chrome only. Document pixels, swatches and user guide colors stay exact.
namespace EditorTheme
{
constexpr std::uint32_t rgb(unsigned r, unsigned g, unsigned b)
{
    return r | (g << 8) | (b << 16);
}
inline constexpr auto kWorkspace = rgb(37, 41, 45);
inline constexpr auto kPanel = rgb(50, 54, 58);
inline constexpr auto kControl = rgb(56, 60, 64);
inline constexpr auto kPressed = rgb(40, 44, 48);
inline constexpr auto kEdgeLight = rgb(81, 86, 91);
inline constexpr auto kEdgeDark = rgb(35, 39, 43);
inline constexpr auto kText = rgb(179, 184, 190);
inline constexpr auto kMutedText = rgb(157, 167, 175);
inline constexpr auto kAccent = rgb(207, 171, 102);
inline constexpr auto kRuler = rgb(59, 64, 69);
inline constexpr auto kRulerText = rgb(125, 134, 143);
inline constexpr auto kTicks = rgb(133, 142, 151);
inline constexpr auto kStatus = rgb(41, 45, 49);
inline constexpr auto kCaption = rgb(43, 47, 51);
inline constexpr auto kSwatchBorder = rgb(57, 62, 67);
inline constexpr auto kScrollTrack = rgb(32, 36, 40);
inline constexpr auto kScrollThumb = rgb(93, 102, 111);
inline constexpr auto kGridMajor = rgb(113, 128, 149);
inline constexpr auto kGridMinor = rgb(185, 195, 207);
} // namespace EditorTheme
