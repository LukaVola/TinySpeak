#pragma once

#include <algorithm>

struct SliderScale {
    int maximum;
    bool fivePercentSteps = false;

    constexpr int trackMaximum() const {
        return position(maximum);
    }

    constexpr int position(int value) const {
        value = std::clamp(value, 0, maximum);
        if (!fivePercentSteps || value <= 5) return value;
        return 5 + (value - 5 + 2) / 5;
    }

    constexpr int value(int position) const {
        position = std::clamp(position, 0, trackMaximum());
        if (!fivePercentSteps || position <= 5) return position;
        return 5 + (position - 5) * 5;
    }
};
