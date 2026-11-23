#include "chat_view.h"
#include "chat_sounds.h"
#include "host_process.h"
#include "layout.h"
#include "network.h"
#include "palette.h"
#include "profile.h"
#include "ui.h"
#include "voice.h"

#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;
namespace {
constexpr wchar_t windowClass[] = L"TinySpeak.Desktop";
constexpr wchar_t localAddress[] = L"http://127.0.0.1:8090";
const std::vector<std::wstring> colorNames{L"Red", L"Orange", L"Yellow", L"Lime",
    L"Green", L"Mint", L"Turquoise", L"Blue", L"Purple", L"Magenta", L"Pink", L"Brown",
    L"Black", L"Dark gray", L"Gray", L"White"};

enum Control { ServerList = 100, NewServer, Join, Actions, Profile, Settings, Archive, Tree, Transcript, Message, Send,
    Invite, LeaveServer, DeleteServer, AddTextRoom, AddVoiceRoom, DeleteRoom, Kick, Export,
    Mic, LeaveVoice, ParticipantVolume, MicSlider, OutputSlider, MuteParticipant, KickVoice, RenameRoom, LeaveText, InviteCode };
constexpr unsigned serverMenuBase = 1000;
constexpr int maxMicrophoneGain = 150;
constexpr int maxOutputGain = 110;
constexpr UINT_PTR uiTimer = 1;
constexpr UINT inviteReceived = WM_APP + 2;

Json arrayOr(const Json& doc, const char* name) {
    auto it = doc.find(name);
    return it != doc.end() && it->is_array() ? *it : Json::array();
}

bool validCode(const std::string& code, std::size_t length = 16) {
    return code.size() == length && std::all_of(code.begin(), code.end(), [](unsigned char c) {
        return ('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z') || ('0' <= c && c <= '9');
    });
}

std::string parseCode(const std::wstring& value) {
    auto text = utf8(value);
    auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    text = text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
    constexpr std::string_view current = "tinyspeak://join/";
    constexpr std::string_view legacy = "kanal://join/";
    if (text.starts_with(current)) text.erase(0, current.size());
    else if (text.starts_with(legacy)) text.erase(0, legacy.size());
    return validCode(text) || hostedInvite(text) ? text : "";
}

Json loadJson(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return Json::object();
    auto doc = Json::parse(stream, nullptr, false);
    return doc.is_discarded() ? Json::object() : doc;
}

bool writeText(const fs::path& file, const std::string& content, bool atomic = false) {
    auto output = file;
    if (atomic) output += L".tmp";
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.close();
    return stream && (!atomic || MoveFileExW(output.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
}

bool writeJson(const fs::path& file, const Json& doc) { return writeText(file, doc.dump(2), true); }

fs::path dataPath() {
    wchar_t value[32768];
    DWORD count = GetEnvironmentVariableW(L"KANAL_DATA_PATH", value, static_cast<DWORD>(std::size(value)));
    if (count && count < std::size(value)) return fs::path(value);
    return profilePath(!GetEnvironmentVariableW(L"KANAL_DEMO_ADDRESS", nullptr, 0));
}

void clipboard(HWND window, const std::wstring& value) {
    if (!OpenClipboard(window)) return;
    HGLOBAL buffer = GlobalAlloc(GMEM_MOVEABLE, (value.size() + 1) * sizeof(wchar_t));
    if (buffer) {
        auto* destination = GlobalLock(buffer);
        if (destination) {
            memcpy(destination, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(buffer);
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, buffer)) buffer = nullptr;
        }
        if (buffer) GlobalFree(buffer);
    }
    CloseClipboard();
}

bool registerInviteLink() {
    wchar_t exe[32768];
    DWORD size = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (!size || size >= std::size(exe)) return false;
    HKEY key = nullptr;
    constexpr wchar_t root[] = L"Software\\Classes\\tinyspeak";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, root, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    constexpr wchar_t title[] = L"URL:TinySpeak invitation";
    constexpr wchar_t empty[] = L"";
    RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(title), sizeof(title));
    RegSetValueExW(key, L"URL Protocol", 0, REG_SZ, reinterpret_cast<const BYTE*>(empty), sizeof(empty));
    RegCloseKey(key);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\tinyspeak\\shell\\open\\command", 0,
        nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const auto cmd = L"\"" + std::wstring(exe, size) + L"\" \"%1\"";
    LONG result = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
        static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

struct Owned {
    std::string code, name, secret;
    Json rooms;
};

struct Node {
    std::string kind, id;
};

class App {
public:
    HWND window = nullptr;
    explicit App(std::string invite) : initialInvite(std::move(invite)) {}

    void setup();
    void arrange();
    void paint();
    void onNetwork(NetworkResult result);
    void command(int id, int notification);
    LRESULT onNotify(NMHDR* note);
    void tick();
    void close();

private:
    HWND servers{}, tree{}, transcript{}, composer{}, newServer{}, join{}, actions{}, muteButton{}, micSlider{}, outputSlider{};
    HFONT regular{}, bold{}, heading{};
    HBRUSH background{}, fieldBrush{};
    HIMAGELIST icons{};
    std::unique_ptr<Network> network;
    std::unique_ptr<Network> parkedHost;
    HostProcess host, probes;
    std::unique_ptr<HostProcess> guest, joiningGuest;
    Voice audio;
    ChatSounds chatSounds;
    unsigned generation = 0, nextGeneration = 0, parkedGeneration = 0;
    std::uint64_t version = 0;
    bool connected = false, registering = false, archiveChat = false, retainOwned = false, remember = true, rememberColor = true;
    bool rebuilding = false, closing = false;
    bool textClosed = false;
    bool directHosting = true, hostStarting = false, onHost = false, hostInternet = false, parkedRegistered = false;
    bool fivePercentSteps = true;
    EqualizerGains savedEqualizer{};
    bool savedNormalization = false;
    int color = 0, pushKey = VK_F8, audioStatus = -1, displayedLevel = -1;
    bool pushWasDown = false, displayedTalk = false;
    ULONGLONG clockNext = 0, retryAt = 0, joinUntil = 0, nextRegistration = 0, probeAt = 0, guestOfflineSince = 0;
    unsigned timerInterval = 250;
    std::wstring address{localAddress}, microphone, playback, status{L"Connecting to the relay…"};
    std::string name, id, initialInvite, pendingJoin, currentText, currentVoice, profileSecret;
    std::wstring hostAddress, guestAddress;
    std::string hostRoute, hostTicket, parkedID, hostJoin, guestInvite, candidateInvite, afterRegistration;
    int guestTurnPort = 0;
    std::string guestTurnHost;
    fs::path directory;
    std::map<std::string, Owned> owned;
    std::map<std::string, std::string> bookmarks, available;
    std::map<std::string, std::string> bookmarkInvites;
    std::vector<std::string> probingBookmarks;
    std::string lastProbedBookmark;
    std::vector<std::pair<std::string, std::string>> serverRows;
    Json state = Json::object();
    std::unordered_map<HTREEITEM, Node> nodes;
    std::unordered_map<std::string, std::deque<ChatMessage>> chat;
    std::deque<std::pair<fs::path, std::string>> logs;
    std::size_t chatBytes = 0;
    unsigned displayedDay = 0;
    Voice::Volumes savedVolumes;
    std::unordered_set<std::string> mutedParticipants;
    std::string previewPeer;
    int previewVolume = 100;

    std::string active() const { return state.value("code", ""); }
    std::string activeInvite() const {
        const auto code = active();
        if (code.empty() || !directHosting) return code;
        if (!onHost) return guestInvite;
        return hostTicket.empty() ? "TS1-" + hostRoute + "." + code : "TS2-" + hostRoute + "." + hostTicket + "." + code;
    }
    bool owner() const { return !id.empty() && state.value("owner", "") == id; }
    std::wstring roomName(const std::string& value) const;
    void sendRequest(Json request);
    void statusText(std::wstring text);
    void load();
    void save();
    bool saveLayout(const Owned& item);
    void registerClient();
    void startNetwork(const std::wstring& endpoint);
    void useHost();
    void ensureHost(std::string joinCode = {});
    void tickHelpers();
    void probeBookmarks();
    void submitJoin(const std::string& code);
    void parkedHostResult(NetworkResult result);
    void advertise(const Owned& item);
    void catalog();
    void refreshServers();
    void applyState(const Json& data);
    void refreshTree();
    void refreshChat();
    void refreshActions();
    void updateParticipantVolumes();
    Json voicePeer(const std::string& session) const;
    std::string participantRoom(const std::string& session) const;
    std::string legacyPeerKey(const Json& member) const;
    std::string peerKey(const Json& member) const;
    int participantVolume(const Json& member) const;
    void promptParticipantVolume(const std::string& session);
    void treeMenu(const Node& node, const POINT& screen);
    void saveVoicePreferences();
    void redrawAudioPanel();
    void configureVolumeSliders();
    void appendChat(const Json& event);
    void flushLogs();
    void requestJoin(const std::string& code);
    void promptJoin(const std::wstring& value = {});
    void promptCreate();
    void promptProfile();
    void promptSettings();
    void promptPlayback(HWND parent);
    void serverPicker();
    void popupActions();
    void addRoom(bool text);
    Node selectedNode() const;
    void localLogs();
    void exportLog(const std::string& text, const std::wstring& filename);
    void textSend();
    void voiceRequest(const std::string& code);
    void selectTree();
    void statusAudio();
    void handleEvent(const Json& event, Network* source);

    friend LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
};

std::wstring App::roomName(const std::string& value) const {
    for (const auto& room : arrayOr(state, "rooms")) {
        if (room.value("id", "") == value) return wide(room.value("name", ""));
    }
    return {};
}

void App::load() {
    directory = dataPath();
    std::error_code ec;
    fs::create_directories(directory / L"servers", ec);
    fs::create_directories(directory / L"logs", ec);
    auto doc = loadJson(directory / L"settings.json");
    if (!doc.is_object()) doc = Json::object();
    const auto bands = arrayOr(doc, "playback_eq_db");
    for (std::size_t i = 0; i < std::min(bands.size(), savedEqualizer.size()); ++i) {
        if (bands[i].is_number_integer()) {
            const auto value = bands[i].get<std::int64_t>();
            if (value >= -equalizerLimitDb && value <= equalizerLimitDb) savedEqualizer[i] = static_cast<std::int8_t>(value);
        }
    }
    savedNormalization = doc.contains("normalize_playback") && doc["normalize_playback"] == true;
    audio.equalizerDb = savedEqualizer;
    audio.normalizePlayback = savedNormalization;
    try {
        name = doc.value("name", "");
        if (name.empty()) name = "Guest" + randomCode(4);
        address = wide(doc.value("address", utf8(localAddress)));
        remember = doc.value("remember", true);
        rememberColor = doc.value("remember_color", true);
        color = rememberColor && doc.contains("color") ? std::clamp(doc.value("color", 0), 0, 15) :
            static_cast<unsigned char>(randomCode(1).front()) % 16;
        archiveChat = doc.value("local_chat", false);
        retainOwned = doc.value("save_owned", false);
        microphone = wide(doc.value("microphone", ""));
        playback = wide(doc.value("playback", ""));
        audio.mode = std::clamp(doc.value("audio_mode", 0), 0, 2);
        audio.inputGain = std::clamp(doc.value("mic_gain", 100), 0, maxMicrophoneGain);
        audio.outputGain = std::clamp(doc.value("output_gain", 100), 0, maxOutputGain);
        fivePercentSteps = doc.value("five_percent_steps", true);
        audio.threshold = std::clamp(doc.value("mic_threshold", microphoneThresholdDefault), 0, microphoneThresholdMax);
        audio.bitrateKbps = validatedVoiceBitrate(doc.value("voice_bitrate_kbps", defaultVoiceBitrate));
        pushKey = std::clamp(doc.value("ptt", VK_F8), VK_F6, VK_F12);
        if (remember) {
            for (const auto& bookmark : arrayOr(doc, "bookmarks")) {
                auto code = bookmark.value("code", "");
                if (validCode(code)) {
                    bookmarks[code] = bookmark.value("name", "");
                    auto invite = bookmark.value("invite", "");
                    if (hostedInvite(invite) && invite.ends_with("." + code)) bookmarkInvites.emplace(code, std::move(invite));
                }
            }
        }
        if (retainOwned) {
            for (const auto& item : arrayOr(doc, "owned")) {
                auto code = item.value("code", ""), secret = item.value("secret", "");
                if (validCode(code) && validCode(secret, 40)) owned.emplace(code, Owned{code, item.value("name", ""), secret, {}});
            }
        }
    } catch (const Json::exception&) {
        if (name.empty()) name = "Guest" + randomCode(4);
    }
    const auto voicePrefs = loadJson(directory / L"voice-preferences.json");
    if (voicePrefs.is_object()) {
        if (auto secret = voicePrefs.find("profile"); secret != voicePrefs.end() && secret->is_string()) {
            profileSecret = secret->get<std::string>();
        }
        if (auto volumes = voicePrefs.find("volumes"); volumes != voicePrefs.end() && volumes->is_object()) {
            for (const auto& [key, gain] : volumes->items()) {
                if (gain.is_number_integer()) {
                    const auto percent = gain.get<std::int64_t>();
                    if (percent >= 0 && percent <= maxParticipantGain && percent != 100) savedVolumes.emplace(key, static_cast<int>(percent));
                }
            }
        }
        for (const auto& key : arrayOr(voicePrefs, "muted")) {
            if (key.is_string()) mutedParticipants.insert(key.get<std::string>());
        }
    }
    if (!validCode(profileSecret, 40)) {
        profileSecret = randomCode(40);
        saveVoicePreferences();
    }
    wchar_t demoAddress[256]{};
    const DWORD length = GetEnvironmentVariableW(L"KANAL_DEMO_ADDRESS", demoAddress, ARRAYSIZE(demoAddress));
    if (length && length < ARRAYSIZE(demoAddress)) {
        address.assign(demoAddress, length);
        directHosting = false;
    } else if (address.starts_with(L"https://")) {
        directHosting = false;
    }
}

void App::saveVoicePreferences() {
    if (!writeJson(directory / L"voice-preferences.json",
        Json{{"profile", profileSecret}, {"volumes", savedVolumes}, {"muted", mutedParticipants}})) {
        statusText(L"Voice preferences could not be saved.");
    }
}

bool App::saveLayout(const Owned& item) {
    if (!retainOwned || !item.rooms.is_array()) return false;
    if (!writeJson(directory / L"servers" / wide(item.code + ".json"), item.rooms)) {
        statusText(L"Server settings could not be saved.");
        return false;
    }
    return true;
}

void App::save() {
    Json doc{{"address", utf8(address)}, {"name", name}, {"remember", remember}, {"remember_color", rememberColor},
        {"local_chat", archiveChat}, {"save_owned", retainOwned}, {"microphone", utf8(microphone)},
        {"playback", utf8(playback)}, {"audio_mode", audio.mode.load()}, {"mic_gain", audio.inputGain.load()},
        {"output_gain", audio.outputGain.load()}, {"mic_threshold", audio.threshold.load()},
        {"playback_eq_db", savedEqualizer}, {"normalize_playback", savedNormalization},
        {"voice_bitrate_kbps", audio.bitrateKbps.load()}, {"five_percent_steps", fivePercentSteps}, {"ptt", pushKey}};
    if (rememberColor) doc["color"] = color;
    if (remember) {
        auto& list = doc["bookmarks"] = Json::array();
        for (const auto& [code, title] : bookmarks) {
            const auto invite = bookmarkInvites.find(code);
            auto entry = Json{{"code", code}, {"name", title}};
            if (invite != bookmarkInvites.end()) entry["invite"] = invite->second;
            list.push_back(std::move(entry));
        }
    }
    if (retainOwned) {
        auto& list = doc["owned"] = Json::array();
        for (auto& [code, item] : owned) {
            saveLayout(item);
            list.push_back({{"code", code}, {"name", item.name}, {"secret", item.secret}});
        }
    }
    if (!writeJson(directory / L"settings.json", doc)) statusText(L"Settings could not be saved on this computer.");
}

void App::sendRequest(Json request) { if (network) network->send(std::move(request)); }

void App::statusText(std::wstring text) {
    if (status == text) return;
    status = std::move(text);
    RECT rect{};
    GetClientRect(window, &rect);
    rect.top = rect.bottom - 26;
    InvalidateRect(window, &rect, FALSE);
}

void App::registerClient() {
    if (!network || registering) return;
    registering = true;
    nextRegistration = GetTickCount64() + 4000;
    sendRequest({{"op", "register"}, {"name", name}, {"color", color}, {"profile", profileSecret}});
}

void App::startNetwork(const std::wstring& endpoint) {
    if (network) network->finish();
    connected = false;
    registering = false;
    version = 0;
    guestOfflineSince = 0;
    id.clear();
    if (!directHosting) available.clear();
    applyState(Json::object());
    pendingJoin.clear();
    retryAt = 0;
    generation = ++nextGeneration;
    network = std::make_unique<Network>(window, endpoint, generation);
    statusText(L"Connecting to the relay…");
    registerClient();
}

void App::useHost() {
    if (onHost) {
        if (connected && !hostJoin.empty()) submitJoin(std::exchange(hostJoin, {}));
        return;
    }
    if (parkedHost) {
        if (network) network->finish();
        network = std::move(parkedHost);
        generation = parkedGeneration;
        id = std::exchange(parkedID, {});
        connected = parkedRegistered;
        registering = !connected;
        version = 0;
        pendingJoin.clear();
        retryAt = 0;
        applyState(Json::object());
    } else {
        startNetwork(hostAddress);
    }
    onHost = true;
    guestOfflineSince = 0;
    guest.reset();
    guestInvite.clear();
    guestAddress.clear();
    if (connected) {
        for (const auto& [_, item] : owned) advertise(item);
        if (!hostJoin.empty()) submitJoin(std::exchange(hostJoin, {}));
        else statusText(L"TinySpeak is hosting your servers.");
    }
}

void App::ensureHost(std::string joinCode) {
    if (!joinCode.empty()) hostJoin = std::move(joinCode);
    if (!hostAddress.empty() && host.running()) {
        if (!hostJoin.empty() || !network) useHost();
        return;
    }
    if (hostStarting) return;
    if (!host.start(L"host \"" + directory.wstring() + L"\"", L"TinySpeak Host.exe")) {
        statusText(L"TinySpeak could not start hosting. Extract all files from the ZIP.");
        return;
    }
    hostStarting = true;
    statusText(L"Starting your TinySpeak host…");
    refreshActions();
}

void App::parkedHostResult(NetworkResult result) {
    const auto& reply = result.reply;
    if (result.request.value("op", "") == "register" && reply.contains("key")) {
        parkedID = reply.at("user").value("id", "");
        parkedRegistered = true;
        for (const auto& [_, item] : owned) {
            parkedHost->send({{"op", "advertise"}, {"code", item.code}, {"name", item.name}, {"secret", item.secret}, {"archive", retainOwned}});
        }
    } else if (reply.value("expired", false) && parkedRegistered) {
        parkedRegistered = false;
        parkedHost->send({{"op", "register"}, {"name", name}, {"color", color}, {"profile", profileSecret}});
    }
    for (const auto& event : arrayOr(reply, "events")) handleEvent(event, parkedHost.get());
}

void App::tickHelpers() {
    if (!directHosting) return;
    if (hostStarting) {
        const auto reply = host.read();
        if (reply || !host.running()) {
            hostStarting = false;
            hostAddress = reply ? wide(reply->value("address", "")) : L"";
            hostRoute = reply ? reply->value("route", "") : "";
            hostTicket = reply ? reply->value("ticket", "") : "";
            hostInternet = reply && reply->value("internet", false);
            if (hostAddress.empty() || hostRoute.size() != 32) {
                statusText(reply ? wide(reply->value("error", "The host could not start.")) : L"The host could not start.");
                host.stop();
            } else if (!network || !hostJoin.empty()) {
                afterRegistration.clear();
                useHost();
            } else {
                parkedGeneration = ++nextGeneration;
                parkedRegistered = false;
                parkedHost = std::make_unique<Network>(window, hostAddress, parkedGeneration);
                parkedHost->send({{"op", "register"}, {"name", name}, {"color", color}, {"profile", profileSecret}});
            }
            refreshActions();
        }
    } else if (!hostAddress.empty() && !host.running()) {
        hostAddress.clear();
        hostRoute.clear();
        parkedHost.reset();
        if (onHost) {
            network.reset();
            connected = false;
            onHost = false;
            applyState(Json::object());
            statusText(L"The TinySpeak host stopped. Select an owned server to restart it.");
        }
    }
    if (joiningGuest) {
        const auto reply = joiningGuest->read();
        if (reply || !joiningGuest->running()) {
            auto endpoint = reply ? wide(reply->value("address", "")) : L"";
            if (endpoint.empty()) {
                statusText(reply ? wide(reply->value("error", "Could not reach the host.")) : L"Could not reach the host.");
                joiningGuest.reset();
            } else {
                if (onHost && network) {
                    if (!active().empty()) network->send({{"op", "leave"}});
                    parkedGeneration = generation;
                    parkedID = id;
                    parkedRegistered = connected;
                    parkedHost = std::move(network);
                }
                onHost = false;
                afterRegistration = candidateInvite.substr(candidateInvite.size() - 16);
                guestTurnPort = reply->value("turnPort", 0);
                guestTurnHost = reply->value("turnHost", "");
                startNetwork(endpoint);
                guest = std::move(joiningGuest);
                guestAddress = std::move(endpoint);
                guestInvite = std::move(candidateInvite);
            }
            refreshActions();
        }
    }
    if (guest && !onHost && (!guest->running() || (guestOfflineSince && GetTickCount64() - guestOfflineSince >= 12000))) {
        auto old = std::move(network);
        generation = ++nextGeneration;
        connected = registering = false;
        guestOfflineSince = 0;
        afterRegistration.clear();
        pendingJoin.clear();
        applyState(Json::object());
        old.reset();
        guest.reset();
        guestAddress.clear();
        guestInvite.clear();
        statusText(L"The connection to the host was lost.");
    }
    if (const auto scan = probes.read(); scan && scan->contains("catalog")) {
        auto live = available;
        for (const auto& checked : probingBookmarks) live.erase(checked);
        std::erase_if(live, [&](const auto& item) { return !bookmarkInvites.contains(item.first); });
        for (const auto& item : arrayOr(*scan, "catalog")) {
            const auto code = item.value("code", "");
            if (bookmarkInvites.contains(code)) live[code] = item.value("name", "");
        }
        if (!onHost && connected && bookmarkInvites.contains(active())) live[active()] = bookmarks[active()];
        if (live != available) {
            available = std::move(live);
            refreshServers();
        }
        probes.stop();
    }
}

void App::probeBookmarks() {
    const auto now = GetTickCount64();
    if (!directHosting || bookmarkInvites.empty() || probes.running() || now < probeAt) return;
    probeAt = now + 20000;
    std::wstring arguments = L"probe";
    probingBookmarks.clear();
    auto it = bookmarkInvites.upper_bound(lastProbedBookmark);
    const auto current = onHost ? std::string{} : active();
    for (size_t count = 0; count < bookmarkInvites.size() && probingBookmarks.size() < 16; ++count) {
        if (it == bookmarkInvites.end()) it = bookmarkInvites.begin();
        lastProbedBookmark = it->first;
        if (it->first != current) {
            probingBookmarks.push_back(it->first);
            arguments += L" " + wide(it->second);
        }
        ++it;
    }
    if (!probingBookmarks.empty()) probes.start(arguments);
}

void App::advertise(const Owned& item) {
    sendRequest({{"op", "advertise"}, {"code", item.code}, {"name", item.name}, {"secret", item.secret}, {"archive", retainOwned}});
}

void App::catalog() {
    if (directHosting) {
        probeBookmarks();
        return;
    }
    if (!connected) return;
    Json codes = Json::array();
    for (const auto& [code, _] : bookmarks) {
        if (!owned.contains(code) && codes.size() < 512) codes.push_back(code);
    }
    if (!codes.empty()) sendRequest({{"op", "catalog"}, {"codes", codes}});
    else if (!available.empty()) {
        available.clear();
        refreshServers();
    }
}

void App::refreshServers() {
    serverRows.clear();
    for (const auto& [code, item] : owned) serverRows.emplace_back(code, item.name);
    for (const auto& [code, title] : available) if (!owned.contains(code)) serverRows.emplace_back(code, title);
    const auto code = active();
    const auto serverName = state.value("name", "");
    if (!code.empty() && !owned.contains(code) && !available.contains(code)) serverRows.emplace_back(code, serverName);
    putText(servers, code.empty() ? L"Select a server" : wide(serverName + "  ·  " + code));
    InvalidateRect(servers, nullptr, FALSE);
}

void App::applyState(const Json& data) {
    const auto old = active();
    auto previous = state;
    state = data;
    if (active() != old) {
        if (auto item = owned.find(old); item != owned.end() && saveLayout(item->second)) item->second.rooms = Json{};
        currentText.clear();
        textClosed = false;
        currentVoice.clear();
        audio.disconnect();
        chat.clear();
        chatBytes = 0;
        refreshServers();
    }
    const auto assignedVoice = state.value("voice", "");
    if (assignedVoice.empty() && !currentVoice.empty()) {
        currentVoice.clear();
        audio.disconnect();
    }
    bool sameRoom = false;
    std::string firstText;
    for (const auto& room : arrayOr(state, "rooms")) {
        if (room.value("kind", "") != "text") continue;
        if (firstText.empty()) firstText = room.value("id", "");
        sameRoom |= room.value("id", "") == currentText;
    }
    if (!sameRoom) {
        currentText = textClosed ? std::string{} : firstText;
        refreshChat();
    }
    if (auto pos = owned.find(active()); pos != owned.end()) {
        auto layout = arrayOr(state, "rooms");
        if (pos->second.rooms != layout) {
            pos->second.rooms = std::move(layout);
            saveLayout(pos->second);
        }
    }
    if (previous != state) {
        refreshTree();
        updateParticipantVolumes();
        const auto presence = chatPresence(previous, state);
        const auto callPresence = voicePresence(previous, state);
        const int volume = audio.outputGain.load();
        if (presence.left || callPresence.left) chatSounds.notify(ChatCue::Leave, volume);
        if (presence.joined || callPresence.joined) chatSounds.notify(ChatCue::Join, volume);
    }
    refreshActions();
}

void App::refreshTree() {
    rebuilding = true;
    SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree);
    nodes.clear();
    auto add = [&](HTREEITEM parent, std::wstring text, Node data, int image = I_IMAGENONE) {
        TVINSERTSTRUCTW entry{};
        entry.hParent = parent;
        entry.hInsertAfter = TVI_LAST;
        entry.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        entry.item.pszText = text.data();
        entry.item.iImage = entry.item.iSelectedImage = image;
        auto handle = TreeView_InsertItem(tree, &entry);
        nodes.emplace(handle, std::move(data));
        return handle;
    };
    if (!active().empty()) {
        auto texts = add(TVI_ROOT, L"Text rooms", {});
        auto voices = add(TVI_ROOT, L"Voice rooms", {});
        auto present = add(TVI_ROOT, L"Online", {});
        HTREEITEM selected = nullptr;
        std::unordered_map<std::string, HTREEITEM> rooms;
        for (const auto& r : arrayOr(state, "rooms")) {
            const auto kind = r.value("kind", ""), code = r.value("id", "");
            auto label = (kind == "text" ? L"# " : L"") + wide(r.value("name", ""));
            if (state.value("voice", "") == code) label += L"  ✓";
            auto item = add(kind == "text" ? texts : voices, std::move(label), {kind, code});
            rooms.emplace(code, item);
            if (code == currentText) selected = item;
        }
        for (const auto& member : arrayOr(state, "users")) {
            const auto memberID = member.value("id", "");
            auto label = wide(member.value("name", ""));
            if (memberID == state.value("owner", "")) label += L"  · owner";
            if (memberID == id) label += L"  (you)";
            else if (mutedParticipants.contains(peerKey(member))) label += L"  · muted";
            auto voice = rooms.find(member.value("voice", ""));
            auto parent = voice == rooms.end() ? present : voice->second;
            add(parent, std::move(label), {"user", memberID}, std::clamp(member.value("color", 0), 0, 15));
        }
        for (const auto& [_, node] : rooms) TreeView_Expand(tree, node, TVE_EXPAND);
        for (auto root : {texts, voices, present}) TreeView_Expand(tree, root, TVE_EXPAND);
        if (selected) TreeView_SelectItem(tree, selected);
    }
    SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree, nullptr, TRUE);
    InvalidateRect(window, nullptr, FALSE);
    rebuilding = false;
}

