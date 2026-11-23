#pragma once

#include <algorithm>

struct SliderScale {
    int maximum;
    bool fivePercentSteps = false;
    int minimum = 0;

    constexpr int trackMaximum() const {
        return position(maximum);
    }

    constexpr int position(int value) const {
        value = std::clamp(value, minimum, maximum);
        if (!fivePercentSteps) return value - minimum;
        if (value <= 5) return value;
        return 5 + (value - 5 + 2) / 5;
    }

    constexpr int value(int position) const {
        position = std::clamp(position, 0, trackMaximum());
        if (!fivePercentSteps) return minimum + position;
        if (position <= 5) return position;
        return 5 + (position - 5) * 5;
    }
};
