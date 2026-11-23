#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

inline std::string_view speakerSession(std::string_view identity) {
    return identity.substr(0, identity.find('-'));
}

inline void mixVoiceSamples(int* sum, const std::int16_t* source, std::size_t count, int percent) {
    if (percent == 100) {
        for (std::size_t i = 0; i < count; ++i) sum[i] += source[i];
    } else if (percent > 0) {
        for (std::size_t i = 0; i < count; ++i) sum[i] += source[i] * percent / 100;
    }
}