void App::refreshChat() {
    SendMessageW(transcript, WM_SETREDRAW, FALSE, 0);
    SendMessageW(transcript, EM_SETREADONLY, FALSE, 0);
    putText(transcript, L"");
    displayedDay = 0;
    auto found = chat.find(currentText);
    if (found != chat.end()) for (const auto& message : found->second) appendChatView(transcript, message, displayedDay);
    SendMessageW(transcript, EM_SETREADONLY, TRUE, 0);
    SendMessageW(transcript, WM_SETREDRAW, TRUE, 0);
    SendMessageW(transcript, EM_SCROLLCARET, 0, 0);
    InvalidateRect(transcript, nullptr, TRUE);
}

void App::refreshActions() {
    const bool joined = !active().empty();
    EnableWindow(composer, joined && !currentText.empty());
    ShowWindow(transcript, joined && !currentText.empty() ? SW_SHOWNA : SW_HIDE);
    EnableWindow(newServer, directHosting ? !hostStarting && !joiningGuest : connected);
    EnableWindow(join, directHosting ? !joiningGuest : connected);
    putText(muteButton, audio.muted ? L"Unmute microphone" : L"Mute microphone");
    InvalidateRect(muteButton, nullptr, FALSE);
    InvalidateRect(window, nullptr, FALSE);
}

