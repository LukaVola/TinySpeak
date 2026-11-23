#include "chat_view.h"
#include "network.h"
#include "palette.h"

#include <richedit.h>
#include <cstdio>

std::string ChatMessage::exportLine() const { return "[" + at + "] " + sender + ": " + text + "\r\n"; }

ChatTime chatTime(const std::string& utc) {
    SYSTEMTIME source{}, local{};
    FILETIME validation{};
    if (utc.size() != 20 || utc.back() != 'Z' ||
        sscanf_s(utc.c_str(), "%4hu-%2hu-%2huT%2hu:%2hu:%2hu", &source.wYear, &source.wMonth,
            &source.wDay, &source.wHour, &source.wMinute, &source.wSecond) != 6 ||
        !SystemTimeToFileTime(&source, &validation)) return {};

    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    const auto zoneResult = GetDynamicTimeZoneInformation(&zone);
    if (zoneResult == TIME_ZONE_ID_INVALID || !SystemTimeToTzSpecificLocalTimeEx(&zone, &source, &local)) local = source;
    wchar_t clock[8]{}, date[80]{};
    swprintf_s(clock, L"%02u:%02u", local.wHour, local.wMinute);
    if (!GetDateFormatEx(L"en-US", 0, &local, L"MMMM d, yyyy", date, ARRAYSIZE(date), nullptr)) {
        swprintf_s(date, L"%u/%u/%u", local.wMonth, local.wDay, local.wYear);
    }
    return {static_cast<unsigned>(local.wYear * 10000 + local.wMonth * 100 + local.wDay), date, clock};
}

namespace {
void paragraph(HWND transcript, bool centered) {
    PARAFORMAT2 format{};
    format.cbSize = sizeof(format);
    format.dwMask = PFM_ALIGNMENT | PFM_SPACEBEFORE | PFM_SPACEAFTER;
    format.wAlignment = centered ? PFA_CENTER : PFA_LEFT;
    format.dySpaceBefore = centered ? 175 : 25;
    format.dySpaceAfter = centered ? 155 : 35;
    SendMessageW(transcript, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&format));
}

void runs(HWND transcript, const std::wstring& value, COLORREF color, bool bold = false) {
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR | CFM_BOLD;
    format.dwEffects = bold ? CFE_BOLD : 0;
    format.crTextColor = color;
    SendMessageW(transcript, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
    SendMessageW(transcript, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(value.c_str()));
}
}

void appendChatView(HWND transcript, const ChatMessage& message, unsigned& displayedDay) {
    auto when = chatTime(message.at);
    CHARRANGE end{-1, -1};
    SendMessageW(transcript, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));
    if (when.day && when.day != displayedDay) {
        paragraph(transcript, true);
        runs(transcript, when.heading + L"\r", theme::secondary, true);
        displayedDay = when.day;
    }
    paragraph(transcript, false);
    if (!when.clock.empty()) runs(transcript, when.clock + L"   ", theme::secondary);
    runs(transcript, wide(message.sender) + L"  ", theme::text, true);
    runs(transcript, wide(message.text) + L"\r", theme::text);
}
