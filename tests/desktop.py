"""Run the installed URL-handler and chat-control checks on a disposable Windows runner."""

import ctypes as ct
import hashlib
from ctypes import wintypes as wt
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import winreg


def wait_for(check, description):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError(description)


class Relay(BaseHTTPRequestHandler):
    joined = []
    room_actions = []
    created = {}
    layouts = {}
    states = {}
    revision = 0
    user = {"id": "listener", "name": "Listener", "color": 8}
    room = {"id": "ChatRoom0001", "kind": "text", "name": "General"}
    voice = {"id": "VoiceRoom001", "kind": "voice", "name": "Lounge", "limit": 6}

    def log_message(self, *args):
        pass

    def do_POST(self):
        command = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        op = command["op"]
        response = {}
        if op == "register":
            alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
            public = ""
            for i in range(8):
                source = f"TinySpeak profile run v1:{command['profile']}:{command['run_nonce']}:{i}"
                digest = hashlib.sha256(source.encode()).digest()
                public += "".join(alphabet[b % 62] for b in digest if b < 248)
                if len(public) >= 16:
                    break
            self.user["profile_code"] = public[:16]
            response = {"key": "test-session", "user": self.user}
        elif op == "profile":
            self.user.update(name=command["name"], color=command["color"])
            response = {"user": self.user}
            if self.states:
                current = self.states[next(reversed(self.states))]
                current["users"][0].update(self.user)
                response["state"] = current
            self.room_actions.append(command)
        elif op == "advertise":
            self.created[command["code"]] = command["name"]
        elif op == "hydrate":
            self.layouts[command["code"]] = command["rooms"]
        elif op == "join" and command["code"] in self.created:
            code = command["code"]
            if code not in self.layouts:
                response = {"retry": True, "events": [{"type": "load", "code": code}]}
            else:
                rooms = self.layouts[code]
                response = {"version": 100, "state": {"code": code, "name": self.created[code], "owner": self.user["id"],
                            "users": [self.user], "rooms": rooms, "text_room": rooms[0]["id"]}}
        elif op == "join":
            code = command["code"]
            self.joined.append(code)
            state = {"code": code, "name": "Test server", "owner": "remote-owner" if code.endswith("00000002") else self.user["id"], "server_rename": True,
                     "users": [dict(self.user, voice=self.voice["id"])], "voice": self.voice["id"],
                     "rooms": [self.room.copy(), self.voice.copy()], "text_room": self.room["id"],
                     "offline": [{"profile_code": "PublicCode123456", "name": "Željko", "color": 2}]}
            self.states[code] = state
            response = {"state": state, "events": [
                {"type": "chat", "code": code, "room": self.room["id"], "name": "General",
                 "at": "2026-01-01T10:00:00Z", "sender": "Speaker", "text": f"Test message {i}"} for i in range(75)]}
        elif op in ("room_limit", "room_add", "room_delete", "room_rename", "server_rename", "voice_leave"):
            state = self.states[command["code"]]
            rooms = state["rooms"]
            if op == "room_add":
                room = {"id": f"ExtraRoom{len(rooms):03d}", "kind": command["kind"], "name": command["name"]}
                if room["kind"] == "voice":
                    room["limit"] = 16
                rooms.append(room)
            elif op == "server_rename":
                state["name"] = command["name"]
            elif op == "voice_leave":
                state["voice"] = state["users"][0]["voice"] = ""
            else:
                room = next(r for r in rooms if r["id"] == command["room"])
                if op == "room_limit":
                    assert room["kind"] == "voice"
                    room["limit"] = command["limit"]
                elif op == "room_rename":
                    room["name"] = command["name"]
                else:
                    rooms.remove(room)
            self.room_actions.append(command)
            response = {"state": state}
        elif op == "poll":
            time.sleep(0.1)
        if "state" in response:
            type(self).revision += 1
            response["version"] = self.revision
        data = json.dumps(response).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


ui = ct.WinDLL("user32", use_last_error=True)
shell = ct.WinDLL("shell32", use_last_error=True)


def native(lib, name, result, *args):
    fn = getattr(lib, name)
    fn.restype = result
    fn.argtypes = args
    return fn