Json App::voicePeer(const std::string& session) const {
    const auto channel = state.value("voice", "");
    if (session.empty() || session == id || channel.empty()) return Json::object();
    for (const auto& member : arrayOr(state, "users")) {
        if (member.value("id", "") == session && member.value("voice", "") == channel) return member;
    }
    return Json::object();
}

std::string App::participantRoom(const std::string& session) const {
    if (session.empty() || session == id) return {};
    for (const auto& member : arrayOr(state, "users")) {
        if (member.value("id", "") == session) return member.value("voice", "");
    }
    return {};
}

std::string App::legacyPeerKey(const Json& member) const {
    auto label = wide(member.value("name", ""));
    if (!label.empty()) {
        const auto n = static_cast<int>(label.size());
        auto count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, label.c_str(), n, nullptr, 0, nullptr, nullptr, 0);
        if (count > 0) {
            std::wstring lower(count, L'\0');
            if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, label.c_str(), n,
                lower.data(), count, nullptr, nullptr, 0)) label = std::move(lower);
        }
    }
    return "name:" + active() + ":" + utf8(label);
}

std::string App::peerKey(const Json& member) const {
    const auto profile = member.value("profile_id", "");
    return validCode(profile, 32) ? "profile:" + profile : legacyPeerKey(member);
}

