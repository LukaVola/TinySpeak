#pragma once

#include <windows.h>
#include <algorithm>

struct Layout {
    RECT brand, server, create, join, menu, serverSettings, listTitle, toggleRooms, splitter, rooms, chatTitle, transcript, composer,
        mute, microphone, input, meter, outputTitle, output, voice, status;
    int toolbarBottom, sidebarRight, footerTop, statusTop;
};

inline constexpr int maxAudioTrackDip = 220;

inline Layout layoutFor(int width, int height, int dpi, int sidebarDip = 0, bool collapsed = false) {
    const auto dip = [dpi](int n) { return MulDiv(n, dpi, 96); };
    const int margin = dip(14), gap = dip(7), button = dip(32);
    const auto box = [](int left, int top, int right, int bottom) {
        return RECT{left, top, std::max(left, right), std::max(top, bottom)};
    };
    const int right = width - margin;
    const int menuLeft = right - dip(94);
    const int settingsRight = menuLeft - gap, settingsLeft = settingsRight - dip(136);
    const int top = dip(7), second = top + button + dip(5);
    const int header = second + button + dip(7);
    const int natural = std::clamp(width * 28 / 100, dip(148), dip(248));
    const int sidebar = collapsed ? dip(34) : std::clamp(sidebarDip > 0 ? dip(sidebarDip) : natural,
        dip(148), std::max(dip(148), width - dip(245)));
    const int workTop = header + dip(44), status = height - dip(26), footer = status - dip(58);
    const int composeBottom = std::max(workTop, footer - dip(12)), composeTop = std::max(workTop, composeBottom - dip(37));
    const int center = width / 2;
    const int firstSlider = margin + dip(154), secondSlider = center + dip(110);
    const int leftSliderEnd = std::min(center - margin, firstSlider + dip(maxAudioTrackDip));
    const int rightSliderEnd = std::min(width - margin, secondSlider + dip(maxAudioTrackDip));
    return {
        box(margin, dip(3), settingsLeft - gap, top + button),
        box(margin, second, settingsLeft - gap, second + button),
        box(settingsLeft, second, settingsRight, second + button),
        box(menuLeft, second, right, second + button),
        box(menuLeft, top, right, top + button),
        box(settingsLeft, top, settingsRight, top + button),
        box(margin, header + dip(6), sidebar - dip(35), workTop),
        box(sidebar - dip(30), header + dip(10), sidebar - dip(5), header + dip(35)),
        box(sidebar - dip(4), header, sidebar + dip(4), footer),
        box(dip(6), workTop, sidebar - dip(7), footer - dip(8)),
        box(sidebar + margin, header + dip(6), width - margin, workTop),
        box(sidebar + margin, workTop + dip(2), width - margin, composeTop - dip(8)),
        box(sidebar + margin, composeTop, width - margin, composeBottom),
        box(margin, footer + dip(10), margin + dip(22), footer + dip(32)),
        box(margin + dip(28), footer + dip(9), firstSlider - gap, footer + dip(36)),
        box(firstSlider, footer + dip(7), leftSliderEnd, footer + dip(35)),
        box(firstSlider + gap, footer + dip(41), leftSliderEnd - gap, footer + dip(44)),
        box(center + margin, footer + dip(9), secondSlider - gap, footer + dip(36)),
        box(secondSlider, footer + dip(7), rightSliderEnd, footer + dip(35)),
        box(center + margin, footer + dip(33), width - margin, status),
        box(margin, status, width - margin, height),
        header, sidebar, footer, status
    };
}
