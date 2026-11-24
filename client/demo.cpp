#include <windows.h>
#include <shellapi.h>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int show) {
    if (!arguments || !*arguments) {
        if (HWND existing = FindWindowW(L"TinySpeak.Desktop", nullptr)) {
            SendMessageTimeoutW(existing, WM_SYSCOMMAND, SC_RESTORE, 0, SMTO_ABORTIFHUNG, 3000, nullptr);
            SetForegroundWindow(existing);
            return 0;
        }
    }
    wchar_t exe[32768];
    const DWORD count = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (!count || count >= std::size(exe)) return 1;
    const auto path = std::wstring(exe, count);
    const auto folder = path.substr(0, path.find_last_of(L"\\/"));
    const auto client = folder + L"\\TinySpeak.exe";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", client.c_str(), arguments, folder.c_str(), show)) <= 32) {
        MessageBoxW(nullptr, L"TinySpeak could not start. Extract all files from the ZIP into the same folder.",
            L"TinySpeak", MB_ICONERROR);
        return 1;
    }
    return 0;
}