int App::participantVolume(const Json& member) const {
    auto key = peerKey(member);
    if (key == previewPeer) return previewVolume;
    auto found = savedVolumes.find(key);
    if (found == savedVolumes.end() && key.starts_with("profile:")) found = savedVolumes.find(legacyPeerKey(member));
    return found == savedVolumes.end() ? 100 : found->second;
}

void App::updateParticipantVolumes() {
    Voice::Volumes current;
    if (!savedVolumes.empty() || !mutedParticipants.empty() || !previewPeer.empty()) {
        const auto channel = state.value("voice", "");
        if (!channel.empty()) {
            for (const auto& member : arrayOr(state, "users")) {
                const auto session = member.value("id", "");
                if (session != id && member.value("voice", "") == channel) {
                    const auto volume = mutedParticipants.contains(peerKey(member)) ? 0 : participantVolume(member);
                    if (volume != 100) current.emplace(session, volume);
                }
            }
        }
    }
    audio.setParticipantVolumes(std::move(current));
}

void App::promptParticipantVolume(const std::string& session) {
    const auto member = voicePeer(session);
    if (member.empty()) return;
    const auto username = wide(member.value("name", ""));
    const auto key = peerKey(member);
    const auto legacy = legacyPeerKey(member);
    const int original = participantVolume(member);
    previewPeer = key;
    previewVolume = original;
    std::vector<Field> fields{{Field::Slider, L"Volume for " + username, L"", {}, original}};
    fields[0].sliderScale = {maxParticipantGain, fivePercentSteps};
    fields[0].sliderPageStep = 1;
    fields[0].onChange = [this](int value) {
        previewVolume = value;
        updateParticipantVolumes();
    };
    const bool confirmed = form(window, L"Participant volume", fields);
    if (confirmed) {
        savedVolumes.erase(legacy);
        const int value = fields[0].selected;
        if (value != 100) savedVolumes[key] = value;
        else savedVolumes.erase(key);
        statusText(username + L" volume: " + percentText(value));
        saveVoicePreferences();
    }
    previewPeer.clear();
    updateParticipantVolumes();
}

void App::appendChat(const Json& event) {
    auto code = event.value("code", ""), room = event.value("room", "");
    if (!validCode(code) || !validCode(room, 12)) return;
    ChatMessage message{event.value("at", ""), event.value("sender", ""), event.value("text", "")};
    if (archiveChat || (retainOwned && owned.contains(code))) {
        auto path = directory / L"logs" / wide(code + "-" + room + ".txt");
        std::string title = event.value("name", "Room") + "   [" + code + "]\r\n\r\n";
        logs.emplace_back(path, title + '\0' + message.exportLine());
    }
    if (code != active()) return;
    auto& messages = chat[room];
    bool trim = false;
    chatBytes += message.size();
    messages.push_back(std::move(message));
    while (messages.size() > 1000 || (chatBytes > 2 * 1024 * 1024 && messages.size() > 1)) {
        chatBytes -= messages.front().size();
        messages.pop_front();
        trim = true;
    }
    if (room != currentText) return;
    if (trim) {
        refreshChat();
    } else {
        SendMessageW(transcript, EM_SETREADONLY, FALSE, 0);
        appendChatView(transcript, messages.back(), displayedDay);
        SendMessageW(transcript, EM_SETREADONLY, TRUE, 0);
        SendMessageW(transcript, EM_SCROLLCARET, 0, 0);
    }
}

void App::flushLogs() {
    while (!logs.empty()) {
        auto [file, body] = std::move(logs.front());
        logs.pop_front();
        auto marker = body.find('\0');
        std::error_code ec;
        auto fresh = !fs::exists(file, ec);
        std::ofstream writer(file, std::ios::binary | std::ios::app);
        if (fresh) writer.write(body.data(), static_cast<std::streamsize>(marker));
        writer.write(body.data() + marker + 1, static_cast<std::streamsize>(body.size() - marker - 1));
        writer.close();
        if (!writer) statusText(L"The local chat log could not be saved.");
    }
}

void App::handleEvent(const Json& e, Network* source) {
    auto type = e.value("type", "");
    const auto code = e.value("code", "");
    if (type == "load") {
        if (auto item = owned.find(code); item != owned.end()) {
            auto& src = item->second;
            auto rooms = src.rooms.is_array() ? src.rooms : loadJson(directory / L"servers" / wide(code + ".json"));
            if (rooms.is_array() && source) source->send({{"op", "hydrate"}, {"code", code}, {"rooms", std::move(rooms)}});
        }
    } else if (type == "chat") {
        appendChat(e);
    } else if (type == "closed" || type == "kicked") {
        if (type == "closed") available.erase(code);
        refreshServers();
        if (type == "closed") statusText(L"The owner closed the server.");
        else statusText(L"The owner removed you from the server.");
    } else if (type == "voice_kicked" && code == active()) {
        statusText(L"The owner removed you from the voice room.");
    }
}

void App::onNetwork(NetworkResult result) {
    if (closing) return;
    if (parkedHost && result.generation == parkedGeneration) {
        parkedHostResult(std::move(result));
        return;
    }
    if (result.generation != generation) return;
    auto& response = result.reply;
    auto& request = result.request;
    const auto op = request.value("op", "");
    if (directHosting && !onHost && guest && op == "poll") {
        if (response.value("network", false) && !guestOfflineSince) guestOfflineSince = GetTickCount64();
        if (response.value("restored", false)) guestOfflineSince = 0;
    }
    if (op == "register") registering = false;
    if (response.contains("error")) {
        statusText(wide(response.value("error", "")));
        if (op == "join") pendingJoin.clear();
        if (response.value("expired", false) && connected) {
            connected = false;
            version = 0;
            applyState(Json::object());
            available.clear();
            refreshServers();
            refreshActions();
            if (guest && !guestInvite.empty()) afterRegistration = guestInvite.substr(guestInvite.size() - 16);
            registerClient();
        }
        if (op == "chat" && windowText(composer).empty() && request.value("code", "") == active() &&
            request.value("room", "") == currentText) putText(composer, wide(request.value("text", "")));
        return;
    }
    if (op == "register") {
        if (!response.contains("key")) return;
        connected = true;
        id = response.at("user").value("id", "");
        if (!directHosting || onHost) for (const auto& [_, item] : owned) advertise(item);
        catalog();
        refreshActions();
        if (onHost) statusText(hostInternet ? L"TinySpeak is hosting your servers." :
            L"Hosting locally. Automatic access from the internet is unavailable on this network.");
        else statusText(L"Connected to the host.");
        if (onHost && !hostJoin.empty()) submitJoin(std::exchange(hostJoin, {}));
        if (!afterRegistration.empty()) submitJoin(std::exchange(afterRegistration, {}));
        if (!initialInvite.empty()) {
            requestJoin(initialInvite);
            initialInvite.clear();
        }
        return;
    }
    const auto replyVersion = response.value("version", std::uint64_t{0});
    if (replyVersion >= version) {
        version = replyVersion;
        if (response.contains("state")) applyState(response.at("state"));
        else if (response.value("reset", false)) applyState(Json::object());
    }
    for (const auto& e : arrayOr(response, "events")) handleEvent(e, network.get());
    if (response.value("restored", false)) statusText(L"Connection to the relay restored.");
    if (op == "catalog") {
        std::map<std::string, std::string> latest;
        for (const auto& item : arrayOr(response, "catalog")) latest[item.value("code", "")] = item.value("name", "");
        if (latest != available) {
            available = std::move(latest);
            refreshServers();
        }
    } else if (op == "join" && request.value("code", "") == pendingJoin) {
        if (response.value("retry", false)) {
            if (GetTickCount64() < joinUntil) retryAt = GetTickCount64() + 250;
            else {
                pendingJoin.clear();
                statusText(L"The server did not load its rooms. Try again.");
            }
        } else {
            pendingJoin.clear();
            if (remember && !owned.contains(active())) {
                if (bookmarks.size() >= 512 && !bookmarks.contains(active())) {
                    bookmarkInvites.erase(bookmarks.begin()->first);
                    bookmarks.erase(bookmarks.begin());
                }
                bookmarks[active()] = state.value("name", "");
                if (directHosting && !guestInvite.empty()) {
                    bookmarkInvites[active()] = guestInvite;
                    available[active()] = bookmarks[active()];
                }
                save();
            }
            statusText(L"Connected to " + wide(state.value("name", "")) +
                (onHost && !hostInternet ? L" · Local network only" : L""));
            catalog();
        }
    } else if (op == "profile" && response.contains("user")) {
        name = response.at("user").value("name", name);
        color = response.at("user").value("color", color);
        if (parkedHost) parkedHost->send({{"op", "profile"}, {"name", name}, {"color", color}});
        save();
    } else if (op == "delete") {
        const auto code = request.value("code", "");
        owned.erase(code);
        available.erase(code);
        std::error_code ec;
        fs::remove(directory / L"servers" / wide(code + ".json"), ec);
        save();
        refreshServers();
        statusText(L"Server deleted.");
    } else if (op == "voice" && response.contains("token")) {
        if (request.value("code", "") == active() && request.value("room", "") == state.value("voice", "")) {
            currentVoice = request.value("room", "");
            const auto url = directHosting && !onHost && guestAddress.starts_with(L"http://") ?
                "ws://" + utf8(guestAddress.substr(7)) : response.value("url", "");
            VoiceRelay relay;
            if (directHosting && !onHost && guestTurnPort > 0 && guestTurnPort <= 65535) {
                relay = {guestTurnHost + ":" + std::to_string(guestTurnPort), response.value("turnUser", ""), response.value("turnPassword", "")};
            }
            audio.connect(url, response.value("token", ""), microphone, playback, std::move(relay));
        }
    } else if (op == "leave") {
        statusText(L"You left the server.");
    }
}

