#pragma once

#include "network.h"

#include <array>
#include <cstdint>
#include <optional>

struct ChatPresence {
    bool joined = false;
    bool left = false;
};

ChatPresence chatPresence(const Json& previous, const Json& next);
ChatPresence voicePresence(const Json& previous, const Json& next);

enum class ChatCue { Join, Leave };

class ChatSounds {
public:
    ~ChatSounds();
    void notify(ChatCue cue, int volume);
    void tick(int volume);
    void playExit(int volume);

private:
    static constexpr unsigned sampleRate = 16000;
    static constexpr unsigned sampleCount = sampleRate * 72 / 1000;
    std::array<std::uint16_t, 22 + sampleCount> wave{};
    std::optional<ChatCue> pending;
    ULONGLONG readyAt = 0;

    void play(ChatCue cue, int volume, bool asynchronous);
};
