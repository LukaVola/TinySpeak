"""Run the installed URL-handler and chat-control checks on a disposable Windows runner."""

import ctypes as ct
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
    user = {"id": "listener", "name": "Listener", "color": 8}
    room = {"id": "ChatRoom0001", "kind": "text", "name": "general"}

    def log_message(self, *args):
        pass

    def do_POST(self):
        command = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        op = command["op"]
        response = {}
        if op == "register":
            response = {"key": "test-session", "user": self.user}
        elif op == "join":
            code = command["code"]
            self.joined.append(code)
            response = {
                "state": {"code": code, "name": "Test server", "owner": self.user["id"],
                          "users": [self.user], "rooms": [self.room]},
                "events": [{"type": "chat", "code": code, "room": self.room["id"], "name": "general",
                            "at": "2026-01-01T10:00:00Z", "sender": "Speaker", "text": f"Test message {i}"}
                           for i in range(75)],
                "version": len(self.joined),
            }
        elif op == "poll":
            time.sleep(0.1)
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
    if title == "Export chat as TXT":
        wait_for(lambda: "TinySpeak-chat" in get_text(field), "Suggested export name")
    send(field, 0x00b1, 0, -1)  # select the previous name, then type as the user would
    for char in str(path):
        send(field, 0x0102, ord(char), 0)
    assert get_text(field) == str(path)
    post(child(dialog, 1), 0x00f5, 0, 0)
    wait_for(lambda: not visible(dialog), "File dialog did not accept the selected path")


class MenuBarInfo(ct.Structure):
    _fields_ = [("size", wt.DWORD), ("rect", wt.RECT), ("menu", wt.HANDLE),
                ("window", wt.HWND), ("flags", wt.DWORD)]


def chat_menu(main, command):
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
            return next((k for k in range(count(menu)) if state(menu, k, 0x0400) & 0x0080), -1)
        if current() < 0:
            post(handle, 0x0100, 0x28, 0)  # Down highlights the first command
            wait_for(lambda: current() >= 0, "Popup keyboard focus")
        for _ in range(count(menu)):
            old = current()
            if old == index:
                return
            post(handle, 0x0100, 0x28, 0)  # Down
            wait_for(lambda: current() != old, "Popup selection did not move")
        raise AssertionError("Popup command was not selectable")

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
    chat_menu(main, 118)
    choose_file("Export chat as TXT", path)
    wait_for(path.exists, "Chat export")
    text = path.read_text(encoding="utf-8-sig")
    assert "Speaker: Test message 0" in text and "Speaker: Test message 74" in text
    chat_menu(main, 129)
    choose_file("Open chat TXT", path)
    viewer = wait_for(lambda: find("#32770", "Saved chat"), "Saved chat viewer")
    assert "Speaker: Test message 74" in get_text(child(viewer, 501))
    press(viewer, 1)
    path.write_text("First ž\n" + "Čćć saved chat\r\n" * 24000 + "Last š", encoding="utf-8-sig")
    chat_menu(main, 129)
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
        check_settings(main, True)
        set_slider(main, child(main, 122), 28)  # 120%
        set_slider(main, child(main, 123), 20)  # 80%
        send(main, 0x111, 119, 0)  # unmute
        settings = json.loads(profile.read_text(encoding="utf-8"))
        assert (settings["mic_gain"], settings["output_gain"], settings["mic_muted"]) == (120, 80, False)
        assert settings["normalization_percent"] == 75 and settings["voice_bitrate_kbps"] == 128
        assert send(main, 0x0011, 0, 0) == 1  # WM_QUERYENDSESSION, followed by confirmed Windows shutdown
        post(main, 0x0016, 1, 0)
        assert first.wait(15) == 0
        assert open_url(None, "open", prefix + codes[0], None, None, 1) > 32
        wait_for(lambda: Relay.joined == codes[:1], "Cold link did not join its server")
        main = wait_for(window, "Cold link did not open TinySpeak")
        check_settings(main, False)
        pid = wt.DWORD()
        window_thread(main, ct.byref(pid))
        assert open_url(None, "open", prefix + codes[1], None, None, 1) > 32
        wait_for(lambda: Relay.joined == codes[:2], "Warm link did not join its server")
        warm_pid = wt.DWORD()
        window_thread(window(), ct.byref(warm_pid))
        assert pid.value == warm_pid.value and window() == main
        paste_into_join(main, prefix + codes[2])
        paste_into_join(main, codes[2])
        wait_for(lambda: len(Relay.joined) >= 3, "Manual invite code did not join")
        inspect_history(main)
        with tempfile.TemporaryDirectory() as folder:
            check_files(main, Path(folder))
        if find("Shell_TrayWnd", None):
            post(main, 0x0010, 0, 0)
            wait_for(lambda: not visible(main) and not child(main, 108), "Close did not unload the view into the tray")
            subprocess.run([client.parent / "Launch TinySpeak.exe"], timeout=10, check=True)
            wait_for(lambda: visible(main) and child(main, 108), "Shortcut did not restore the tray session")
            assert get_text(child(main, 109)) == "x" and "Test message 74" in get_text(child(main, 108))
        assert Relay.joined == codes, "The Join box also accepted the URL"
        assert subprocess.run([client, "--join-uri", prefix + codes[0] + "?unexpected"], timeout=10).returncode == 2
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
