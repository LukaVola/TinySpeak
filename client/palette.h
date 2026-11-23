#pragma once

#include <windows.h>
#include <array>

namespace theme {
// 20 neutral tones and 12 accents form the application palette.
inline constexpr unsigned neutralSteps = 20;
constexpr COLORREF gray(unsigned step) {
    const unsigned level = (step * 255 + (neutralSteps - 1) / 2) / (neutralSteps - 1);
    return RGB(level, level, level);
}

inline constexpr auto background = gray(3);
inline constexpr auto sidebar = gray(2);
inline constexpr auto field = gray(4);
inline constexpr auto edge = gray(6);
inline constexpr auto hover = gray(5);
inline constexpr auto selected = gray(7);
inline constexpr auto text = gray(18);
inline constexpr auto secondary = gray(14);
inline constexpr auto muted = gray(11);
inline constexpr auto white = gray(19);
inline constexpr std::array<COLORREF, 16> profile{
    RGB(229, 82, 82), RGB(232, 137, 70), RGB(226, 191, 73), RGB(169, 196, 82),
    RGB(90, 187, 104), RGB(74, 189, 154), RGB(68, 177, 211), RGB(94, 128, 223),
    RGB(142, 108, 215), RGB(186, 101, 213), RGB(222, 110, 170), RGB(162, 115, 83),
    gray(0), gray(6), gray(11), gray(19)
};
}
