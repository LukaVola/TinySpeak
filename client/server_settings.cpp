#include "server_settings.h"
#include "palette.h"
#include "ui.h"

#include <algorithm>
#include <array>
#include <map>
#include <uxtheme.h>

namespace {
enum PanelControl { RoomsPage = 401, MembersPage, Table, RenameServer, AddText, AddVoice, RenameRoom,
    Limit, DeleteRoom, ExportMembers, ForgetMember, Feedback, ServerTitle };

void putCell(HWND table, int row, int col, const std::wstring& text) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = col;
    item.pszText = const_cast<wchar_t*>(text.c_str());
    if (col == 0) ListView_InsertItem(table, &item);
    else ListView_SetItemText(table, row, col, item.pszText);
}

LRESULT CALLBACK tableProc(HWND window, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NOTIFY) {
        const auto& note = *reinterpret_cast<NMHDR*>(l);
        if (note.hwndFrom == ListView_GetHeader(window) && note.code == NM_CUSTOMDRAW) {
            const auto& item = *reinterpret_cast<NMCUSTOMDRAW*>(l);
            if (item.dwDrawStage == CDDS_PREPAINT) {
                fillColor(item.hdc, item.rc, theme::field);
                return CDRF_NOTIFYITEMDRAW;
            }
            if (item.dwDrawStage == CDDS_ITEMPREPAINT) {
                wchar_t text[80]{};
                HDITEMW header{};
                header.mask = HDI_TEXT;
                header.pszText = text;
                header.cchTextMax = ARRAYSIZE(text);
                Header_GetItem(note.hwndFrom, static_cast<int>(item.dwItemSpec), &header);
                RECT bounds = item.rc;
                fillColor(item.hdc, bounds, theme::field);
                bounds.left += MulDiv(9, GetDpiForWindow(window), 96);
                drawTextLine(item.hdc, bounds, text, theme::secondary);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return DefSubclassProc(window, msg, w, l);
}
}

void ServerSettingsPanel::show(HWND owner, const Json& state, const std::string& self, const std::string& publicCode,
        bool membersFirst, Action action) {
    if (!server.empty() || state.value("code", "").empty()) return;
    server = state.at("code").get<std::string>();
    act = std::move(action);
    memberPage = membersFirst || state.value("owner", "") != self;
    update(state, self, publicCode);
    EmptyDialog layout(316, 194);
    layout.dialog.style |= WS_THICKFRAME | WS_CLIPCHILDREN;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &layout.dialog, owner, procedure, reinterpret_cast<LPARAM>(this));
    window = table = nullptr;
    for (auto font : {regular, heading}) if (font) DeleteObject(font);
    regular = heading = nullptr;
    if (background) DeleteObject(background);
    background = nullptr;
    rooms.clear(); members.clear();
    selectedRoom.clear(); selectedMember.clear(); server.clear();
    act = {};
}

void ServerSettingsPanel::close() {
    if (window) EndDialog(window, IDCANCEL);
}

void ServerSettingsPanel::feedback(const std::wstring& text) {
    if (window) putText(GetDlgItem(window, Feedback), text);
}

void ServerSettingsPanel::update(const Json& state, const std::string& self, const std::string& publicCode) {
    if (server.empty()) return;
    if (state.value("code", "") != server) { close(); return; }
    title = wide(state.value("name", ""));
    canManage = !self.empty() && state.value("owner", "") == self;
    canRename = canManage && state.value("server_rename", false);
    canLimit = canManage && state.contains("text_room");
    std::vector<Room> nextRooms;
    if (auto list = state.find("rooms"); list != state.end() && list->is_array()) {
        for (const auto& room : *list) {
            const bool voice = room.value("kind", "") == "voice";
            const int limit = room.value("limit", 0);
            nextRooms.push_back({room.value("id", ""), wide(room.value("name", "")), voice, voice && limit > 0 ? limit : 16});
        }
    }
    std::map<std::string, Member> unique;
    auto collect = [&](const char* field, bool online) {
        auto list = state.find(field);
        if (list == state.end() || !list->is_array()) return;
        for (const auto& person : *list) {
            const auto session = person.value("id", "");
            auto code = online && session == self ? publicCode : person.value("profile_code", "");
            const auto key = code.empty() ? "session:" + session : code;
            unique[key] = {key, std::move(code), wide(person.value("name", "")), online};
        }
    };
    collect("offline", false);
    collect("users", true);
    std::vector<Member> nextMembers;
    for (auto& [key, member] : unique) nextMembers.push_back(std::move(member));
    std::ranges::sort(nextMembers, [](const Member& a, const Member& b) {
        const int cmp = CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE);
        return cmp == CSTR_EQUAL ? a.key < b.key : cmp == CSTR_LESS_THAN;
    });
    const bool changed = memberPage ? members != nextMembers : rooms != nextRooms;
    rooms = std::move(nextRooms);
    members = std::move(nextMembers);
    if (window) {
        putText(GetDlgItem(window, ServerTitle), title);
        if (changed) populate(false);
        else actions();
    }
}

