#pragma once

#include <windows.h>
#include <shlobj.h>
#include <filesystem>

inline std::filesystem::path profilePath(bool localDemo) {
    namespace fs = std::filesystem;
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &folder))) {
        return fs::current_path() / L"TinySpeakData";
    }
    const fs::path local(folder);
    CoTaskMemFree(folder);
    auto next = local / L"TinySpeak";
    auto previous = local / L"Kanal";
    if (localDemo) {
        next /= L"LokalnaProba";
        previous /= L"LokalnaProba";
    }
    std::error_code error;
    if (!fs::exists(next / L"settings.json", error) && fs::exists(previous / L"settings.json", error)) {
        fs::create_directories(next, error);
        if (!error) fs::copy(previous, next, fs::copy_options::recursive | fs::copy_options::skip_existing, error);
    }
    return next;
}
