#include "host_process.h"

#include <algorithm>
#include <vector>

namespace {
void closeHandle(HANDLE& handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}
}

std::filesystem::path programFolder() {
    wchar_t path[32768]{};
    const auto count = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (count == 0 || count >= std::size(path)) return {};
    return std::filesystem::path(std::wstring_view(path, count)).parent_path();
}

bool hostedInvite(std::string_view code) {
    if (code.size() != 53 || !code.starts_with("TS1-") || code[36] != '.') return false;
    const auto alphanumeric = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    return std::ranges::all_of(code.substr(4, 32), [&](char c) { return alphanumeric(c) || c == '-' || c == '_'; }) &&
        std::ranges::all_of(code.substr(37), alphanumeric);
}

HostProcess::~HostProcess() { stop(); }

bool HostProcess::start(const std::wstring& arguments, const wchar_t* executable) {
    stop();
    const auto folder = programFolder();
    const auto file = folder / executable;
    if (folder.empty()) return false;
    HANDLE childInput = nullptr, childOutput = nullptr;
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE errors = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    auto cleanup = [&] {
        closeHandle(childInput);
        closeHandle(childOutput);
        closeHandle(errors);
    };
    job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || errors == INVALID_HANDLE_VALUE ||
        !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !CreatePipe(&childInput, &input, &inherit, 0) || !CreatePipe(&output, &childOutput, &inherit, 0) ||
        !SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0)) {
        cleanup();
        stop();
        return false;
    }
    SIZE_T length = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &length);
    std::vector<std::byte> storage(length);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &length)) {
        cleanup();
        stop();
        return false;
    }
    HANDLE childHandles[]{childInput, childOutput, errors};
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childInput;
    startup.StartupInfo.hStdOutput = childOutput;
    startup.StartupInfo.hStdError = errors;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION child{};
    auto command = L"\"" + file.wstring() + L"\" " + arguments;
    const bool launched = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        childHandles, sizeof(childHandles), nullptr, nullptr) &&
        CreateProcessW(file.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, folder.c_str(),
            &startup.StartupInfo, &child);
    DeleteProcThreadAttributeList(attributes);
    cleanup();
    if (launched) {
        process = child.hProcess;
        if (AssignProcessToJobObject(job, process) && ResumeThread(child.hThread) != DWORD(-1)) {
            CloseHandle(child.hThread);
            return true;
        }
        TerminateProcess(process, 1);
        CloseHandle(child.hThread);
    }
    stop();
    return false;
}

bool HostProcess::running() const { return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }

std::optional<Json> HostProcess::read() {
    if (!output) return std::nullopt;
    for (;;) {
        const auto end = bytes.find('\n');
        if (end != std::string::npos) {
            auto result = Json::parse(bytes.begin(), bytes.begin() + end, nullptr, false);
            bytes.erase(0, end + 1);
            if (result.is_object()) return result;
            return Json{{"error", "The TinySpeak helper returned an invalid response."}};
        }
        DWORD available = 0, count = 0;
        char block[4096];
        if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr) || available == 0 ||
            !ReadFile(output, block, std::min<DWORD>(available, sizeof(block)), &count, nullptr) || !count) {
            return std::nullopt;
        }
        bytes.append(block, count);
        if (bytes.size() > 128 * 1024) return Json{{"error", "The TinySpeak helper response is too large."}};
    }
}

void HostProcess::stop() {
    closeHandle(input);
    if (process) WaitForSingleObject(process, 2200);
    closeHandle(job);
    closeHandle(process);
    closeHandle(output);
    bytes.clear();
}
