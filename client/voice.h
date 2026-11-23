#pragma once
#include "voice_gate.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

inline constexpr std::array voiceBitrates{64, 48, 32, 24};
inline constexpr int defaultVoiceBitrate = 32;

inline constexpr int validatedVoiceBitrate(int requested) {
    for (int bitrate : voiceBitrates) {
        if (requested == bitrate) return requested;
    }
    return defaultVoiceBitrate;
}

struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

struct VoiceRelay {
    std::string address;
    std::string user, password;
};

class Voice {
public:
    using Volumes = std::unordered_map<std::string, int>;

    Voice();
    ~Voice();
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;

    std::pair<std::vector<AudioDevice>, std::vector<AudioDevice>> devices() const;
    void connect(std::string url, std::string token, std::wstring input, std::wstring output, VoiceRelay relay = {});
    void disconnect();
    void setParticipantVolumes(Volumes volumes);
    // 0 = activation by voice, 1 = push to talk, 2 = open microphone.
    std::atomic_int inputGain{100}, outputGain{100}, mode{0}, level{0}, threshold{microphoneThresholdDefault};
    std::atomic_int bitrateKbps{defaultVoiceBitrate};
    std::atomic_bool muted{true}, push{false}, talking{false}, permitted{false};
    std::atomic_int status{0}; // 0 idle, 1 connecting, 2 connected, 3 audio failure, 4 listening only

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