find = native(ui, "FindWindowW", wt.HWND, wt.LPCWSTR, wt.LPCWSTR)
child = native(ui, "GetDlgItem", wt.HWND, wt.HWND, ct.c_int)
find_child = native(ui, "FindWindowExW", wt.HWND, wt.HWND, wt.HWND, wt.LPCWSTR, wt.LPCWSTR)
send = native(ui, "SendMessageW", ct.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
post = native(ui, "PostMessageW", wt.BOOL, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
style = native(ui, "GetWindowLongW", wt.LONG, wt.HWND, ct.c_int)
visible = native(ui, "IsWindowVisible", wt.BOOL, wt.HWND)
window_thread = native(ui, "GetWindowThreadProcessId", wt.DWORD, wt.HWND, ct.POINTER(wt.DWORD))
open_url = native(shell, "ShellExecuteW", ct.c_ssize_t, wt.HWND, wt.LPCWSTR, wt.LPCWSTR,
                  wt.LPCWSTR, wt.LPCWSTR, ct.c_int)


class ThreadInfo(ct.Structure):
    _fields_ = [("size", wt.DWORD), ("flags", wt.DWORD), ("active", wt.HWND), ("focus", wt.HWND),
                ("capture", wt.HWND), ("menu", wt.HWND), ("move", wt.HWND), ("caret", wt.HWND),
                ("rect", wt.RECT)]


def focused(window):
    info = ThreadInfo()
    info.size = ct.sizeof(info)
    get_info = native(ui, "GetGUIThreadInfo", wt.BOOL, wt.DWORD, ct.POINTER(ThreadInfo))
    assert get_info(window_thread(window, None), ct.byref(info))
    return info.focus


def children(window, cls):
    previous = None
    while handle := find_child(window, previous, cls, None):
        yield handle
        previous = handle


def set_text(window, value):
    data = ct.create_unicode_buffer(value)
    assert send(window, 0x000C, 0, ct.addressof(data))


def get_text(window):
    size = send(window, 0x000E, 0, 0) + 1
    data = ct.create_unicode_buffer(size)
    send(window, 0x000D, size, ct.addressof(data))
    return data.value


def press(dialog, command):
    post(dialog, 0x111, command, 0)
    wait_for(lambda: not visible(dialog), "Dialog did not close")


def set_slider(dialog, slider, position):
    send(slider, 0x405, 1, position)  # TBM_SETPOS
    send(dialog, 0x0114, 4, slider)  # WM_HSCROLL, TB_THUMBPOSITION


def check_settings(main, change):
    post(main, 0x111, 105, 0)
    dialog = wait_for(lambda: find("#32770", "Settings"), "Settings dialog")
    bitrate = None
    target = ct.create_unicode_buffer("128 kbps")
    for combo in children(dialog, "ComboBox"):
        idx = send(combo, 0x0158, -1, ct.addressof(target))  # CB_FINDSTRINGEXACT
        if idx >= 0:
            bitrate = combo
            break
    assert bitrate
    assert send(bitrate, 0x0158, -1, ct.addressof(ct.create_unicode_buffer("32 kbps"))) >= 0
    key = find_child(dialog, None, "Button", "F8" if change else "V")
    assert key, "The selected push-to-talk key was not restored"
    if change:
        send(key, 0x00f5, 0, 0)
        send(key, 0x0100, ord("V"), 0x002f0001)
        send(key, 0x0101, ord("V"), 0xc02f0001)
        assert get_text(key) == "V"
    assert send(bitrate, 0x0158, -1, ct.addressof(ct.create_unicode_buffer("80 kbps"))) >= 0
    if change:
        send(bitrate, 0x014e, idx, 0)  # CB_SETCURSEL
    else:
        assert send(bitrate, 0x0147, 0, 0) == idx
    for name in ["Save my chat logs on this computer", "Save my servers, rooms and chat logs",
                 "Keep TinySpeak in the system tray when I close the window"]:
        assert send(find_child(dialog, None, "Button", name), 0x00f0, 0, 0) == 1
    button = find_child(dialog, None, "Button", "Playback equalizer and normalization…")
    post(button, 0x00f5, 0, 0)
    eq = wait_for(lambda: find("#32770", "Playback equalizer"), "EQ dialog")
    tracks = list(children(eq, "msctls_trackbar32"))
    assert len(tracks) == 5 and send(tracks[-1], 0x0402, 0, 0) == 4  # five strengths
    if change:
        set_slider(eq, tracks[1], 15)  # +3 dB
        set_slider(eq, tracks[-1], 3)  # 75%
    else:
        assert send(tracks[1], 0x0400, 0, 0) == 15
        assert send(tracks[-1], 0x0400, 0, 0) == 3
    press(eq, 1)
    press(dialog, 1)


def choose_file(title, path):
    dialog = wait_for(lambda: find("#32770", title), title)
    def file_field():
        combo = child(dialog, 0x047c)  # cmb13 on Explorer-style common dialogs
        classic = (find_child(combo, None, "Edit", None) if combo else None) or child(dialog, 0x0480)
        if classic:
            return classic
        # The modern common dialog nests the file-name edit inside a DirectUI host.
        matches = []
        visitor = ct.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        ctrl_id = native(ui, "GetDlgCtrlID", ct.c_int, wt.HWND)
        parent = native(ui, "GetParent", wt.HWND, wt.HWND)
        @visitor
        def visit(w, _):
            if ctrl_id(w) in (1001, 0x047c) and find_child(parent(w), None, "Edit", None) == w:
                matches.append(w)
            return not matches
        native(ui, "EnumChildWindows", wt.BOOL, wt.HWND, visitor, wt.LPARAM)(dialog, visit, 0)
        return matches[0] if matches else None
    field = wait_for(lambda: file_field() if visible(dialog) else None, "File name edit")
    initial = {"Export chat as TXT": "TinySpeak-chat", "Export server members": "TinySpeak-members"}.get(title)
    if initial:
        wait_for(lambda: initial in get_text(field), "Suggested export name")
    send(field, 0x00b1, 0, -1)  # select the previous name, then type as the user would
    for char in str(path):
        send(field, 0x0102, ord(char), 0)
    assert get_text(field) == str(path)
    post(child(dialog, 1), 0x00f5, 0, 0)
    wait_for(lambda: not visible(dialog), "File dialog did not accept the selected path")


class MenuBarInfo(ct.Structure):
    _fields_ = [("size", wt.DWORD), ("rect", wt.RECT), ("menu", wt.HANDLE),
                ("window", wt.HWND), ("flags", wt.DWORD)]


def choose_menu(main, command, room=None, required=(), absent=()):
    bar_info = native(ui, "GetMenuBarInfo", wt.BOOL, wt.HWND, wt.LONG, wt.LONG, ct.POINTER(MenuBarInfo))
    count = native(ui, "GetMenuItemCount", ct.c_int, wt.HANDLE)
    item_id = native(ui, "GetMenuItemID", wt.UINT, wt.HANDLE, ct.c_int)
    submenu = native(ui, "GetSubMenu", wt.HANDLE, wt.HANDLE, ct.c_int)
    state = native(ui, "GetMenuState", wt.UINT, wt.HANDLE, wt.UINT, wt.UINT)
    thread = window_thread(main, None)

    def popup(expected=None):
        for handle in children(None, "#32768"):
            if not visible(handle) or window_thread(handle, None) != thread:
                continue
            info = MenuBarInfo()
            info.size = ct.sizeof(info)
            if bar_info(handle, -4, 0, ct.byref(info)) and info.menu and (not expected or expected == info.menu):
                return handle, info.menu

    def highlight(handle, menu, index):
        def current():
            for k in range(count(menu)):
                flags = state(menu, k, 0x0400)
                if flags != 0xffffffff and flags & 0x0080:
                    return k
            return -1

        def moved(old):
            new = current()
            # Native menus briefly clear all highlights between two items.
            return (new,) if new >= 0 and new != old else None

        selected = current()
        for _ in range(2 * count(menu) + 2):
            if selected == index:
                return
            post(handle, 0x0100, 0x28, 0)
            selected, = wait_for(lambda: moved(selected), "Popup selection did not move to another item")
        if selected != index:
            raise AssertionError(f"Popup selection stopped on {selected}, expected {index}")

    if room:
        tree = child(main, 107)
        send(tree, 0x110b, 9, room)  # Select this room, then open its native context menu.
        post(main, 0x007b, tree, -1)
        handle, menu = wait_for(popup, "Room context menu")
        commands = [item_id(menu, i) for i in range(count(menu))]
        assert command in commands and all(n in commands for n in required) and all(n not in commands for n in absent), commands
        highlight(handle, menu, commands.index(command))
        post(handle, 0x0100, 0x0d, 0)
        return

    assert not child(main, 118) and not child(main, 129), "Chat file buttons are still on the main window"
    post(child(main, 103), 0x00f5, 0, 0)
    outer, menu = wait_for(popup, "Main menu")
    items = range(count(menu))
    assert all(item_id(menu, i) not in (118, 129) for i in items)
    for i in items:
        logs = submenu(menu, i)
        if logs and [item_id(logs, j) for j in range(count(logs))] == [118, 129, 106]:
            break
    else:
        raise AssertionError("Chat logs submenu missing or in the wrong order")
    highlight(outer, menu, i)
    post(outer, 0x0100, 0x27, 0)  # Right opens the submenu
    inner, _ = wait_for(lambda: popup(logs), "Chat logs submenu did not open")
    highlight(inner, logs, [118, 129, 106].index(command))
    post(inner, 0x0100, 0x0d, 0)  # Enter activates the actual command


def check_files(main, root):
    path = root / "conversation.txt"
    choose_menu(main, 118)
    choose_file("Export chat as TXT", path)
    wait_for(path.exists, "Chat export")
    text = path.read_text(encoding="utf-8-sig")
    assert "Speaker: Test message 0" in text and "Speaker: Test message 74" in text
    choose_menu(main, 129)
    choose_file("Open chat TXT", path)
    viewer = wait_for(lambda: find("#32770", "Saved chat"), "Saved chat viewer")
    assert "Speaker: Test message 74" in get_text(child(viewer, 501))
    press(viewer, 1)
    path.write_text("First ž\n" + "Čćć saved chat\r\n" * 24000 + "Last š", encoding="utf-8-sig")
    choose_menu(main, 129)
    choose_file("Open chat TXT", path)
    viewer = wait_for(lambda: find("#32770", "Saved chat"), "Large chat viewer")
    assert get_text(child(viewer, 501)).startswith("First ž")
    send(viewer, 0x111, 503, 0)
    assert get_text(child(viewer, 501)).endswith("Last š")
    send(viewer, 0x111, 502, 0)
    assert get_text(child(viewer, 501)).startswith("First ž")
    press(viewer, 1)


def paste_into_join(main, value):
    post(main, 0x111, 102, 0)  # WM_COMMAND, Join
    dialog = wait_for(lambda: find("#32770", "Join a server"), "Join dialog did not open")
    field = find_child(dialog, None, "Edit", None)
    assert field
    data = ct.create_unicode_buffer(value)
    assert send(field, 0x000C, 0, ct.addressof(data))  # WM_SETTEXT
    post(dialog, 0x111, 1, 0)
    wait_for(lambda: not find("#32770", "Join a server"), "Join dialog did not close")


def profile_code(main):
    post(main, 0x111, 104, 0)
    dialog = wait_for(lambda: find("#32770", "Username and color"), "Profile dialog")
    code = next(get_text(w) for w in children(dialog, "Edit") if style(w, -16) & 0x0800)
    assert len(code) == 16 and code.isascii() and code.isalnum()
    press(dialog, 2)
    return code


def bounds(window):
    rect = wt.RECT()
    assert native(ui, "GetWindowRect", wt.BOOL, wt.HWND, ct.POINTER(wt.RECT))(window, ct.byref(rect))
    return rect


def check_sidebar(main, restoring=False):
    tree, toggle = child(main, 107), child(main, 138)
    setting, menu, new_server = [bounds(child(main, i)) for i in (135, 103, 101)]
    assert setting.right <= menu.left and setting.bottom <= new_server.top and setting.left == new_server.left
    if restoring:
        assert not visible(tree) and get_text(toggle) == "Expand rooms"
        send(toggle, 0x00f5, 0, 0)
        assert visible(tree)
        return bounds(tree).right - bounds(tree).left
    size = bounds(tree)
    dpi = native(ui, "GetDpiForWindow", wt.UINT, wt.HWND)(main)
    point = wt.POINT(size.right + round(7 * dpi / 96), size.top + 8)
    assert native(ui, "ScreenToClient", wt.BOOL, wt.HWND, ct.POINTER(wt.POINT))(main, ct.byref(point))
    def packed(x, y):
        return (x & 0xffff) | ((y & 0xffff) << 16)
    send(main, 0x0201, 1, packed(point.x, point.y))
    send(main, 0x0200, 1, packed(point.x + 46, point.y))
    send(main, 0x0202, 0, packed(point.x + 46, point.y))
    result = bounds(tree)
    assert result.right - size.right >= 44
    send(toggle, 0x00f5, 0, 0)
    assert not visible(tree) and get_text(toggle) == "Expand rooms"
    return result.right - result.left


def check_rooms(main):
    tree = child(main, 107)
    def node(flag, item=0):
        return send(tree, 0x110a, flag, item)
    def room(voice=False):
        root = node(0)
        return node(4, node(1, root) if voice else root)
    def changed(op, **values):
        wait_for(lambda: any(r["op"] == op and all(r.get(k) == v for k, v in values.items())
                             for r in Relay.room_actions), op)
    def edit(title, name):
        dialog = wait_for(lambda: find("#32770", title), title)
        set_text(find_child(dialog, None, "Edit", None), name)
        press(dialog, 1)
    online = node(1, node(1, node(0)))
    assert online and not node(1, online), "Unexpected Offline tree section"
    choose_menu(main, 104, node(4, room(True)), required=(120,), absent=(133,))
    dialog = wait_for(lambda: find("#32770", "Username and color"), "Edit own username")
    set_text(find_child(dialog, None, "Edit", None), "Listener 18")
    press(dialog, 1)
    changed("profile", name="Listener 18")
    profile_dir = Path(os.environ.get("KANAL_DATA_PATH", Path(os.environ["LOCALAPPDATA"]) / "TinySpeak"))
    wait_for(lambda: json.loads((profile_dir / "settings.json").read_text(encoding="utf-8")).get("name") == "Listener 18",
             "The client saved its renamed profile")
    choose_menu(main, 120, node(4, room(True)), absent=(133,))
    changed("voice_leave")
    wait_for(lambda: node(4, node(1, node(1, node(0)))), "Self moved back to Online")
    choose_menu(main, 104, node(4, node(1, node(1, node(0)))), absent=(120, 133))
    dialog = wait_for(lambda: find("#32770", "Username and color"), "Idle own menu")
    assert get_text(find_child(dialog, None, "Edit", None)) == "Listener 18"
    press(dialog, 2)
    choose_menu(main, 114, room(), required=(116,), absent=(132,))
    edit("New text room", "Extra text")
    changed("room_add", kind="text", name="Extra text")
    wait_for(lambda: node(1, room()), "New text room in tree")
    choose_menu(main, 132, room(True), required=(115, 116))
    dialog = wait_for(lambda: find("#32770", "Room limit"), "Voice capacity dialog")
    choices = find_child(dialog, None, "ComboBox", None)
    assert send(choices, 0x0146, 0, 0) == 16 and send(choices, 0x0147, 0, 0) == 5
    send(choices, 0x014e, 2, 0)
    press(dialog, 1)
    changed("room_limit", room=Relay.voice["id"], limit=3)

    post(child(main, 135), 0x00f5, 0, 0)
    panel = wait_for(lambda: find("#32770", "Server settings"), "Server settings button")
    table = child(panel, 403)
    def click(i):
        post(child(panel, i), 0x00f5, 0, 0)
    def select(index):
        send(table, 0x100, 0x24, 0)  # Home
        for _ in range(index):
            send(table, 0x100, 0x28, 0)
        wait_for(lambda: send(table, 0x100c, -1, 2) == index, "Selected settings row")
    click(404)
    edit("Rename server", "Renamed server")
    wait_for(lambda: get_text(child(panel, 413)) == "Renamed server", "Server rename acknowledged")
    assert "Renamed server" in get_text(child(main, 100))
    select(0)
    assert not visible(child(panel, 408)), "Text room exposed a voice limit"
    click(406)
    edit("New voice room", "Second voice")
    wait_for(lambda: send(table, 0x1004, 0, 0) == 4, "New voice room in settings")
    select(1)
    click(408)
    dialog = wait_for(lambda: find("#32770", "Room limit"), "Voice limit from settings")
    choice = find_child(dialog, None, "ComboBox", None)
    assert send(choice, 0x147, 0, 0) == 2
    send(choice, 0x14e, 15, 0)
    press(dialog, 1)
    changed("room_limit", room=Relay.voice["id"], limit=16)
    click(407)
    edit("Rename room", "Quiet lounge")
    changed("room_rename", room=Relay.voice["id"], name="Quiet lounge")
    select(3)
    click(409)
    confirm = wait_for(lambda: find("#32770", "Delete room"), "Delete room confirmation")
    press(confirm, 6)
    wait_for(lambda: send(table, 0x1004, 0, 0) == 3, "Deleted room disappeared")
    click(402)
    wait_for(lambda: visible(child(panel, 410)) and send(table, 0x1004, 0, 0) == 2, "Member directory")
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "members.txt"
        click(410)
        choose_file("Export server members", path)
        wait_for(lambda: get_text(child(panel, 412)) == "Member list exported." and path.exists(), "Member TXT")
        rows = path.read_text(encoding="utf-8-sig").splitlines()
        assert rows[0] == "Username\tProfile ID"
        assert set(rows[1:]) == {f"{Relay.user['name']}\t{Relay.user['profile_code']}", "Željko\tPublicCode123456"}
    press(panel, 2)


def check_guest_settings(main):
    post(child(main, 135), 0x00f5, 0, 0)
    panel = wait_for(lambda: find("#32770", "Server settings"), "Member access to server settings")
    wait_for(lambda: visible(child(panel, 410)), "Member directory opens for a guest")
    assert not visible(child(panel, 404)) and not visible(child(panel, 411))
    send(child(panel, 401), 0x00f5, 0, 0)
    assert all(not visible(child(panel, i)) for i in (405, 406, 407, 408, 409))
    press(panel, 2)


def inspect_history(main):
    history, composer = child(main, 108), child(main, 109)
    wait_for(lambda: send(history, 0x000E, 0, 0) > 1000, "Messages did not reach the channel view")
    assert style(history, -16) & 0x0800 and not style(history, -16) & 0x10000
    send(history, 0x0007, 0, 0)  # an attempted WM_SETFOCUS must land in the composer
    assert focused(main) == composer
    for msg, buttons, pos in [(0x0201, 1, 0x00080008), (0x0200, 1, 0x00600090),
                              (0x0202, 0, 0x00600090), (0x0203, 1, 0x00600090)]:
        send(history, msg, buttons, pos)
    selection = send(history, 0x00B0, 0, 0)  # EM_GETSEL
    assert selection & 0xffff == selection >> 16 & 0xffff and focused(main) == composer
    send(history, 0x0115, 6, 0)  # WM_VSCROLL, SB_TOP
    first_line = send(history, 0x00CE, 0, 0)
    send(history, 0x020A, (-120 & 0xffff) << 16, 0)
    assert send(history, 0x00CE, 0, 0) > first_line
    send(composer, 0x0102, ord("x"), 0)  # WM_CHAR
    assert send(composer, 0x000E, 0, 0) == 1


def test(client):
    def window():
        return find("TinySpeak.Desktop", None)

    def close_window():
        handle = window()
        if handle:
            post(handle, 0x111, 131, 0)  # Quit TinySpeak, including a tray session
            wait_for(lambda: not window(), "Test client did not close")

    codes = [f"RoomTest{i:08d}" for i in range(1, 4)]
    prefix = "tinyspeak://join/"
    assert not window(), "A TinySpeak session is already open"
    try:
        first = subprocess.Popen([client])
        wait_for(lambda: visible(window()), "First launch did not finish setting up its window")
        profile = Path(os.environ["LOCALAPPDATA"]) / "TinySpeak" / "settings.json"
        settings = json.loads(profile.read_text(encoding="utf-8"))
        assert all(settings[k] for k in ("save_owned", "local_chat", "remember", "remember_color", "close_to_tray"))
        path = r"Software\Classes\tinyspeak"
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
            assert winreg.QueryValueEx(key, "URL Protocol")[0] == ""
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path + r"\shell\open\command") as key:
            assert winreg.QueryValueEx(key, None)[0] == f'"{client}" --join-uri "%1"'
        main = window()
        initial_code = profile_code(main)
        wait_for(lambda: Relay.user.get("profile_code") == initial_code, "Public code agrees with host")
        sidebar_width = check_sidebar(main)
        check_settings(main, True)
        set_slider(main, child(main, 122), 28)  # 120%
        set_slider(main, child(main, 123), 20)  # 80%
        send(main, 0x111, 119, 0)  # unmute
        settings = json.loads(profile.read_text(encoding="utf-8"))
        assert (settings["mic_gain"], settings["output_gain"], settings["mic_muted"]) == (120, 80, False)
        assert settings["normalization_percent"] == 75 and settings["voice_bitrate_kbps"] == 128
        assert settings["ptt"] == ord("V") and settings["sidebar_collapsed"] and settings["sidebar_width_dip"] > 0
        assert send(main, 0x0011, 0, 0) == 1  # WM_QUERYENDSESSION, followed by confirmed Windows shutdown
        post(main, 0x0016, 1, 0)
        assert first.wait(15) == 0
        assert open_url(None, "open", prefix + codes[0], None, None, 1) > 32
        wait_for(lambda: Relay.joined == codes[:1], "Cold link did not join its server")
        main = wait_for(window, "Cold link did not open TinySpeak")
        current_code = profile_code(main)
        assert current_code != initial_code and current_code == Relay.user["profile_code"]
        assert abs(check_sidebar(main, restoring=True) - sidebar_width) <= 1
        check_settings(main, False)
        pid = wt.DWORD()
        window_thread(main, ct.byref(pid))
        assert open_url(None, "open", prefix + codes[1], None, None, 1) > 32
        wait_for(lambda: Relay.joined == codes[:2], "Warm link did not join its server")
        warm_pid = wt.DWORD()
        window_thread(window(), ct.byref(warm_pid))
        assert pid.value == warm_pid.value and window() == main
        wait_for(lambda: codes[1] in get_text(child(main, 100)), "Guest server became active")
        check_guest_settings(main)
        paste_into_join(main, prefix + codes[2])
        paste_into_join(main, codes[2])
        wait_for(lambda: len(Relay.joined) >= 3, "Manual invite code did not join")
        inspect_history(main)
        check_rooms(main)
        with tempfile.TemporaryDirectory() as folder:
            check_files(main, Path(folder))
        if find("Shell_TrayWnd", None):
            post(main, 0x0010, 0, 0)
            wait_for(lambda: not visible(main) and not child(main, 108), "Close did not unload the view into the tray")
            subprocess.run([client.parent / "Launch TinySpeak.exe"], timeout=10, check=True)
            wait_for(lambda: visible(main) and child(main, 108), "Shortcut did not restore the tray session")
            assert profile_code(main) == current_code
            assert get_text(child(main, 109)) == "x" and "Test message 74" in get_text(child(main, 108))
        assert Relay.joined == codes, "The Join box also accepted the URL"
        assert subprocess.run([client, "--join-uri", prefix + codes[0] + "?unexpected"], timeout=10).returncode == 2
        post(main, 0x111, 101, 0)
        dialog = wait_for(lambda: find("#32770", "New server"), "New server dialog")
        set_text(find_child(dialog, None, "Edit", None), "Fresh server")
        press(dialog, 1)
        wait_for(lambda: Relay.layouts, "The owner did not load the new server layout")
        rooms = next(iter(Relay.layouts.values()))
        assert [(r["kind"], r["name"], r.get("limit", 0)) for r in rooms] == [("text", "General", 0), ("voice", "Lounge", 16)]
    finally:
        close_window()


if __name__ == "__main__":
    if os.environ.get("GITHUB_ACTIONS") != "true":
        sys.exit("Use the disposable Windows release runner for the real shell registration test.")
    with ThreadingHTTPServer(("127.0.0.1", 0), Relay) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        os.environ["KANAL_DEMO_ADDRESS"] = f"http://127.0.0.1:{server.server_port}"
        try:
            test(Path(sys.argv[1]).resolve())
        finally:
            server.shutdown()
    print("Passed cold/warm Windows URL activation, code-only joining, chat focus, drag, wheel and typing.")
