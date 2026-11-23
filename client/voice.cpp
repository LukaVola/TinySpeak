#include "voice.h"
#include "voice_mix.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <livekit/livekit.h>
#include <livekit/audio_processing_module.h>
#include <livekit/audio_source.h>
#include <livekit/room.h>
#include <livekit/room_delegate.h>

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_GENERATION
#define MA_NO_NODE_GRAPH
#define MA_NO_AVX2
#define MA_NO_AVX512F
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

namespace {
constexpr int sampleRate = 48000;
constexpr int frameSize = sampleRate / 100;

struct Ring {
    std::array<std::int16_t, frameSize * 12> samples{};
    ma_pcm_rb buffer{};
    Ring() { ma_pcm_rb_init(ma_format_s16, 1, static_cast<ma_uint32>(samples.size()), samples.data(), nullptr, &buffer); }
    ~Ring() { ma_pcm_rb_uninit(&buffer); }
    void write(const std::int16_t* source, ma_uint32 count) {
        while (count) {
            ma_uint32 n = count;
            void* to = nullptr;
            if (ma_pcm_rb_acquire_write(&buffer, &n, &to) != MA_SUCCESS || !n) break;
            std::copy_n(source, n, static_cast<std::int16_t*>(to));
            ma_pcm_rb_commit_write(&buffer, n);
            source += n;
            count -= n;
        }
    }
    ma_uint32 read(std::int16_t* target, ma_uint32 count) {
        ma_uint32 read = 0;
        while (read < count) {
            ma_uint32 n = count - read;
            void* from = nullptr;
            if (ma_pcm_rb_acquire_read(&buffer, &n, &from) != MA_SUCCESS || !n) break;
            std::copy_n(static_cast<const std::int16_t*>(from), n, target + read);
            ma_pcm_rb_commit_read(&buffer, n);
            read += n;
        }
        return read;
    }
};

struct DeviceContext {
    ma_context ctx{};
    bool ok;
    DeviceContext() {
        const ma_backend backend = ma_backend_wasapi;
        ok = ma_context_init(&backend, 1, nullptr, &ctx) == MA_SUCCESS;
    }
    ~DeviceContext() { if (ok) ma_context_uninit(&ctx); }
};

std::wstring widen(const char* value) {
    int count = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (!count) return {};
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, -1, result.data(), count);
    result.pop_back();
    return result;
}

struct Connection final : livekit::RoomDelegate {
    struct Peer {
        std::string session;
        std::shared_ptr<Ring> samples;
    };

    Voice& owner;
    const std::atomic<std::shared_ptr<const Voice::Volumes>>& gains;
    livekit::Room room;
    std::shared_ptr<livekit::AudioSource> source;
    std::shared_ptr<livekit::LocalAudioTrack> track;
    std::mutex peersMutex;
    std::unordered_map<std::string, Peer> peers;
    DeviceContext context;
    ma_device device{};
    bool activeDevice = false;
    Ring input, reference;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::jthread processing;
    std::atomic_bool stopping{false};

    Connection(Voice& voice, const std::atomic<std::shared_ptr<const Voice::Volumes>>& volumes)
        : owner(voice), gains(volumes) { room.setDelegate(this); }
    ~Connection() override {
        stopping = true;
        if (activeDevice) ma_device_uninit(&device);
        if (processing.joinable()) {
            processing.request_stop();
            SetEvent(wake);
            processing.join();
        }
        room.setDelegate(nullptr);
        room.disconnect();
        if (wake) CloseHandle(wake);
        owner.level = 0;
        owner.talking = false;
    }

    void onTrackSubscribed(livekit::Room&, const livekit::TrackSubscribedEvent& event) override {
        if (stopping || !event.participant || !event.track || event.track->kind() != livekit::TrackKind::KIND_AUDIO) return;
        auto delay = std::make_shared<Ring>();
        auto identity = event.participant->identity();
        {
            std::lock_guard lock(peersMutex);
            peers[identity] = {std::string(speakerSession(identity)), delay};
        }
        livekit::AudioStream::Options options;
        options.capacity = 8;
        room.setOnAudioFrameCallback(identity, event.track->name(), [delay](const livekit::AudioFrame& frame) {
            if (frame.sampleRate() == sampleRate && frame.numChannels() == 1) {
                delay->write(frame.data().data(), static_cast<ma_uint32>(frame.data().size()));
            }
        }, options);
    }

