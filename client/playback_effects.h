#pragma once

#include "voice_mix.h"
#include "miniaudio.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <span>

// The device renders mono at 48 kHz in blocks of at most 10 ms. Work on the
// wide mix before its single conversion to int16, including at gains above 100%.
class PlaybackEffects {
public:
    PlaybackEffects() {
        const auto flat = ma_biquad_config_init(ma_format_f32, 1, 1, 0, 0, 1, 0, 0);
        for (auto* band : bands_) ma_biquad_init(&flat, nullptr, band);
    }
    ~PlaybackEffects() {
        for (auto* band : bands_) ma_biquad_uninit(band, nullptr);
    }
    PlaybackEffects(const PlaybackEffects&) = delete;
    PlaybackEffects& operator=(const PlaybackEffects&) = delete;

    void render(std::span<const int> mix, std::int16_t* output, int volume,
        EqualizerGains target, int normalizationPercent) {
        assert(mix.size() <= voiceBlockFrames);
        if (mix.empty()) return;
        if (applied_ != target) updateBands(target);
        const int strength = normalizationLevel(normalizationPercent);
        if (!strength && normalized_) limiter_ = {};
        normalized_ = strength != 0;
        if (applied_ == EqualizerGains{} && !strength) {
            for (std::size_t i = 0; i < mix.size(); ++i) {
                output[i] = static_cast<std::int16_t>(std::clamp(mix[i] * volume / 100, -32768, 32767));
            }
            return;
        }

        for (std::size_t i = 0; i < mix.size(); ++i) work_[i] = static_cast<float>(mix[i]);
        for (std::size_t i = 0; i < bands_.size(); ++i) {
            auto* band = bands_[i];
            if (!applied_[i] || !band->pR1) continue;
            ma_biquad_process_pcm_frames(band, work_.data(), work_.data(), mix.size());
            // Silence may last minutes in a call. Flush inaudible filter tails
            // before they decay into expensive subnormal floats on older CPUs.
            if (std::abs(band->pR1[0].f32) < 1e-12f && std::abs(band->pR2[0].f32) < 1e-12f) {
                ma_biquad_clear_cache(band);
            }
        }
        if (strength) limiter_.process({work_.data(), mix.size()}, strength);
        const float master = static_cast<float>(volume) / 100;
        for (std::size_t i = 0; i < mix.size(); ++i) {
            output[i] = static_cast<std::int16_t>(std::clamp(work_[i] * master, -32768.f, 32767.f));
        }
    }

private:
    struct PeakLimiter {
        float speechPeak = 4096.f;
        float gain = 1.f;

        void process(std::span<float> samples, int strength) {
            // Allow about 18, 14, 10 or 6 dB above recent speech peaks.
            // The strongest level keeps the original limiter's -3 dBFS ceiling.
            constexpr std::array headroom{8.f, 5.f, 3.16f, 2.f};
            constexpr std::array absolutePeak{32767.f, 30935.f, 27570.f, 23200.f};
            const float ceiling = std::min(absolutePeak[strength - 1], headroom[strength - 1] * speechPeak);
            float blockPeak = 0;
            for (auto& sample : samples) {
                const float peak = std::abs(sample);
                blockPeak = std::max(blockPeak, peak);
                gain += (1.f - gain) * (1.f / (voiceSampleRate * 0.080f));
                if (peak * gain > ceiling) gain = ceiling / peak;
                sample *= gain;
            }
            // Learn sustained changes over time. A single microphone knock
            // cannot raise the reference for the rest of the conversation.
            const float observed = std::clamp(blockPeak, 512.f, 4.f * speechPeak);
            const float time = observed > speechPeak ? 0.35f : 0.8f;
            const float frames = static_cast<float>(samples.size());
            speechPeak += (observed - speechPeak) * frames / (time * voiceSampleRate + frames);
        }
    };

    ma_loshelf2 bass_{};
    ma_peak2 lowMid_{}, highMid_{};
    ma_hishelf2 treble_{};
    const std::array<ma_biquad*, 4> bands_{&bass_.bq, &lowMid_.bq, &highMid_.bq, &treble_.bq};
    EqualizerGains applied_{};
    std::array<float, voiceBlockFrames> work_{};
    PeakLimiter limiter_{};
    bool normalized_ = false;

    void updateBands(const EqualizerGains& target) {
        for (std::size_t i = 0; i < bands_.size(); ++i) {
            const int delta = static_cast<int>(target[i]) - applied_[i];
            if (!delta) continue;
            // Slew large slider changes one dB per block using existing filter memory.
            applied_[i] += static_cast<std::int8_t>(std::clamp(delta, -1, 1));
            const int db = applied_[i];
            if (!bands_[i]->pR1) continue;
            if (!db) {
                ma_biquad_clear_cache(bands_[i]);
            } else if (i == 0) {
                const auto config = ma_loshelf2_config_init(ma_format_f32, 1, voiceSampleRate, db, 1, 200);
                ma_loshelf2_reinit(&config, &bass_);
            } else if (i == 3) {
                const auto config = ma_hishelf2_config_init(ma_format_f32, 1, voiceSampleRate, db, 1, 1000);
                ma_hishelf2_reinit(&config, &treble_);
            } else {
                const double low = i == 1 ? 200.0 : 400.0;
                const double high = i == 1 ? 400.0 : 1000.0;
                const double center = std::sqrt(low * high);
                const auto config = ma_peak2_config_init(ma_format_f32, 1, voiceSampleRate, db, center / (high - low), center);
                ma_peak2_reinit(&config, i == 1 ? &lowMid_ : &highMid_);
            }
        }
    }
};