std::string ServerSettingsPanel::membersText() const {
    std::string text = "\xef\xbb\xbfUsername\tProfile ID\r\n";
    for (const auto& member : members) text += utf8(member.name) + "\t" + member.code + "\r\n";
    return text;
}

void ServerSettingsPanel::create(HWND parent) {
    const auto dpi = GetDpiForWindow(window);
    auto font = [dpi](int size, int weight) {
        return CreateFontW(-MulDiv(size, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    };
    regular = font(10, FW_NORMAL);
    heading = font(15, FW_SEMIBOLD);
    background = CreateSolidBrush(theme::background);
    darkFrame(window);
    putText(window, L"Server settings");
    auto label = control(window, L"STATIC", title.c_str(), SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX, ServerTitle);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(heading), TRUE);
    auto button = [&](int id, const wchar_t* text) {
        HWND handle = control(window, L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, id);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(regular), TRUE);
    };
    button(RenameServer, L"Rename server…");
    button(RoomsPage, L"Rooms"); button(MembersPage, L"Members");
    button(AddText, L"New text room…"); button(AddVoice, L"New voice room…");
    button(RenameRoom, L"Rename…"); button(Limit, L"Limit…"); button(DeleteRoom, L"Delete…");
    button(ExportMembers, L"Export as TXT…"); button(ForgetMember, L"Remove member…"); button(IDCANCEL, L"Close");
    auto status = control(window, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX, Feedback);
    SendMessageW(status, WM_SETFONT, reinterpret_cast<WPARAM>(regular), TRUE);
    table = control(window, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
        LVS_OWNERDRAWFIXED | LVS_NOSORTHEADER, Table);
    SetWindowTheme(table, L"", L"");
    SetWindowTheme(ListView_GetHeader(table), L"", L"");
    SetWindowSubclass(table, tableProc, 1, 0);
    ListView_SetBkColor(table, theme::sidebar);
    ListView_SetTextBkColor(table, theme::sidebar);
    ListView_SetTextColor(table, theme::text);
    ListView_SetExtendedListViewStyle(table, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SendMessageW(table, WM_SETFONT, reinterpret_cast<WPARAM>(regular), TRUE);
    RECT anchor{};
    GetWindowRect(parent, &anchor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto area = monitor.rcWork;
    const int width = std::min<int>(MulDiv(640, dpi, 96), area.right - area.left);
    const int height = std::min<int>(MulDiv(465, dpi, 96), area.bottom - area.top);
    const int x = std::clamp<int>((anchor.left + anchor.right - width) / 2, area.left, area.right - width);
    const int y = std::clamp<int>((anchor.top + anchor.bottom - height) / 2, area.top, area.bottom - height);
    SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    populate(true);
    SetFocus(GetDlgItem(window, memberPage ? MembersPage : RoomsPage));
}

void ServerSettingsPanel::arrange() {
    if (!table) return;
    RECT area{}; GetClientRect(window, &area);
    const int dpi = GetDpiForWindow(window);
    const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    const int margin = px(16), gap = px(6), row = px(30), right = area.right - margin;
    auto put = [&](int id, int x, int y, int width, int height) {
        MoveWindow(GetDlgItem(window, id), x, y, std::max(1, width), std::max(1, height), TRUE);
    };
    put(ServerTitle, margin, px(9), right - margin - px(138), px(35));
    put(RenameServer, right - px(126), px(12), px(126), row);
    put(RoomsPage, margin, px(52), px(84), row);
    put(MembersPage, margin + px(84) + gap, px(52), px(95), row);
    const int footer = area.bottom - margin - row, tools = footer - gap - row;
    put(Table, margin, px(89), right - margin, tools - px(89) - px(8));
    put(AddText, margin, tools, px(112), row);
    put(AddVoice, margin + px(112) + gap, tools, px(124), row);
    put(DeleteRoom, right - px(74), tools, px(74), row);
    put(Limit, right - px(74+64) - gap, tools, px(64), row);
    put(RenameRoom, right - px(74+64+82) - gap * 2, tools, px(82), row);
    put(ExportMembers, margin, tools, px(136), row);
    put(ForgetMember, right - px(138), tools, px(138), row);
    put(Feedback, margin, footer, right - margin - px(86) - gap, row);
    put(IDCANCEL, right - px(86), footer, px(86), row);
    RECT list{}; GetClientRect(table, &list);
    int width = std::max<int>(1, list.right);
    if (memberPage) {
        const int codeWidth = std::clamp<int>(width / 2, px(166), px(240));
        ListView_SetColumnWidth(table, 0, std::max(1, width - codeWidth));
        ListView_SetColumnWidth(table, 1, codeWidth);
    } else {
        ListView_SetColumnWidth(table, 0, std::max(1, width - px(187)));
        ListView_SetColumnWidth(table, 1, px(103));
        ListView_SetColumnWidth(table, 2, px(84));
    }
}

void ServerSettingsPanel::populate(bool columns) {
    refreshing = true;
    SendMessageW(table, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(table);
    if (columns) {
        while (ListView_DeleteColumn(table, 0)) {}
        const std::array<const wchar_t*,3> labels = memberPage ?
            std::array<const wchar_t*,3>{L"Username", L"Profile ID", nullptr} :
            std::array<const wchar_t*,3>{L"Room", L"Type", L"Voice limit"};
        for (int i=0; i<3 && labels[i]; ++i) {
            LVCOLUMNW col{}; col.mask = LVCF_TEXT | LVCF_WIDTH; col.cx = 80;
            col.pszText = const_cast<wchar_t*>(labels[i]);
            ListView_InsertColumn(table, i, &col);
        }
    }
    int chosen = -1;
    if (memberPage) {
        for (int i = 0; i < static_cast<int>(members.size()); ++i) {
            const auto& member = members[i];
            putCell(table, i, 0, member.name);
            putCell(table, i, 1, member.code.empty() ? L"—" : wide(member.code));
            if (member.key == selectedMember) chosen = i;
        }
    } else {
        for (int i = 0; i < static_cast<int>(rooms.size()); ++i) {
            const auto& room = rooms[i];
            putCell(table, i, 0, room.name);
            putCell(table, i, 1, room.voice ? L"Voice" : L"Text");
            putCell(table, i, 2, room.voice ? std::to_wstring(room.limit) : L"—");
            if (room.id == selectedRoom) chosen = i;
        }
    }
    if (chosen < 0 && ListView_GetItemCount(table)) chosen = 0;
    if (chosen >= 0) {
        ListView_SetItemState(table, chosen, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(table, chosen, FALSE);
    }
    refreshing = false;
    arrange();
    select();
    SendMessageW(table, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(table, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    for (int id : {RoomsPage, MembersPage}) InvalidateRect(GetDlgItem(window, id), nullptr, TRUE);
}

void ServerSettingsPanel::select() {
    if (!table || refreshing) return;
    const int i = ListView_GetNextItem(table, -1, LVNI_SELECTED);
    if (memberPage) selectedMember = i >= 0 && i < static_cast<int>(members.size()) ? members[i].key : "";
    else selectedRoom = i >= 0 && i < static_cast<int>(rooms.size()) ? rooms[i].id : "";
    actions();
}

void ServerSettingsPanel::actions() {
    const int i = ListView_GetNextItem(table, -1, LVNI_SELECTED);
    const bool room = !memberPage && i >= 0 && i < static_cast<int>(rooms.size());
    const bool absent = memberPage && i >= 0 && i < static_cast<int>(members.size()) && !members[i].online;
    auto button = [&](int id, bool show, bool enable) {
        const auto handle = GetDlgItem(window, id);
        ShowWindow(handle, show ? SW_SHOWNA : SW_HIDE);
        EnableWindow(handle, enable);
    };
    button(RenameServer, canManage, canRename);
    for (int id : {AddText, AddVoice, RenameRoom, Limit, DeleteRoom}) {
        const bool enabled = id == Limit ? room && rooms[i].voice && canLimit :
            id == DeleteRoom ? room && rooms.size() > 1 : id == RenameRoom ? room : true;
        button(id, !memberPage && canManage && (id != Limit || (room && rooms[i].voice)), enabled);
    }
    button(ExportMembers, memberPage, !members.empty());
    button(ForgetMember, memberPage && canManage, absent);
}

void ServerSettingsPanel::command(int id) {
    if (id == IDCANCEL) { close(); return; }
    if (id == RoomsPage || id == MembersPage) {
        if (memberPage != (id == MembersPage)) {
            memberPage = id == MembersPage;
            populate(true);
            feedback(L"");
        }
        return;
    }
    if (!act || id < RenameServer || id > ForgetMember) return;
    const auto source = GetDlgItem(window, id);
    if (!IsWindowEnabled(source) || !IsWindowVisible(source)) return;
    const std::array commands{ServerAction::Rename, ServerAction::AddText, ServerAction::AddVoice,
        ServerAction::RenameRoom, ServerAction::VoiceLimit, ServerAction::DeleteRoom, ServerAction::ExportMembers,
        ServerAction::ForgetMember};
    const std::string target = memberPage ? selectedMember : selectedRoom;
    act(window, commands[id - RenameServer], target);
}

void ServerSettingsPanel::draw(const DRAWITEMSTRUCT& item) {
    if (item.CtlType == ODT_BUTTON) {
        drawButton(item, false);
        if (item.CtlID == (memberPage ? MembersPage : RoomsPage)) {
            auto line = item.rcItem;
            line.top = line.bottom - MulDiv(2, GetDpiForWindow(window), 96);
            fillColor(item.hDC, line, theme::text);
        }
    } else if (item.CtlID == Table && item.itemID != static_cast<UINT>(-1)) {
        fillColor(item.hDC, item.rcItem, item.itemState & ODS_SELECTED ? theme::selected : theme::sidebar);
        int x = item.rcItem.left;
        const int pad = MulDiv(9, GetDpiForWindow(window), 96);
        for (int i = 0; i < (memberPage ? 2 : 3); ++i) {
            wchar_t text[160]{};
            ListView_GetItemText(table, item.itemID, i, text, ARRAYSIZE(text));
            const int width = ListView_GetColumnWidth(table, i);
            RECT cell{x + pad, item.rcItem.top, x + width - pad, item.rcItem.bottom};
            drawTextLine(item.hDC, cell, text, i == 0 ? theme::text : theme::secondary);
            x += width;
        }
    }
}

INT_PTR CALLBACK ServerSettingsPanel::procedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* panel = reinterpret_cast<ServerSettingsPanel*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        panel = reinterpret_cast<ServerSettingsPanel*>(l);
        SetWindowLongPtrW(hwnd, DWLP_USER, l);
        panel->window = hwnd;
        panel->create(GetParent(hwnd));
        return FALSE;
    }
    if (!panel) return FALSE;
    switch (msg) {
    case WM_COMMAND: if (HIWORD(w) == BN_CLICKED) panel->command(LOWORD(w)); return TRUE;
    case WM_CLOSE: panel->close(); return TRUE;
    case WM_SIZE: panel->arrange(); return TRUE;
    case WM_GETMINMAXINFO: {
        const int dpi = GetDpiForWindow(hwnd);
        auto* limits = reinterpret_cast<MINMAXINFO*>(l);
        limits->ptMinTrackSize = {MulDiv(530,dpi,96),MulDiv(315,dpi,96)};
        return TRUE;
    }
    case WM_NOTIFY: {
        const auto* note = reinterpret_cast<NMHDR*>(l);
        if (note->idFrom == Table && note->code == LVN_ITEMCHANGED) panel->select();
        return FALSE;
    }
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
        if (item->CtlID != Table) return FALSE;
        item->itemHeight = MulDiv(27,GetDpiForWindow(hwnd),96);
        return TRUE;
    }
    case WM_DRAWITEM: panel->draw(*reinterpret_cast<DRAWITEMSTRUCT*>(l)); return TRUE;
    case WM_CTLCOLORDLG: return reinterpret_cast<INT_PTR>(panel->background);
    case WM_CTLCOLORSTATIC: {
        auto dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, GetDlgCtrlID(reinterpret_cast<HWND>(l)) == ServerTitle ? theme::text : theme::secondary);
        SetBkColor(dc, theme::background);
        return reinterpret_cast<INT_PTR>(panel->background);
    }
    case WM_DESTROY: panel->window = panel->table = nullptr; return TRUE;
    }
    return FALSE;
}