void App::requestJoin(const std::string& code) {
    if (code.empty()) return;
    if (directHosting) {
        if (owned.contains(code)) {
            ensureHost(code);
            return;
        }
        if (!hostedInvite(code)) {
            const auto saved = bookmarkInvites.find(code);
            if (saved != bookmarkInvites.end()) requestJoin(saved->second);
            else statusText(L"Ask the owner for a current TinySpeak host invite.");
            return;
        }
        if (code == guestInvite && connected) {
            submitJoin(code.substr(code.size() - 16));
            return;
        }
        auto candidate = std::make_unique<HostProcess>();
        if (!candidate->start(L"join " + wide(code))) {
            statusText(L"Could not start the TinySpeak connection helper.");
            return;
        }
        candidateInvite = code;
        joiningGuest = std::move(candidate);
        statusText(L"Looking for the TinySpeak host…");
        refreshActions();
        return;
    }
    if (hostedInvite(code)) {
        statusText(L"This copy is configured to use a shared relay.");
        return;
    }
    submitJoin(code);
}

void App::submitJoin(const std::string& code) {
    pendingJoin = code;
    retryAt = 0;
    joinUntil = GetTickCount64() + 12000;
    sendRequest({{"op", "join"}, {"code", code}});
    statusText(L"Opening server…");
}

void App::promptJoin(const std::wstring& value) {
    std::vector<Field> fields{{Field::Text, L"Invite code or link", value}};
    if (!form(window, L"Join a server", fields)) return;
    const auto code = parseCode(fields.front().text);
    if (code.empty()) statusText(L"Paste the complete TinySpeak invite code or link.");
    else requestJoin(code);
}

void App::promptCreate() {
    std::vector<Field> fields{{Field::Text, L"Server name", L""}};
    if (!form(window, L"New server", fields)) return;
    auto title = fields.front().text;
    auto start = title.find_first_not_of(L" \t");
    if (start == std::wstring::npos || title.size() > 64) {
        statusText(L"Server names must be 1–64 characters.");
        return;
    }
    title = title.substr(start, title.find_last_not_of(L" \t") - start + 1);
    auto secret = randomCode(40);
    auto code = inviteCode(secret);
    auto rooms = Json::array({
        {{"id", randomCode(12)}, {"kind", "text"}, {"name", "general"}},
        {{"id", randomCode(12)}, {"kind", "voice"}, {"name", "Lounge"}}
    });
    auto& item = owned.emplace(code, Owned{code, utf8(title), std::move(secret), std::move(rooms)}).first->second;
    if (!directHosting || (onHost && connected)) advertise(item);
    if (retainOwned) save();
    refreshServers();
    requestJoin(code);
}

void App::promptProfile() {
    std::vector<Field> fields{{Field::Text, L"Username (4–32 characters)", wide(name)},
        {Field::Choice, L"Profile color", L"", colorNames, color}};
    fields[1].swatches = theme::profile.data();
    if (!form(window, L"Username and color", fields)) return;
    auto label = fields[0].text;
    const auto start = label.find_first_not_of(L" 	\r\n");
    if (start != std::wstring::npos) label = label.substr(start, label.find_last_not_of(L" 	\r\n") - start + 1);
    const auto length = std::ranges::count_if(label, [](wchar_t c) { return !(c >= 0xDC00 && c <= 0xDFFF); });
    if (length < 4 || length > 32 || std::ranges::any_of(label, [](wchar_t c) { return c < 32 || (c >= 127 && c <= 159); })) {
        statusText(L"Usernames must be 4–32 characters.");
        return;
    }
    if (connected) {
        sendRequest({{"op", "profile"}, {"name", utf8(label)}, {"color", fields[1].selected}});
    } else {
        name = utf8(label);
        color = fields[1].selected;
        save();
    }
}

void App::promptPlayback(HWND parent) {
    const auto original = audio.equalizerDb.load();
    const bool originalNormalize = audio.normalizePlayback.load();
    constexpr std::array labels{L"1–200 Hz", L"201–400 Hz", L"401–1,000 Hz", L"1,001–20,000 Hz"};
    std::vector<Field> fields;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        Field field{Field::Slider, labels[i], L"", {}, original[i]};
        field.sliderScale = {equalizerLimitDb, false, -equalizerLimitDb};
        field.sliderSuffix = L" dB";
        field.sliderPageStep = 1;
        field.onChange = [this, i](int value) {
            auto gains = audio.equalizerDb.load();
            gains[i] = static_cast<std::int8_t>(value);
            audio.equalizerDb = gains;
        };
        fields.push_back(std::move(field));
    }
    fields.push_back({Field::Check, L"Normalize incoming voice (limit sudden peaks)", L"", {}, originalNormalize});
    fields.back().onChange = [this](int checked) { audio.normalizePlayback = checked != 0; };
    if (!form(parent, L"Playback equalizer", fields)) {
        audio.equalizerDb = original;
        audio.normalizePlayback = originalNormalize;
        return;
    }
    EqualizerGains gains{};
    for (std::size_t i = 0; i < gains.size(); ++i) gains[i] = static_cast<std::int8_t>(fields[i].selected);
    audio.equalizerDb = gains;
    audio.normalizePlayback = fields.back().selected != 0;
}

void App::promptSettings() {
    enum SettingField { RelayAddress, InputDevice, OutputDevice, PlaybackEffectsField, InputMode, Threshold, TalkKey, Bitrate, FivePercentSteps,
        RememberServers, RememberColor, ArchiveChat, RetainOwned, RegisterLinks };
    auto [mics, speakers] = audio.devices();
    auto deviceField = [](const wchar_t* label, const std::vector<AudioDevice>& devices, const std::wstring& current) {
        Field field{Field::Choice, label, L"", {L"Default Windows device"}};
        for (const auto& device : devices) {
            field.choices.push_back(device.name);
            if (device.id == current) field.selected = static_cast<int>(field.choices.size()) - 1;
        }
        return field;
    };
    const int originalThreshold = audio.threshold.load();
    const int originalBitrate = audio.bitrateKbps.load();
    std::vector<std::wstring> bitrates;
    for (int value : voiceBitrates) {
        bitrates.push_back(std::to_wstring(value) + (value == defaultVoiceBitrate ? L" kbps - normal quality" : L" kbps"));
    }
    const int selectedBitrate = static_cast<int>(std::find(voiceBitrates.begin(), voiceBitrates.end(), originalBitrate) - voiceBitrates.begin());
    std::vector<Field> fields{
        {Field::Text, L"Relay address", address},
        deviceField(L"Microphone", mics, microphone),
        deviceField(L"Headphones or speakers", speakers, playback),
        {Field::Action, L"Playback equalizer and normalization…"},
        {Field::Choice, L"Microphone mode", L"", {L"Voice activation", L"Push-to-talk", L"Open microphone"}, audio.mode.load()},
        {Field::Slider, L"Microphone threshold (relative dB, 0 = off)", L"", {}, originalThreshold},
        {Field::Choice, L"Push-to-talk key", L"", {L"F6", L"F7", L"F8", L"F9", L"F10", L"F11", L"F12"}, pushKey - VK_F6},
        {Field::Choice, L"Microphone bitrate", L"", std::move(bitrates), selectedBitrate},
        {Field::Check, L"5% volume steps (1% from 0% to 5%)", L"", {}, fivePercentSteps},
        {Field::Check, L"Remember servers I join", L"", {}, remember},
        {Field::Check, L"Remember my color", L"", {}, rememberColor},
        {Field::Check, L"Save my chat logs on this computer", L"", {}, archiveChat},
        {Field::Check, L"Save my servers, rooms and chat logs", L"", {}, retainOwned},
        {Field::Check, L"Open tinyspeak:// invites in Windows", L"", {}, 0},
    };
    fields[Threshold].sliderScale.maximum = microphoneThresholdMax;
    fields[RelayAddress].visible = !directHosting;
    fields[PlaybackEffectsField].onClick = [this](HWND parent) { promptPlayback(parent); };
    fields[Threshold].sliderSuffix = L" dB";
    fields[Threshold].onChange = [this](int value) { audio.threshold = value; };
    if (!form(window, L"Settings", fields)) {
        audio.threshold = originalThreshold;
        audio.equalizerDb = savedEqualizer;
        audio.normalizePlayback = savedNormalization;
        return;
    }
    savedEqualizer = audio.equalizerDb.load();
    savedNormalization = audio.normalizePlayback.load();
    audio.threshold = fields[Threshold].selected;
    auto selectDevice = [](const std::vector<AudioDevice>& devices, int index) {
        if (index > 0 && static_cast<std::size_t>(index) <= devices.size()) return devices[index - 1].id;
        return std::wstring{};
    };
    auto before = address;
    if (!directHosting) address = fields[RelayAddress].text;
    const auto prevMic = microphone, prevPlayback = playback;
    microphone = selectDevice(mics, fields[InputDevice].selected);
    playback = selectDevice(speakers, fields[OutputDevice].selected);
    if (fields[InputMode].selected >= 0) audio.mode = fields[InputMode].selected;
    if (fields[TalkKey].selected >= 0) pushKey = VK_F6 + fields[TalkKey].selected;
    if (const int bitrate = fields[Bitrate].selected; bitrate >= 0 && static_cast<std::size_t>(bitrate) < voiceBitrates.size()) {
        audio.bitrateKbps = voiceBitrates[bitrate];
    }
    if (const bool stepped = fields[FivePercentSteps].selected != 0; stepped != fivePercentSteps) {
        fivePercentSteps = stepped;
        configureVolumeSliders();
    }
    remember = fields[RememberServers].selected != 0;
    rememberColor = fields[RememberColor].selected != 0;
    archiveChat = fields[ArchiveChat].selected != 0;
    const bool previousSave = retainOwned;
    retainOwned = fields[RetainOwned].selected != 0;
    if (previousSave && !retainOwned) {
        std::error_code ec;
        for (auto& [code, item] : owned) {
            auto path = directory / L"servers" / wide(code + ".json");
            if (!item.rooms.is_array()) item.rooms = loadJson(path);
            fs::remove(path, ec);
        }
    }
    if (!remember) {
        bookmarks.clear();
        bookmarkInvites.clear();
        if (directHosting) available.clear();
        refreshServers();
    }
    save();
    if (fields[RegisterLinks].selected) {
        statusText(registerInviteLink() ? L"Invite links can now open in TinySpeak." : L"Could not register TinySpeak invite links.");
    }
    if (address != before) {
        startNetwork(address);
    } else {
        if (retainOwned != previousSave) {
            for (const auto& [_, item] : owned) {
                if (parkedHost) parkedHost->send({{"op", "advertise"}, {"code", item.code}, {"name", item.name},
                    {"secret", item.secret}, {"archive", retainOwned}});
                else if (!directHosting || onHost) advertise(item);
            }
        }
        if (!currentVoice.empty() && (microphone != prevMic || playback != prevPlayback || audio.bitrateKbps != originalBitrate)) {
            voiceRequest(currentVoice);
        }
        catalog();
    }
}

