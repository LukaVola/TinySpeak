#pragma once

#include <windows.h>
#include <algorithm>

struct Layout {
    RECT brand, server, create, join, menu, listTitle, rooms, chatTitle, exportChat, openChat, transcript, composer,
        mute, microphone, input, meter, outputTitle, output, voice, status;
    int toolbarBottom, sidebarRight, footerTop, statusTop;
};

inline constexpr int maxAudioTrackDip = 220;

inline Layout layoutFor(int width, int height, int dpi) {
    const auto dip = [dpi](int n) { return MulDiv(n, dpi, 96); };
    const int margin = dip(14), gap = dip(7), button = dip(32);
    const auto box = [](int left, int top, int right, int bottom) {
        return RECT{left, top, std::max(left, right), std::max(top, bottom)};
    };
    const bool narrow = width < dip(795);
    const int menuWidth = dip(96), createWidth = dip(100), joinWidth = dip(97);
    const int menuRight = width - margin, menuLeft = menuRight - menuWidth;
    const int joinRight = narrow ? menuRight : menuLeft - gap;
    const int joinLeft = joinRight - joinWidth;
    const int createRight = joinLeft - gap, createLeft = createRight - createWidth;
    const int controlsTop = narrow ? dip(54) : dip(13);
    const int header = narrow ? dip(98) : dip(60);
    const int sidebar = std::clamp(width * 28 / 100, dip(148), dip(248));
    const int logLeft = width - margin - dip(118);
    const int workTop = header + dip(56), status = height - dip(26), footer = status - dip(58);
    const int composeBottom = std::max(workTop, footer - dip(12)), composeTop = std::max(workTop, composeBottom - dip(37));
    const int center = width / 2;
    const int firstSlider = margin + dip(154), secondSlider = center + dip(110);
    const int leftSliderEnd = std::min(center - margin, firstSlider + dip(maxAudioTrackDip));
    const int rightSliderEnd = std::min(width - margin, secondSlider + dip(maxAudioTrackDip));
    return {
        box(margin, dip(5), menuLeft - gap, dip(38)),
        box(narrow ? margin : dip(140), controlsTop, createLeft - gap, controlsTop + button),
        box(createLeft, controlsTop, createRight, controlsTop + button),
        box(joinLeft, controlsTop, joinRight, controlsTop + button),
        box(menuLeft, dip(13), menuRight, dip(13) + button),
        box(margin, header + dip(6), sidebar - margin, workTop),
        box(dip(6), workTop, sidebar - dip(7), footer - dip(8)),
        box(sidebar + margin, header + dip(6), logLeft - gap, workTop),
        box(logLeft, header + dip(3), width - margin, header + dip(26)),
        box(logLeft, header + dip(29), width - margin, header + dip(52)),
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