    void onTrackUnsubscribed(livekit::Room&, const livekit::TrackUnsubscribedEvent& event) override {
        if (!event.participant || !event.track) return;
        std::lock_guard lock(peersMutex);
        peers.erase(event.participant->identity());
    }

    void onDisconnected(livekit::Room&, const livekit::DisconnectedEvent&) override {
        if (!stopping) owner.status = 3;
    }

    static void callback(ma_device* device, void* output, const void* capture, ma_uint32 frames) {
        auto& self = *static_cast<Connection*>(device->pUserData);
        if (capture) self.input.write(static_cast<const std::int16_t*>(capture), frames);
        if (output) self.mix(static_cast<std::int16_t*>(output), frames);
        if (capture) SetEvent(self.wake);
    }

    void mix(std::int16_t* output, ma_uint32 frames) {
        const int volume = owner.outputGain.load();
        const auto participantVolumes = gains.load();
        std::unique_lock lock(peersMutex, std::try_to_lock);
        while (frames) {
            const auto n = std::min<ma_uint32>(frames, frameSize);
            std::array<int, frameSize> sum{};
            std::array<std::int16_t, frameSize> samples{};
            if (lock.owns_lock()) {
                for (const auto& [_, peer] : peers) {
                    const auto count = peer.samples->read(samples.data(), n);
                    int gain = 100;
                    if (participantVolumes) {
                        const auto custom = participantVolumes->find(peer.session);
                        if (custom != participantVolumes->end()) gain = custom->second;
                    }
                    mixVoiceSamples(sum.data(), samples.data(), count, gain);
                }
            }
            for (ma_uint32 i = 0; i < n; ++i) {
                output[i] = static_cast<std::int16_t>(std::clamp(sum[i] * volume / 100, -32768, 32767));
            }
            reference.write(output, n);
            frames -= n;
            output += n;
        }
    }

    void process(std::stop_token stop) {
        livekit::AudioProcessingModule::Options options;
        options.echo_cancellation = true;
        options.noise_suppression = true;
        options.high_pass_filter = true;
        try {
            livekit::AudioProcessingModule apm(options);
            auto mic = livekit::AudioFrame::create(sampleRate, 1, frameSize);
            auto speaker = livekit::AudioFrame::create(sampleRate, 1, frameSize);
            int lastThreshold = -1;
            std::uint64_t minimumPower = 0;
            while (!stop.stop_requested()) {
                WaitForSingleObject(wake, 100);
                while (!stop.stop_requested() && ma_pcm_rb_available_read(&input.buffer) >= frameSize) {
                    auto& render = speaker.data();
                    const auto n = reference.read(render.data(), frameSize);
                    std::fill(render.begin() + n, render.end(), 0);
                    apm.processReverseStream(speaker);

                    auto& samples = mic.data();
                    input.read(samples.data(), frameSize);
                    apm.processStream(mic);
                    const int threshold = owner.threshold.load();
                    if (threshold != lastThreshold) {
                        lastThreshold = threshold;
                        minimumPower = microphoneGatePower(threshold, samples.size());
                    }
                    const bool microphoneAllowed = owner.permitted && !owner.muted && (owner.mode != 1 || owner.push);
                    const auto frame = gateMicrophoneFrame(samples, owner.inputGain.load(), minimumPower, microphoneAllowed);
                    owner.level = frame.peakPercent;
                    owner.talking = frame.transmitting;
                    try { source->captureFrame(mic, 20); } catch (const std::exception&) {}
                }
            }
        } catch (const std::exception&) {
            if (!stopping) owner.status = 4;
        }
    }

