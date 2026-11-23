#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

inline constexpr int microphoneThresholdMax = 70;
inline constexpr int microphoneThresholdDefault = 30;

inline std::uint64_t microphoneGatePower(int threshold, std::size_t sampleCount) {
    if (threshold <= 0) return 0;
    // The slider counts dB above -70 dBFS. Power is summed across the frame.
    constexpr double fullScale = 32768.0;
    const int dbfs = std::min(threshold, microphoneThresholdMax) - microphoneThresholdMax;
    return static_cast<std::uint64_t>(std::ceil(fullScale * fullScale * sampleCount * std::pow(10.0, dbfs / 10.0)));
}

struct MicrophoneFrame {
    int peakPercent;
    bool transmitting;
};

inline MicrophoneFrame gateMicrophoneFrame(std::span<std::int16_t> samples,
    int gain, std::uint64_t minimumPower, bool microphoneAllowed) {
    int peak = 0;
    std::uint64_t power = 0;
    for (auto& sample : samples) {
        const auto adjusted = std::clamp(static_cast<int>(sample) * gain / 100, -32768, 32767);
        sample = static_cast<std::int16_t>(adjusted);
        peak = std::max(peak, std::abs(adjusted));
        power += static_cast<std::uint64_t>(adjusted * adjusted);
    }
    const bool allowed = microphoneAllowed && power >= minimumPower;
    if (!allowed) std::fill(samples.begin(), samples.end(), 0);
    return {peak * 100 / 32768, allowed};
}