void App::serverPicker() {
    const auto visible = serverRows;
    std::vector<MenuEntry> entries;
    for (std::size_t i = 0; i < visible.size(); ++i) {
        const auto& [code, label] = visible[i];
        entries.push_back({serverMenuBase + static_cast<unsigned>(i), wide(label + "  ·  " + code), directHosting || connected});
    }
    if (entries.empty()) entries.push_back({0, L"No servers available", false});
    const auto pick = showMenu(window, servers, std::move(entries), false);
    if (pick >= serverMenuBase && pick - serverMenuBase < visible.size()) requestJoin(visible[pick - serverMenuBase].first);
}

Node App::selectedNode() const {
    auto found = nodes.find(TreeView_GetSelection(tree));
    return found != nodes.end() ? found->second : Node{};
}

void App::popupActions() {
    const bool joined = !active().empty();
    const bool inChat = joined && !currentText.empty();
    const auto selected = selectedNode();
    const bool canManage = joined && owner();
    const auto pick = showMenu(window, actions, {
        {Profile, L"Username and color…", directHosting || connected},
        {Settings, L"Settings…"},
        {Archive, L"Local chat logs…"},
        {0, L"", false, true},
        {Send, L"Send message  ·  Enter", inChat && GetWindowTextLengthW(composer) > 0},
        {Export, L"Export chat as TXT…", inChat},
        {Mic, audio.muted ? L"Unmute microphone" : L"Mute microphone"},
        {ParticipantVolume, L"Participant volume…", selected.kind == "user" && !voicePeer(selected.id).empty()},
        {LeaveVoice, L"Leave voice room", !state.value("voice", "").empty()},
        {0, L"", false, true},
        {InviteCode, L"Copy invite code", joined},
        {Invite, L"Copy invite link", joined},
        {LeaveServer, L"Leave server", joined},
        {0, L"", false, true},
        {AddTextRoom, L"New text room…", canManage},
        {AddVoiceRoom, L"New voice room…", canManage},
        {DeleteRoom, L"Delete selected room…", canManage && (selected.kind == "text" || selected.kind == "voice")},
        {Kick, L"Remove selected user", canManage && selected.kind == "user" && selected.id != id},
        {DeleteServer, L"Delete server…", canManage},
    }, true);
    if (pick) command(static_cast<int>(pick), 0);
}

void App::addRoom(bool text) {
    if (!owner()) return;
    const auto code = active();
    std::vector<Field> fields{{Field::Text, L"Room name", L""}};
    if (form(window, text ? L"New text room" : L"New voice room", fields)) {
        sendRequest({{"op", "room_add"}, {"code", code}, {"kind", text ? "text" : "voice"},
            {"name", utf8(fields.front().text)}});
    }
}

void App::exportLog(const std::string& text, const std::wstring& filename) {
    wchar_t file[MAX_PATH]{};
    wcsncpy_s(file, filename.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"Text file (*.txt)\0*.txt\0\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&dialog)) {
        statusText(writeText(file, "\xef\xbb\xbf" + text) ? L"TXT file saved." : L"The TXT file could not be saved.");
    }
}

void App::localLogs() {
    flushLogs();
    std::vector<fs::path> files;
    std::vector<std::wstring> names;
    std::error_code ec;
    const fs::directory_iterator end;
    for (fs::directory_iterator it(directory / L"logs", ec); !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != L".txt" || !it->is_regular_file(ec)) continue;
        std::ifstream input(it->path(), std::ios::binary);
        std::string title;
        if (!std::getline(input, title)) continue;
        if (!title.empty() && title.back() == '\r') title.pop_back();
        files.push_back(it->path());
        names.push_back(wide(title));
    }
    if (files.empty()) {
        statusText(L"There are no local chat logs.");
        return;
    }
    std::vector<Field> fields{{Field::Choice, L"Chat log", L"", names},
        {Field::Choice, L"Action", L"", {L"Read", L"Export as TXT", L"Delete"}}};
    if (!form(window, L"Local chat logs", fields) || fields[0].selected < 0) return;
    const auto& file = files[fields[0].selected];
    if (fields[1].selected == 2) {
        if (MessageBoxW(window, L"Permanently delete this local log?", L"TinySpeak", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            std::error_code removeError;
            fs::remove(file, removeError);
            statusText(removeError ? L"The log could not be deleted." : L"Local log deleted.");
        }
        return;
    }
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        statusText(L"The log is unavailable.");
        return;
    }
    if (fields[1].selected == 1) {
        wchar_t output[MAX_PATH] = L"TinySpeak-chat.txt";
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = window;
        dialog.lpstrFilter = L"Text file (*.txt)\0*.txt\0\0";
        dialog.lpstrFile = output;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrDefExt = L"txt";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&dialog)) {
            std::error_code copyError;
            if (file != fs::path(output)) fs::copy_file(file, output, fs::copy_options::overwrite_existing, copyError);
            statusText(copyError ? L"The TXT file could not be saved." : L"TXT file saved.");
        }
    } else {
        // The archive can grow on disk. Display its most recent 256 KiB at a time.
        input.seekg(0, std::ios::end);
        const auto size = static_cast<std::streamoff>(input.tellg());
        const auto limit = std::streamoff{256 * 1024};
        input.seekg(size > limit ? size - limit : std::streamoff{0}, std::ios::beg);
        if (size > limit) input.ignore(limit, '\n');
        const std::string text{std::istreambuf_iterator<char>(input), {}};
        readOnlyWindow(window, L"Local chat log", wide(text));
    }
}

void App::textSend() {
    auto text = utf8(windowText(composer));
    if (currentText.empty() || text.empty()) return;
    sendRequest({{"op", "chat"}, {"code", active()}, {"room", currentText}, {"text", std::move(text)}});
    putText(composer, L"");
}

void App::voiceRequest(const std::string& code) {
    sendRequest({{"op", "voice"}, {"code", active()}, {"room", code}});
}

void App::selectTree() {
    if (rebuilding) return;
    auto entry = nodes.find(TreeView_GetSelection(tree));
    if (entry != nodes.end() && entry->second.kind == "text" && entry->second.id != currentText) {
        textClosed = false;
        currentText = entry->second.id;
        refreshChat();
        refreshActions();
    }
}

void App::treeMenu(const Node& node, const POINT& screen) {
    std::vector<MenuEntry> entries;
    const auto code = active();
    const auto voice = state.value("voice", "");
    const auto member = node.kind == "user" ? voicePeer(node.id) : Json::object();
    const auto peer = member.empty() ? std::string{} : peerKey(member);
    const auto targetRoom = node.kind == "user" ? participantRoom(node.id) : std::string{};
    if (node.kind == "voice" || node.kind == "text") {
        if (node.kind == "voice" && node.id == voice) entries.push_back({LeaveVoice, L"Leave voice room"});
        if (node.kind == "text" && node.id == currentText) entries.push_back({LeaveText, L"Leave text channel"});
        if (owner()) entries.push_back({RenameRoom, node.kind == "voice" ? L"Rename room…" : L"Rename channel…"});
    } else if (node.kind == "user") {
        if (!peer.empty()) {
            entries.push_back({MuteParticipant, mutedParticipants.contains(peer) ? L"Unmute for me" : L"Mute for me"});
            entries.push_back({ParticipantVolume, L"Participant volume…  " + percentText(participantVolume(member))});
        }
        if (owner() && !targetRoom.empty()) entries.push_back({KickVoice, L"Remove from voice room"});
    }
    if (entries.empty()) return;
    const auto action = showMenu(window, tree, std::move(entries), false, &screen);
    if (active() != code) return;
    switch (action) {
    case LeaveVoice:
        if (state.value("voice", "") == node.id) command(LeaveVoice, 0);
        break;
    case LeaveText:
        if (currentText == node.id) {
            textClosed = true;
            currentText.clear();
            TreeView_SelectItem(tree, nullptr);
            refreshChat();
            refreshActions();
        }
        break;
    case RenameRoom: {
        if (!owner()) break;
        std::vector<Field> fields{{Field::Text, L"Name", roomName(node.id)}};
        if (form(window, L"Rename", fields) && active() == code) {
            sendRequest({{"op", "room_rename"}, {"code", code}, {"room", node.id}, {"name", utf8(fields.front().text)}});
        }
        break;
    }
    case MuteParticipant:
        if (!peer.empty() && !voicePeer(node.id).empty()) {
            if (!mutedParticipants.erase(peer)) mutedParticipants.insert(peer);
            updateParticipantVolumes();
            refreshTree();
            saveVoicePreferences();
        }
        break;
    case ParticipantVolume:
        promptParticipantVolume(node.id);
        break;
    case KickVoice:
        if (owner() && !targetRoom.empty()) {
            sendRequest({{"op", "voice_kick"}, {"code", code}, {"room", targetRoom}, {"target", node.id}});
        }
        break;
    }
}

