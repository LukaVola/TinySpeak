#pragma once

#include "network.h"

#include <filesystem>
#include <optional>
#include <string_view>

std::filesystem::path programFolder();
bool hostedInvite(std::string_view code);

class HostProcess {
public:
    HostProcess() = default;
    ~HostProcess();
    HostProcess(const HostProcess&) = delete;
    HostProcess& operator=(const HostProcess&) = delete;
    bool start(const std::wstring& arguments, const wchar_t* executable = L"kanal-relay.exe");
    bool running() const;
    std::optional<Json> read();
    void stop();

private:
    HANDLE job{}, process{}, input{}, output{};
    std::string bytes;
};
