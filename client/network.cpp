#include "network.h"

#include <winhttp.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
constexpr char codeAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
void appendSymbols(std::string& value, const auto& bytes, std::size_t count) {
    for (auto b : bytes) {
        if (b < 248) value.push_back(codeAlphabet[b % 62]);
        if (value.size() == count) break;
    }
}
}

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::string randomCode(std::size_t count) {
    std::string value;
    while (value.size() < count) {
        std::array<unsigned char, 48> bytes{};
        if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            throw std::runtime_error("Windows random number generator failed.");
        }
        appendSymbols(value, bytes, count);
    }
    return value;
}

static std::string shortCode(const char* domain, const std::string& secret) {
    BCRYPT_ALG_HANDLE sha = nullptr;
    if (BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        throw std::runtime_error("Windows SHA-256 provider failed.");
    }
    std::string value;
    for (int n = 0; value.size() < 16; ++n) {
        auto input = domain + secret + ":" + std::to_string(n);
        std::array<unsigned char, 32> digest{};
        if (BCryptHash(sha, nullptr, 0, reinterpret_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()),
            digest.data(), static_cast<ULONG>(digest.size())) != 0) {
            BCryptCloseAlgorithmProvider(sha, 0);
            throw std::runtime_error("Windows SHA-256 hash failed.");
        }
        appendSymbols(value, digest, 16);
    }
    BCryptCloseAlgorithmProvider(sha, 0);
    return value;
}

std::string inviteCode(const std::string& secret) { return shortCode("Kanal invite v1:", secret); }
std::string runProfileCode(const std::string& secret, const std::string& nonce) {
    return shortCode("TinySpeak profile run v1:", secret + ":" + nonce);
}

namespace {
struct HttpHandle {
    HINTERNET handle = nullptr;
    explicit HttpHandle(HINTERNET value) : handle(value) {}
    ~HttpHandle() { if (handle) WinHttpCloseHandle(handle); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    operator HINTERNET() const { return handle; }
};

struct Endpoint {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool secure = false, local = false;

    explicit Endpoint(std::wstring url) {
        URL_COMPONENTSW parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwUserNameLength =
            parts.dwPasswordLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || !parts.dwHostNameLength ||
            parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength) return;
        host.assign(parts.lpszHostName, parts.dwHostNameLength);
        if (parts.dwUrlPathLength) path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
        while (!path.empty() && path.back() == L'/') path.pop_back();
        path += L"/api";
        secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
        local = host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]";
        if (secure || (local && parts.nScheme == INTERNET_SCHEME_HTTP)) port = parts.nPort;
    }
};
}

struct Network::Impl {
    HWND window;
    unsigned generation;
    Endpoint endpoint;
    HttpHandle http;
    std::mutex lock;
    std::condition_variable signal;
    std::deque<Json> commands;
    std::string key;
    bool stop = false;
    std::thread writer, reader;

    Impl(HWND target, std::wstring address, unsigned version)
        : window(target), generation(version), endpoint(std::move(address)),
          http(WinHttpOpen(L"TinySpeak", endpoint.local ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)),
          writer([this] { write(); }), reader([this] { poll(); }) {}

    ~Impl() { finish(); }

    void deliver(Json request, Json reply) {
        auto result = std::make_unique<NetworkResult>(NetworkResult{generation, std::move(request), std::move(reply)});
        if (PostMessageW(window, networkResult, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    }

    Json call(const Json& body, const std::string& access, bool longPoll) {
        const auto error = [] { return Json{{"error", "Could not connect to the relay."}, {"network", true}}; };
        if (!endpoint.port) return {{"error", "Enter the HTTPS relay address or a localhost address."}, {"network", true}};
        if (!http) return error();
        HttpHandle connection(WinHttpConnect(http, endpoint.host.c_str(), endpoint.port, 0));
        if (!connection) return error();
        HttpHandle request(WinHttpOpenRequest(connection, L"POST", endpoint.path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, endpoint.secure ? WINHTTP_FLAG_SECURE : 0));
        if (!request) return error();
        WinHttpSetTimeouts(request, 4000, 4000, 4000, longPoll ? 14000 : 5000);
        DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
        std::wstring headers = L"Content-Type: application/json\r\n";
        if (!access.empty()) headers += L"Authorization: Bearer " + wide(access) + L"\r\n";
        auto payload = body.dump();
        const auto size = static_cast<DWORD>(payload.size());
        if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                              payload.data(), size, size, 0) || !WinHttpReceiveResponse(request, nullptr)) return error();
        DWORD statusCode = 0, statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        std::string bytes;
        std::array<char, 8192> block{};
        for (;;) {
            DWORD read = 0;
            if (!WinHttpReadData(request, block.data(), static_cast<DWORD>(block.size()), &read)) return error();
            if (!read) break;
            if (bytes.size() + read > 6 * 1024 * 1024) return error();
            bytes.append(block.data(), read);
        }
        Json reply = Json::parse(bytes, nullptr, false);
        if (!reply.is_object()) return error();
        if (statusCode == 401) reply["expired"] = true;
        return reply;
    }

    void write() {
        for (;;) {
            std::unique_lock guard(lock);
            signal.wait(guard, [this] { return stop || !commands.empty(); });
            if (commands.empty()) return;
            auto request = std::move(commands.front());
            commands.pop_front();
            auto access = key;
            guard.unlock();
            const bool registration = request["op"] == "register";
            if (!registration && access.empty()) continue;
            Json reply = call(request, registration ? "" : access, false);
            if (registration && reply.contains("key")) {
                {
                    std::lock_guard update(lock);
                    key = reply["key"].get<std::string>();
                }
                signal.notify_all();
            }
            if (request["op"] != "quit") deliver(std::move(request), std::move(reply));
        }
    }

    void poll() {
        bool offline = false;
        for (;;) {
            std::unique_lock guard(lock);
            signal.wait(guard, [this] { return stop || !key.empty(); });
            if (stop) return;
            auto access = key;
            guard.unlock();
            Json q{{"op", "poll"}};
            Json reply = call(q, access, true);
            if (reply.value("expired", false) || (reply.contains("error") && !reply.value("network", false))) {
                {
                    std::lock_guard update(lock);
                    if (key == access) key.clear();
                }
                reply["expired"] = true;
                deliver(std::move(q), std::move(reply));
                continue;
            }
            if (reply.value("network", false)) {
                if (!offline) deliver(q, std::move(reply));
                offline = true;
                guard.lock();
                signal.wait_for(guard, std::chrono::seconds(1), [this] { return stop; });
                continue;
            }
            if (offline) reply["restored"] = true;
            offline = false;
            if (!reply.empty()) deliver(std::move(q), std::move(reply));
        }
    }

    void finish() {
        {
            std::lock_guard guard(lock);
            if (stop) return;
            commands.clear();
            commands.push_back(Json{{"op", "quit"}});
            stop = true;
        }
        signal.notify_all();
        if (writer.joinable()) writer.join();
        if (reader.joinable()) reader.join();
    }
};

Network::Network(HWND window, std::wstring address, unsigned generation)
    : impl_(std::make_unique<Impl>(window, std::move(address), generation)) {}
Network::~Network() = default;

void Network::send(Json value) {
    {
        std::lock_guard guard(impl_->lock);
        if (impl_->stop || impl_->commands.size() >= 512) return;
        impl_->commands.push_back(std::move(value));
    }
    impl_->signal.notify_all();
}

void Network::finish() { impl_->finish(); }