LRESULT App::onNotify(NMHDR* note) {
    if (note->code == NM_CUSTOMDRAW && (note->hwndFrom == micSlider || note->hwndFrom == outputSlider)) {
        return drawTrackbar(*reinterpret_cast<NMCUSTOMDRAW*>(note), theme::sidebar);
    }
    if (note->hwndFrom == tree) {
        if (note->code == NM_CUSTOMDRAW) {
            auto& item = *reinterpret_cast<NMTVCUSTOMDRAW*>(note);
            if (item.nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (item.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                const bool selected = (item.nmcd.uItemState & CDIS_SELECTED) != 0;
                item.nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS | CDIS_HOT);
                item.clrText = theme::text;
                item.clrTextBk = selected ? theme::selected : theme::sidebar;
                return CDRF_NEWFONT;
            }
        }
        if (note->code == TVN_SELCHANGEDW) selectTree();
        if (note->code == NM_RCLICK) {
            const DWORD cursor = GetMessagePos();
            const POINT screen{GET_X_LPARAM(cursor), GET_Y_LPARAM(cursor)};
            TVHITTESTINFO hit{};
            hit.pt = screen;
            ScreenToClient(tree, &hit.pt);
            const auto node = nodes.find(TreeView_HitTest(tree, &hit));
            if (node != nodes.end()) {
                const auto selected = node->second;
                treeMenu(selected, screen);
                return TRUE;
            }
        }
        const bool activated = note->code == NM_DBLCLK ||
            (note->code == TVN_KEYDOWN && reinterpret_cast<NMTVKEYDOWN*>(note)->wVKey == VK_RETURN);
        if (activated) {
            const auto node = selectedNode();
            if (node.kind == "voice") voiceRequest(node.id);
            if (node.kind == "user") promptParticipantVolume(node.id);
        }
    }
    return 0;
}

void App::command(int which, int notification) {
    if ((which == ServerList || which == NewServer || which == Join || which == Actions) && notification != BN_CLICKED) return;
    switch (which) {
    case ServerList: serverPicker(); break;
    case NewServer: promptCreate(); break;
    case Join: promptJoin(); break;
    case Actions: popupActions(); break;
    case Profile: promptProfile(); break;
    case Settings: promptSettings(); break;
    case Archive: localLogs(); break;
    case Send: textSend(); break;
    case Invite:
    case InviteCode:
        if (const auto code = activeInvite(); !code.empty()) {
            clipboard(window, (which == Invite ? L"tinyspeak://join/" : L"") + wide(code));
            auto message = which == Invite ? std::wstring{L"Invite link copied."} : std::wstring{L"Invite code copied."};
            if (onHost && !hostInternet) message += L" The invite is currently only reachable on your local network.";
            statusText(std::move(message));
        }
        break;
    case LeaveServer:
        if (!active().empty()) sendRequest({{"op", "leave"}});
        break;
    case DeleteServer: {
        const auto code = active();
        if (owner() && MessageBoxW(window, L"Delete this server and its rooms?", L"TinySpeak", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            sendRequest({{"op", "delete"}, {"code", code}});
        }
        break;
    }
    case AddTextRoom: addRoom(true); break;
    case AddVoiceRoom: addRoom(false); break;
    case DeleteRoom: {
        const auto selected = selectedNode();
        const auto code = active();
        if (owner() && (selected.kind == "text" || selected.kind == "voice") &&
            MessageBoxW(window, L"Delete the selected room?", L"TinySpeak", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            sendRequest({{"op", "room_delete"}, {"code", code}, {"room", selected.id}});
        }
        break;
    }
    case Kick: {
        const auto selected = selectedNode();
        if (owner() && selected.kind == "user" && selected.id != id) {
            sendRequest({{"op", "kick"}, {"code", active()}, {"target", selected.id}});
        }
        break;
    }
    case Export: {
        std::string text;
        auto found = chat.find(currentText);
        if (found != chat.end()) for (const auto& entry : found->second) text += entry.exportLine();
        exportLog(text, L"TinySpeak-chat.txt");
        break;
    }
    case Mic:
        audio.muted = !audio.muted;
        refreshActions();
        statusText(audio.muted ? L"Microphone muted." : L"Microphone unmuted.");
        break;
    case ParticipantVolume: {
        const auto chosen = selectedNode();
        if (chosen.kind == "user") promptParticipantVolume(chosen.id);
        break;
    }
    case LeaveVoice:
        audio.disconnect();
        currentVoice.clear();
        sendRequest({{"op", "voice_leave"}, {"code", active()}});
        break;
    }
}

void App::statusAudio() {
    const int current = audio.status.load();
    const int level = audio.level.load();
    const bool talking = audio.talking.load();
    if (current != audioStatus) {
        audioStatus = current;
        if (current == 2) statusText(L"Connected to the voice room.");
        if (current == 3) {
            statusText(L"The voice connection or audio device is unavailable.");
            if (!currentVoice.empty()) sendRequest({{"op", "voice_leave"}, {"code", active()}});
        }
        if (current == 4) statusText(L"Listening only. No microphone is available.");
    }
    if (level != displayedLevel || talking != displayedTalk) {
        displayedLevel = level;
        displayedTalk = talking;
        redrawAudioPanel();
    }
}

void App::redrawAudioPanel() {
    RECT rect{};
    GetClientRect(window, &rect);
    rect.top = layoutFor(rect.right, rect.bottom, GetDpiForWindow(window)).footerTop;
    InvalidateRect(window, &rect, FALSE);
}

void App::configureVolumeSliders() {
    configureSlider(micSlider, {maxMicrophoneGain, fivePercentSteps}, audio.inputGain.load());
    configureSlider(outputSlider, {maxOutputGain, fivePercentSteps}, audio.outputGain.load());
    redrawAudioPanel();
}

void App::tick() {
    tickHelpers();
    chatSounds.tick(audio.outputGain.load());
    const unsigned interval = currentVoice.empty() ? 250 : 35;
    if (interval != timerInterval) {
        timerInterval = interval;
        SetTimer(window, uiTimer, timerInterval, nullptr);
    }
    const bool down = !currentVoice.empty() && audio.mode == 1 && (GetAsyncKeyState(pushKey) & 0x8000);
    if (down != pushWasDown) {
        pushWasDown = down;
        audio.push = down;
    }
    statusAudio();
    const auto now = GetTickCount64();
    if (retryAt && retryAt <= now) {
        retryAt = 0;
        if (!pendingJoin.empty()) sendRequest({{"op", "join"}, {"code", pendingJoin}});
    }
    if (now >= clockNext) {
        clockNext = now + 4000;
        flushLogs();
        catalog();
        if (!connected && now >= nextRegistration) registerClient();
    }
}

void App::close() {
    if (closing) return;
    closing = true;
    audio.muted = true;
    audio.disconnect();
    flushLogs();
    save();
    ShowWindow(window, SW_HIDE);
    if (!active().empty()) chatSounds.playExit(audio.outputGain.load());
    if (network) network->finish();
    parkedHost.reset();
    joiningGuest.reset();
    guest.reset();
    host.stop();
    probes.stop();
    MSG message{};
    while (PeekMessageW(&message, window, networkResult, networkResult, PM_REMOVE)) {
        delete reinterpret_cast<NetworkResult*>(message.lParam);
    }
    DestroyWindow(window);
}

LRESULT CALLBACK composerProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR app) {
    if (message == WM_KEYDOWN && wParam == VK_RETURN) {
        reinterpret_cast<App*>(app)->command(Send, BN_CLICKED);
        return 0;
    }
    if (message == WM_CHAR && wParam == L'\r') return 0;
    const auto result = DefSubclassProc(control, message, wParam, lParam);
    if (message == WM_PAINT && GetFocus() != control && !GetWindowTextLengthW(control)) {
        RECT area{};
        GetClientRect(control, &area);
        auto dc = GetDC(control);
        auto old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
        fillColor(dc, area, theme::field);
        drawTextLine(dc, area, L"Type a message · Enter to send",
            IsWindowEnabled(control) ? theme::secondary : theme::muted);
        SelectObject(dc, old);
        ReleaseDC(control, dc);
    }
    return result;
}

LRESULT CALLBACK transcriptProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEWHEEL && !(GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL)) {
        int rows = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &rows, 0);
        if (rows == -1) {
            SendMessageW(control, WM_VSCROLL, GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? SB_PAGEUP : SB_PAGEDOWN, 0);
        } else {
            const int steps = MulDiv(-GET_WHEEL_DELTA_WPARAM(wParam), rows, WHEEL_DELTA);
            if (steps) SendMessageW(control, EM_LINESCROLL, 0, steps);
        }
        return 0;
    }
    return DefSubclassProc(control, message, wParam, lParam);
}

void App::setup() {
    load();
    darkFrame(window);
    const int dpi = GetDpiForWindow(window);
    auto font = [&](int size, int weight) {
        return CreateFontW(-MulDiv(size, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    };
    regular = font(10, FW_NORMAL);
    bold = font(10, FW_SEMIBOLD);
    heading = font(15, FW_SEMIBOLD);
    background = CreateSolidBrush(theme::background);
    fieldBrush = CreateSolidBrush(theme::field);
    auto make = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id, DWORD ext = 0) {
        auto handle = control(window, type, text, style, id, ext);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(regular), TRUE);
        return handle;
    };
    auto button = [&](const wchar_t* title, int id) { return make(L"BUTTON", title, WS_TABSTOP | BS_OWNERDRAW, id); };
    servers = button(L"Select a server", ServerList);
    newServer = button(L"New server", NewServer);
    join = button(L"Join", Join);
    actions = button(L"Menu", Actions);
    tree = make(WC_TREEVIEWW, L"", WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
        TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP | TVS_NOHSCROLL, Tree);
    SetWindowTheme(tree, L"", L"");
    TreeView_SetBkColor(tree, theme::sidebar);
    TreeView_SetTextColor(tree, theme::text);
    TreeView_SetLineColor(tree, theme::edge);
    const int k = MulDiv(17, dpi, 96);
    icons = ImageList_Create(k, k, ILC_COLOR32, static_cast<int>(theme::profile.size()), 1);
    BITMAPINFO dib{};
    dib.bmiHeader.biSize = sizeof(dib.bmiHeader);
    dib.bmiHeader.biWidth = k;
    dib.bmiHeader.biHeight = -k;
    dib.bmiHeader.biPlanes = 1;
    dib.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &dib, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap) {
        auto* pixels = static_cast<std::uint32_t*>(bits);
        const auto pixel = [](COLORREF color) {
            return 0xff000000u | (GetRValue(color) << 16) | (GetGValue(color) << 8) | GetBValue(color);
        };
        const int radius = k - 4, inner = radius - 3;
        for (auto color : theme::profile) {
            const auto opaque = pixel(color);
            for (int y = 0; y < k; ++y) {
                for (int x = 0; x < k; ++x) {
                    const int dx = x * 2 + 1 - k, dy = y * 2 + 1 - k;
                    const int d = dx * dx + dy * dy;
                    pixels[y * k + x] = d <= inner * inner ? opaque : d <= radius * radius ? pixel(theme::secondary) : 0;
                }
            }
            ImageList_Add(icons, bitmap, nullptr);
        }
        DeleteObject(bitmap);
    }
    TreeView_SetImageList(tree, icons, TVSIL_NORMAL);
    transcript = make(MSFTEDIT_CLASS, L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_TABSTOP, Transcript);
    SendMessageW(transcript, EM_SETBKGNDCOLOR, 0, theme::background);
    SendMessageW(transcript, EM_EXLIMITTEXT, 0, 2 * 1024 * 1024);
    SendMessageW(transcript, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(transcript, EM_SETUNDOLIMIT, 0, 0);
    SetWindowSubclass(transcript, transcriptProc, 1, 0);
    composer = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, Message);
    SendMessageW(composer, EM_LIMITTEXT, 4000, 0);
    SendMessageW(composer, EM_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(L"Type a message · Enter to send"));
    SetWindowSubclass(composer, composerProc, 1, reinterpret_cast<DWORD_PTR>(this));
    muteButton = button(L"Unmute microphone", Mic);
    micSlider = make(TRACKBAR_CLASSW, L"Microphone volume", TBS_NOTICKS | WS_TABSTOP, MicSlider);
    outputSlider = make(TRACKBAR_CLASSW, L"Output volume", TBS_NOTICKS | WS_TABSTOP, OutputSlider);
    configureVolumeSliders();
    refreshServers();
    refreshActions();
    arrange();
    if (!directHosting) startNetwork(address);
    else {
        statusText(L"Create a server or enter an invite code.");
        if (!owned.empty()) ensureHost();
        if (!initialInvite.empty()) requestJoin(std::exchange(initialInvite, {}));
        probeBookmarks();
    }
    SetTimer(window, uiTimer, timerInterval, nullptr);
}

