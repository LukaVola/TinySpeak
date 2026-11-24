#pragma once

#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include "json.hpp"

using Json = nlohmann::json;
std::wstring wide(const std::string& text);
std::string utf8(const std::wstring& text);
std::string randomCode(std::size_t count);
std::string runProfileCode(const std::string& secret, const std::string& nonce);
std::string inviteCode(const std::string& secret);

constexpr UINT networkResult = WM_APP + 1;
struct NetworkResult {
    unsigned generation;
    Json request;
    Json reply;
};

class Network {
public:
    Network(HWND window, std::wstring address, unsigned generation);
    ~Network();
    void send(Json request);
    void finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
