#include "voice.h"

#include <livekit/livekit.h>
#include <livekit/audio_source.h>
#include <livekit/room.h>
#include <livekit/room_delegate.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Timer = std::chrono::steady_clock;

constexpr int frameSize = 480;
constexpr int sampleRate = 48000;
constexpr int pulseCount = 5;
constexpr int pulseStart = 85;
constexpr int pulseGap = 55;
constexpr int pulseWidth = 12;
constexpr int frames = pulseStart + (pulseCount - 1) * pulseGap + pulseWidth + 65;

class Receiver : public livekit::RoomDelegate {
public:
    std::atomic_int subscriptions{0};

    void onTrackSubscribed(livekit::Room& room, const livekit::TrackSubscribedEvent& event) override {
        if (!event.participant || !event.track || event.track->kind() != livekit::TrackKind::KIND_AUDIO) return;
        livekit::AudioStream::Options options;
        options.capacity = 8;
        room.setOnAudioFrameCallback(event.participant->identity(), event.track->name(),
            [this](const livekit::AudioFrame& frame) { detect(frame); }, options);
        ++subscriptions;
    }

    std::vector<Timer::time_point> received() {
        std::lock_guard lock(mutex_);
        return arrivals_;
    }

private:
    std::mutex mutex_;
    Timer::time_point lastSound_{};
    std::vector<Timer::time_point> arrivals_;

    void detect(const livekit::AudioFrame& frame) {
        const auto at = Timer::now();
        if (frame.sampleRate() != sampleRate || frame.numChannels() != 1 || frame.data().empty()) return;
        std::uint64_t energy = 0;
        for (auto sample : frame.data()) energy += static_cast<int>(sample) * static_cast<int>(sample);
        if (energy <= 1000ULL * 1000 * frame.data().size()) return;
        std::lock_guard lock(mutex_);
        if (arrivals_.empty() || at - lastSound_ > 210ms) arrivals_.push_back(at);
        lastSound_ = at;
    }
};

struct Publisher {
    std::shared_ptr<livekit::AudioSource> source = std::make_shared<livekit::AudioSource>(sampleRate, 1, 20);
    std::shared_ptr<livekit::LocalAudioTrack> track = livekit::LocalAudioTrack::createLocalAudioTrack("microphone", source);
    livekit::AudioFrame frame = livekit::AudioFrame::create(sampleRate, 1, frameSize);
    std::array<std::int16_t, frameSize> tone{};
    std::vector<Timer::time_point> sent;

    Publisher(livekit::Room& room, int bitrate, int frequency) {
        livekit::TrackPublishOptions options;
        options.source = livekit::TrackSource::SOURCE_MICROPHONE;
        options.audio_encoding = livekit::AudioEncodingOptions{1000ULL * validatedVoiceBitrate(bitrate)};
        options.dtx = true;
        options.red = true;
        room.localParticipant().lock()->publishTrack(track, options);
        for (std::size_t i = 0; i < tone.size(); ++i) {
            const auto phase = 2 * std::numbers::pi * frequency * i / sampleRate;
            tone[i] = static_cast<std::int16_t>(std::sin(phase) * 9500);
        }
    }

    void send(int index) {
        const auto offset = index - pulseStart;
        const bool sound = offset >= 0 && offset / pulseGap < pulseCount && offset % pulseGap < pulseWidth;
        auto& samples = frame.data();
        if (sound) {
            std::copy(tone.begin(), tone.end(), samples.begin());
        } else {
            std::fill(samples.begin(), samples.end(), 0);
        }
        gateMicrophoneFrame(samples, 100, microphoneGatePower(microphoneThresholdDefault, samples.size()), true);
        if (sound && offset % pulseGap == 0) sent.push_back(Timer::now());
        source->captureFrame(frame, 35);
    }
};

bool report(const char* direction, const Publisher& sender, Receiver& receiver) {
    auto received = receiver.received();
    if (sender.sent.size() != pulseCount || received.size() != pulseCount) {
        std::cerr << direction << " heard " << received.size() << " / " << pulseCount << " pulses\n";
        return false;
    }
    std::array<long long, pulseCount> elapsed{};
    for (int i = 0; i < pulseCount; ++i) {
        elapsed[i] = std::chrono::duration_cast<std::chrono::milliseconds>(received[i] - sender.sent[i]).count();
        if (elapsed[i] < 0) return false;
    }
    std::sort(elapsed.begin(), elapsed.end());
    std::cout << direction << " " << pulseCount << "/" << pulseCount << " received, one-way ms min/median/max "
        << elapsed.front() << "/" << elapsed[pulseCount / 2] << "/" << elapsed.back() << '\n';
    return true;
}

int main(int argc, char** argv) {
    if ((argc != 6 && argc != 9 && argc != 12) || !livekit::initialize(std::getenv("TINYSPEAK_TEST_TRACE") ? livekit::LogLevel::Debug : livekit::LogLevel::Off)) return 1;
    const int bitrate = std::stoi(argv[4]);
    bool ok = false;
    try {
        Receiver receiverA, receiverB;
        livekit::Room roomA, roomB;
        roomA.setDelegate(&receiverA);
        roomB.setDelegate(&receiverB);
        auto viaTurn = [](char** config) {
            livekit::RoomOptions guest;
            livekit::RtcConfig rtc;
            rtc.ice_transport_type = 0;
            rtc.ice_servers.push_back({"turn:" + std::string(config[0]) + "?transport=tcp", config[1], config[2]});
            guest.rtc_config = std::move(rtc);
            return guest;
        };
        const auto guest = argc >=9 ? viaTurn(argv+6) : livekit::RoomOptions{};
        const auto owner = argc ==12 ? viaTurn(argv+9) : livekit::RoomOptions{};
        if (!roomA.connect(argv[1], argv[2], owner)) { std::cerr<<"First RTC connection failed\n"; return 2; }
        std::cerr<<"First RTC connection established\n";
        if (!roomB.connect(argv[5], argv[3], guest)) { std::cerr<<"Second RTC connection failed\n"; return 2; }
        std::cerr<<"Second RTC connection established\n";
        Publisher senderA(roomA, bitrate, 500), senderB(roomB, bitrate, 900);
        for (int wait = 0; wait < 40 && (!receiverA.subscriptions || !receiverB.subscriptions); ++wait) {
            std::this_thread::sleep_for(50ms);
        }
        if (!receiverA.subscriptions || !receiverB.subscriptions) return 3;
        auto next = Timer::now();
        for (int index = 0; index < frames; ++index) {
            senderA.send(index);
            senderB.send(index);
            next += 10ms;
            std::this_thread::sleep_until(next);
        }
        const bool aToB = report("A -> B", senderA, receiverB);
        const bool bToA = report("B -> A", senderB, receiverA);
        ok = aToB && bToA;
        roomA.setDelegate(nullptr);
        roomB.setDelegate(nullptr);
        roomA.disconnect();
        roomB.disconnect();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    livekit::shutdown();
    return ok ? 0 : 4;
}