void App::arrange() {
    if (!tree) return;
    RECT bounds{};
    GetClientRect(window, &bounds);
    const int dpi = GetDpiForWindow(window);
    const auto layout = layoutFor(bounds.right, bounds.bottom, dpi);
    auto place = [](HWND control, const RECT& area) {
        MoveWindow(control, area.left, area.top, std::max<LONG>(1, area.right - area.left),
            std::max<LONG>(1, area.bottom - area.top), TRUE);
    };
    place(servers, layout.server);
    place(newServer, layout.create);
    place(join, layout.join);
    place(actions, layout.menu);
    place(tree, layout.rooms);
    place(transcript, layout.transcript);
    RECT edit = layout.composer;
    InflateRect(&edit, -MulDiv(10, dpi, 96), -MulDiv(8, dpi, 96));
    place(composer, edit);
    place(muteButton, layout.mute);
    place(micSlider, layout.input);
    place(outputSlider, layout.output);
    InvalidateRect(window, nullptr, FALSE);
}

void App::paint() {
    PAINTSTRUCT p{};
    HDC dc = BeginPaint(window, &p);
    RECT bounds{};
    GetClientRect(window, &bounds);
    const auto dpi = GetDpiForWindow(window);
    const auto l = layoutFor(bounds.right, bounds.bottom, dpi);
    auto text = [&](RECT box, const std::wstring& value, HFONT font, COLORREF color) {
        auto old = SelectObject(dc, font);
        drawTextLine(dc, box, value, color);
        SelectObject(dc, old);
    };
    FillRect(dc, &bounds, background);
    fillColor(dc, {0, 0, bounds.right, l.toolbarBottom}, theme::background);
    fillColor(dc, {0, l.toolbarBottom, l.sidebarRight, l.footerTop}, theme::sidebar);
    fillColor(dc, {0, l.toolbarBottom - 1, bounds.right, l.toolbarBottom}, theme::edge);
    fillColor(dc, {l.sidebarRight - 1, l.toolbarBottom, l.sidebarRight, l.footerTop}, theme::edge);
    text(l.brand, L"TinySpeak", bold, theme::text);
    text(l.listTitle, L"Rooms", bold, theme::secondary);
    text(l.chatTitle, currentText.empty() ? L"Chat" : L"#  " + roomName(currentText), heading, theme::text);
    if (active().empty()) {
        auto hint = l.transcript;
        hint.bottom = hint.top + MulDiv(29, dpi, 96);
        text(hint, L"Create a server or join one with an invite.", regular, theme::secondary);
    }
    fillColor(dc, l.composer, theme::edge);
    auto inside = l.composer;
    InflateRect(&inside, -1, -1);
    fillColor(dc, inside, theme::field);
    fillColor(dc, {0, l.footerTop, bounds.right, l.statusTop}, theme::sidebar);
    fillColor(dc, {0, l.footerTop, bounds.right, l.footerTop + 1}, theme::edge);
    text(l.microphone, L"Microphone  " + percentText(audio.inputGain.load()), regular, theme::text);
    text(l.outputTitle, L"Output  " + percentText(audio.outputGain.load()), regular, theme::text);
    const auto channel = roomName(state.value("voice", ""));
    const std::wstring voiceLabel = channel.empty() ? L"" : L"Voice  ·  " + channel;
    if (!voiceLabel.empty()) text(l.voice, voiceLabel, bold, theme::secondary);
    fillColor(dc, l.meter, theme::edge);
    RECT level = l.meter;
    level.right = level.left + (level.right - level.left) * std::clamp(displayedLevel, 0, 100) / 100;
    fillColor(dc, level, displayedTalk ? theme::white : theme::secondary);
    fillColor(dc, {0, l.statusTop, bounds.right, l.statusTop + 1}, theme::edge);
    auto label = status;
    if (state.value("archive", false)) label += L"   ·   Owner saves a local chat log";
    text(l.status, label, regular, theme::secondary);
    EndPaint(window, &p);
}

LRESULT CALLBACK procedure(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_CREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->setup();
        return 0;
    }
    if (!app) return DefWindowProcW(window, msg, wParam, lParam);
    switch (msg) {
    case WM_SIZE: if (wParam != SIZE_MINIMIZED) app->arrange(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: app->paint(); return 0;
    case WM_COMMAND: app->command(LOWORD(wParam), HIWORD(wParam)); return 0;
    case WM_NOTIFY: return app->onNotify(reinterpret_cast<NMHDR*>(lParam));
    case WM_TIMER: if (wParam == uiTimer) app->tick(); return 0;
    case WM_CLOSE: app->close(); return 0;
    case WM_DESTROY:
        KillTimer(window, uiTimer);
        ImageList_Destroy(app->icons);
        for (auto font : {app->regular, app->bold, app->heading}) DeleteObject(font);
        DeleteObject(app->background);
        DeleteObject(app->fieldBrush);
        PostQuitMessage(0);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* sizes = reinterpret_cast<MINMAXINFO*>(lParam);
        int dpi = GetDpiForWindow(window);
        RECT minimum{0, 0, MulDiv(505, dpi, 96), MulDiv(345, dpi, 96)};
        AdjustWindowRectExForDpi(&minimum, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        sizes->ptMinTrackSize.x = minimum.right - minimum.left;
        sizes->ptMinTrackSize.y = minimum.bottom - minimum.top;
        return 0;
    }
    case WM_MEASUREITEM:
        if (measureMenu(window, *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam))) return TRUE;
        break;
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (drawMenu(item)) return TRUE;
        if (item.CtlType == ODT_BUTTON) {
            if (item.CtlID == Mic) {
                drawMicrophoneButton(item, app->audio.muted.load());
            } else if (item.CtlID == ServerList) {
                const auto code = app->active();
                drawButton(item, true, wide(app->state.value("name", "")), wide(code));
            } else {
                drawButton(item, item.CtlID == Actions);
            }
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const bool input = reinterpret_cast<HWND>(lParam) == app->composer;
        SetTextColor(dc, theme::text);
        SetBkColor(dc, input ? theme::field : theme::background);
        return reinterpret_cast<LRESULT>(input ? app->fieldBrush : app->background);
    }
    case WM_HSCROLL: {
        const auto slider = reinterpret_cast<HWND>(lParam);
        if (slider != app->micSlider && slider != app->outputSlider) return 0;
        const bool microphone = slider == app->micSlider;
        auto& gain = microphone ? app->audio.inputGain : app->audio.outputGain;
        const SliderScale scale{microphone ? maxMicrophoneGain : maxOutputGain, app->fivePercentSteps};
        const int position = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        if (position != scale.position(gain.load())) {
            gain = scale.value(position);
            app->redrawAudioPanel();
        }
        return 0;
    }
    case networkResult: {
        std::unique_ptr<NetworkResult> reply(reinterpret_cast<NetworkResult*>(lParam));
        try { app->onNetwork(std::move(*reply)); }
        catch (const Json::exception&) { app->statusText(L"Invalid response from the relay."); }
        return 0;
    }
    case WM_COPYDATA: {
        auto* copy = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (copy->dwData != 1 || copy->cbData < 16 || copy->cbData > 254) return FALSE;
        std::string code(static_cast<const char*>(copy->lpData), copy->cbData);
        if (!validCode(code) && !hostedInvite(code)) return FALSE;
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        app->initialInvite = std::move(code);
        PostMessageW(window, inviteReceived, 0, 0);
        return TRUE;
    }
    case inviteReceived:
        if ((app->directHosting || app->connected) && !app->initialInvite.empty()) {
            auto code = std::exchange(app->initialInvite, {});
            app->promptJoin(wide(code));
        }
        return 0;
    }
    return DefWindowProcW(window, msg, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const auto richEdit = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!richEdit) return 3;
    int argc = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &argc);
    auto code = argc > 1 ? parseCode(args[1]) : std::string{};
    LocalFree(args);
    if (!code.empty() && !GetEnvironmentVariableW(L"KANAL_DATA_PATH", nullptr, 0)) {
        HWND other = FindWindowW(windowClass, nullptr);
        if (other) {
            COPYDATASTRUCT data{1, static_cast<DWORD>(code.size()), code.data()};
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                SMTO_ABORTIFHUNG, 600, &result) && result) return 0;
        }
    }
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    WNDCLASSW klass{};
    klass.hInstance = instance;
    klass.lpszClassName = windowClass;
    klass.lpfnWndProc = procedure;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    if (!RegisterClassW(&klass)) return 1;
    App app(std::move(code));
    HWND window = CreateWindowExW(0, windowClass, L"TinySpeak", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 954, 641, nullptr, nullptr, instance, &app);
    if (!window) return 2;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