    bool open(const std::string& url, const std::string& token, const std::wstring& microphone, const std::wstring& playback) {
        if (!context.ok || !wake || !room.connect(url, token, livekit::RoomOptions{})) return false;
        ma_device_info *mics = nullptr, *speakers = nullptr;
        ma_uint32 micCount = 0, speakerCount = 0;
        ma_context_get_devices(&context.ctx, &speakers, &speakerCount, &mics, &micCount);

        auto find = [](const ma_device_info* list, ma_uint32 count, const std::wstring& id) -> const ma_device_id* {
            for (ma_uint32 i = 0; i < count; ++i) if (id == list[i].id.wasapi) return &list[i].id;
            return nullptr;
        };
        ma_device_config config = ma_device_config_init(ma_device_type_duplex);
        config.sampleRate = sampleRate;
        config.periodSizeInFrames = frameSize;
        config.periods = 3;
        config.capture.format = config.playback.format = ma_format_s16;
        config.capture.channels = config.playback.channels = 1;
        config.capture.pDeviceID = find(mics, micCount, microphone);
        config.playback.pDeviceID = find(speakers, speakerCount, playback);
        config.pUserData = this;
        config.dataCallback = callback;
        bool capture = micCount && ma_device_init(&context.ctx, &config, &device) == MA_SUCCESS;
        if (!capture) {
            config.deviceType = ma_device_type_playback;
            if (ma_device_init(&context.ctx, &config, &device) != MA_SUCCESS) return false;
        }
        activeDevice = true;
        if (capture) {
            source = std::make_shared<livekit::AudioSource>(sampleRate, 1, 20);
            track = livekit::LocalAudioTrack::createLocalAudioTrack("microphone", source);
            livekit::TrackPublishOptions options;
            options.source = livekit::TrackSource::SOURCE_MICROPHONE;
            options.audio_encoding = livekit::AudioEncodingOptions{1000ULL * owner.bitrateKbps.load()};
            options.dtx = true;
            options.red = true;
            auto participant = room.localParticipant().lock();
            if (!participant) return false;
            participant->publishTrack(track, options);
            processing = std::jthread([this](std::stop_token stop) { process(stop); });
        }
        if (ma_device_start(&device) != MA_SUCCESS) return false;
        owner.status = capture ? 2 : 4;
        return true;
    }
};
}

struct Voice::Impl {
    Voice& owner;
    std::atomic<std::shared_ptr<const Voice::Volumes>> gains;
    std::mutex mutex;
    std::condition_variable ready;
    std::string url, token;
    std::wstring input, output;
    bool updated = false, exit = false;
    std::thread worker;

    explicit Impl(Voice& voice) : owner(voice), worker([this] { run(); }) {}
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            exit = true;
        }
        ready.notify_one();
        worker.join();
    }

    void run() {
        bool initialized = false;
        std::unique_ptr<Connection> current;
        for (;;) {
            std::unique_lock lock(mutex);
            ready.wait(lock, [this] { return exit || updated; });
            if (exit) break;
            std::string to = url, access = token;
            std::wstring microphone = input, speaker = output;
            updated = false;
            lock.unlock();
            current.reset();
            if (to.empty()) {
                owner.status = 0;
                continue;
            }
            owner.status = 1;
            try {
                if (!initialized) initialized = livekit::initialize(livekit::LogLevel::Off);
                if (initialized) {
                    current = std::make_unique<Connection>(owner, gains);
                    if (current->open(to, access, microphone, speaker)) {
                        std::lock_guard latest(mutex);
                        owner.permitted = !updated && !exit;
                        continue;
                    }
                }
            } catch (const std::exception&) {}
            current.reset();
            owner.status = 3;
        }
        current.reset();
        if (initialized) livekit::shutdown();
    }
};

Voice::Voice() : impl_(std::make_unique<Impl>(*this)) {}
Voice::~Voice() = default;

void Voice::connect(std::string url, std::string token, std::wstring input, std::wstring output) {
    std::lock_guard lock(impl_->mutex);
    permitted = false;
    status = url.empty() ? 0 : 1;
    impl_->url = std::move(url);
    impl_->token = std::move(token);
    impl_->input = std::move(input);
    impl_->output = std::move(output);
    impl_->updated = true;
    impl_->ready.notify_one();
}

void Voice::disconnect() { connect({}, {}, {}, {}); }

void Voice::setParticipantVolumes(Volumes volumes) {
    if (volumes.empty()) impl_->gains.store(nullptr);
    else impl_->gains.store(std::make_shared<const Volumes>(std::move(volumes)));
}

std::pair<std::vector<AudioDevice>, std::vector<AudioDevice>> Voice::devices() const {
    DeviceContext context;
    std::pair<std::vector<AudioDevice>, std::vector<AudioDevice>> result;
    if (!context.ok) return result;
    ma_device_info *mics = nullptr, *speakers = nullptr;
    ma_uint32 micCount = 0, speakerCount = 0;
    if (ma_context_get_devices(&context.ctx, &speakers, &speakerCount, &mics, &micCount) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < micCount; ++i) result.first.push_back({mics[i].id.wasapi, widen(mics[i].name)});
        for (ma_uint32 i = 0; i < speakerCount; ++i) result.second.push_back({speakers[i].id.wasapi, widen(speakers[i].name)});
    }
    return result;
}
