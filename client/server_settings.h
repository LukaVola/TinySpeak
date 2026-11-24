#pragma once

#include "network.h"
#include <functional>
#include <vector>

enum class ServerAction { Rename, AddText, AddVoice, RenameRoom, VoiceLimit, DeleteRoom, ExportMembers, ForgetMember };

class ServerSettingsPanel {
public:
    using Action = std::function<void(HWND, ServerAction, const std::string&)>;
    void show(HWND owner, const Json& state, const std::string& self, const std::string& publicCode, bool membersFirst, Action action);
    void update(const Json& state, const std::string& self, const std::string& publicCode);
    void feedback(const std::wstring& text);
    void close();
    std::string membersText() const;

private:
    struct Room {
        std::string id;
        std::wstring name;
        bool voice;
        int limit;
        bool operator==(const Room&) const = default;
    };
    struct Member {
        std::string key, code;
        std::wstring name;
        bool online;
        bool operator==(const Member&) const = default;
    };
    HWND window{}, table{};
    HFONT regular{}, heading{};
    HBRUSH background{};
    std::string server, selectedRoom, selectedMember;
    std::wstring title;
    std::vector<Room> rooms;
    std::vector<Member> members;
    bool memberPage = false, canManage = false, canRename = false, canLimit = false, refreshing = false;
    Action act;
    static INT_PTR CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    void create(HWND parent);
    void arrange();
    void populate(bool columns);
    void select();
    void actions();
    void command(int id);
    void draw(const DRAWITEMSTRUCT& item);
};
