#include "chat_sounds.h"

#include <mmsystem.h>
#include <cmath>
#include <numbers>
#include <string_view>
#include <unordered_set>

namespace {
std::unordered_set<std::string_view> members(const Json& users, const std::string& room = {}) {
    std::unordered_set<std::string_view> ids;
    ids.reserve(users.size());
    for (const auto& user : users) {
        if (!room.empty() && user.value("voice", "") != room) continue;
        const auto id = user.find("id");
        if (id != user.end() && id->is_string()) ids.emplace(id->get_ref<const std::string&>());
    }
    return ids;
}

ChatPresence rosterChange(const Json& previous, const Json& next, const std::string& room = {}) {
    const auto previousUsers = previous.find("users");
    const auto nextUsers = next.find("users");
    if (previousUsers == previous.end() || nextUsers == next.end() || *previousUsers == *nextUsers) return {};
    const auto previousIds = members(*previousUsers, room);
    const auto nextIds = members(*nextUsers, room);
    ChatPresence result;
    for (auto id : previousIds) result.left |= !nextIds.contains(id);
    for (auto id : nextIds) result.joined |= !previousIds.contains(id);
    return result;
}
}

ChatPresence chatPresence(const Json& previous, const Json& next) {
    const auto before = previous.value("code", "");
    const auto after = next.value("code", "");
    if (before != after) return {!after.empty(), !before.empty()};
    return after.empty() ? ChatPresence{} : rosterChange(previous, next);
}

ChatPresence voicePresence(const Json& previous, const Json& next) {
    const auto before = previous.value("voice", "");
    const auto after = next.value("voice", "");
    if (before != after || previous.value("code", "") != next.value("code", "")) {
        return {!after.empty(), !before.empty()};
    }
    return after.empty() ? ChatPresence{} : rosterChange(previous, next, after);
}

ChatSounds::~ChatSounds() {
    if (readyAt) PlaySoundW(nullptr, nullptr, 0);
}

void ChatSounds::notify(ChatCue cue, int volume) {
    tick(volume);
    pending = cue;
    tick(volume);
}

void ChatSounds::tick(int volume) {
    if (volume <= 0) {
        pending.reset();
        return;
    }
    if (!pending || GetTickCount64() < readyAt) return;
    const auto cue = *pending;
    pending.reset();
    play(cue, volume, true);
}

void ChatSounds::playExit(int volume) {
    pending.reset();
    if (volume > 0) play(ChatCue::Leave, volume, false);
}

void ChatSounds::play(ChatCue cue, int volume, bool asynchronous) {
    if (readyAt) PlaySoundW(nullptr, nullptr, 0);
    constexpr unsigned dataBytes = sampleCount * sizeof(std::uint16_t);
    constexpr std::array<std::uint16_t, 22> header{
        0x4952, 0x4646, 36 + dataBytes, 0, 0x4157, 0x4556, // RIFF / WAVE
        0x6d66, 0x2074, 16, 0, 1, 1, sampleRate, 0, sampleRate * 2, 0, 2, 16, // Mono PCM
        0x6164, 0x6174, dataBytes, 0 // data
    };
    for (std::size_t i = 0; i < header.size(); ++i) wave[i] = header[i];

    const bool rising = cue == ChatCue::Join;
    const double start = rising ? 490 : 730;
    const double sweep = rising ? 240 : -240;
    constexpr double pi = std::numbers::pi;
    const double peak = 1150.0 * volume / 100.0;
    for (unsigned i = 0; i < sampleCount; ++i) {
        const double progress = static_cast<double>(i) / (sampleCount - 1);
        const double time = static_cast<double>(i) / sampleRate;
        const double fade = std::sin(pi * progress);
        const double tone = std::sin(2 * pi * time * (start + 0.5 * sweep * progress));
        wave[header.size() + i] = static_cast<std::int16_t>(peak * fade * fade * tone);
    }

    const DWORD flags = SND_MEMORY | SND_NODEFAULT | (asynchronous ? SND_ASYNC : SND_SYNC);
    const auto played = PlaySoundW(reinterpret_cast<LPCWSTR>(wave.data()), nullptr, flags);
    readyAt = played && asynchronous ? GetTickCount64() + 110 : 0;
}
