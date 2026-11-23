#define MA_NO_DEVICE_IO
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "playback_effects.h"
#include "slider_scale.h"

#include <cassert>
#include <cstdio>
#include <numbers>

namespace {
using Mix = std::array<int, voiceBlockFrames>;
using Output = std::array<std::int16_t, voiceBlockFrames>;

double responseDb(EqualizerGains gains, double frequency) {
    PlaybackEffects effects;
    Mix mix{};
    Output out{};
    double inputPower = 0, outputPower = 0;
    for (int block = 0; block < 80; ++block) {
        for (int i = 0; i < voiceBlockFrames; ++i) {
            const double t = (block * voiceBlockFrames + i) / double(voiceSampleRate);
            mix[i] = static_cast<int>(3600 * std::sin(2 * std::numbers::pi * frequency * t));
        }
        effects.render(mix, out.data(), 100, gains, false);
        if (block < 40) continue;
        for (int i = 0; i < voiceBlockFrames; ++i) {
            inputPower += double(mix[i]) * mix[i];
            outputPower += double(out[i]) * out[i];
        }
    }
    return 10 * std::log10(outputPower / inputPower);
}
}

int main() {
    const SliderScale eqScale{equalizerLimitDb, false, -equalizerLimitDb};
    for (int db = -equalizerLimitDb; db <= equalizerLimitDb; ++db) assert(eqScale.value(eqScale.position(db)) == db);
    for (bool stepped : {false, true}) {
        const SliderScale participant{maxParticipantGain, stepped};
        assert(participant.value(participant.trackMaximum()) == 150);
        assert(participant.value(participant.position(135)) == 135);
    }
    Mix mix{};
    Output samples{}, out{};
    for (int i = 0; i < voiceBlockFrames; ++i) samples[i] = static_cast<std::int16_t>(i * 131 - 32000);
    mixVoiceSamples(mix.data(), samples.data(), mix.size(), 150);
    mixVoiceSamples(mix.data(), samples.data(), mix.size(), 0);
    PlaybackEffects effects;
    for (int volume : {0, 3, 100, 110}) {
        effects.render(mix, out.data(), volume, {}, false);
        for (int i = 0; i < voiceBlockFrames; ++i) {
            assert(mix[i] == samples[i] * 150 / 100);
            assert(out[i] == std::clamp(mix[i] * volume / 100, -32768, 32767));
        }
    }
    constexpr std::array frequencies{60., 283., 633., 5000.};
    for (int band = 0; band < 4; ++band) {
        for (int db : {-12, -6, 6, 12}) {
            EqualizerGains gains{};
            gains[band] = static_cast<std::int8_t>(db);
            const double measured = responseDb(gains, frequencies[band]);
            assert(std::abs(measured - db) < 0.4);
            assert(std::abs(responseDb(gains, frequencies[band < 2 ? 3 : 0])) < 0.55);
        }
    }

    // A repeatable voice-level tone, followed by one first-sample microphone knock.
    for (int i = 0; i < voiceBlockFrames; ++i) mix[i] = static_cast<int>(1000 * std::cos(2 * std::numbers::pi * i / 48));
    for (int n = 0; n < 500; ++n) effects.render(mix, out.data(), 100, {}, true);
    assert(out[0] == 1000);
    for (int incoming : {32000, -32000}) {
        mix[0] = incoming;
        effects.render(mix, out.data(), 100, {}, true);
        std::printf("Voice peak 1000, incoming knock %d, protected peak %d\n", incoming, out[0]);
        assert(std::abs(out[0]) <= 2150 && std::abs(out[0]) >= 1900);
        mix[0] = 1000;
        for (int n = 0; n < 60; ++n) effects.render(mix, out.data(), 100, {}, true);
        assert(out[0] >= 995);
    }
    for (int i = 0; i < voiceBlockFrames; ++i) mix[i] *= 700;
    for (int n = 0; n < 200; ++n) {
        effects.render(mix, out.data(), 110, {12, -12, 12, 12}, true);
        for (auto sample : out) assert(std::abs(sample) <= 25521);
    }
    effects.render(mix, out.data(), 0, {12, -12, 12, 12}, true);
    for (auto sample : out) assert(sample == 0);
    for (int n = 0; n < 12; ++n) effects.render(mix, out.data(), 100, {}, false);
    for (int i = 0; i < voiceBlockFrames; ++i) assert(out[i] == std::clamp(mix[i], -32768, 32767));
    std::puts("Passed participant gain, bypass, four EQ bands, first-sample limiting, recovery and mute.");
}
