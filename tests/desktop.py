"""Run the installed URL-handler and chat-control checks on a disposable Windows runner."""

import ctypes as ct
from ctypes import wintypes as wt
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import sys
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
            post(handle, 0x0010, 0, 0)
            wait_for(lambda: not window(), "Test client did not close")

    codes = [f"RoomTest{i:08d}" for i in range(1, 4)]
    prefix = "tinyspeak://join/"
    assert not window(), "A TinySpeak session is already open"
    try:
        first = subprocess.Popen([client])
        wait_for(lambda: visible(window()), "First launch did not finish setting up its window")
        path = r"Software\Classes\tinyspeak"
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
            assert winreg.QueryValueEx(key, "URL Protocol")[0] == ""
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path + r"\shell\open\command") as key:
            assert winreg.QueryValueEx(key, None)[0] == f'"{client}" --join-uri "%1"'
        close_window()
        assert first.wait(15) == 0
        assert open_url(None, "open", prefix + codes[0], None, None, 1) > 32
        wait_for(lambda: Relay.joined == codes[:1], "Cold link did not join its server")
        main = wait_for(window, "Cold link did not open TinySpeak")
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
