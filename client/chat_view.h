#pragma once

#include <windows.h>
#include <string>

struct ChatMessage {
    std::string at, sender, text;
    std::string exportLine() const;
    std::size_t size() const { return at.size() + sender.size() + text.size() + sizeof(ChatMessage); }
};

struct ChatTime {
    unsigned day = 0;
    std::wstring heading, clock;
};

ChatTime chatTime(const std::string& utc);
void appendChatView(HWND transcript, const ChatMessage& message, unsigned& displayedDay);
