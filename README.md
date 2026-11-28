# <img src="TinySpeak.png" width="40" height="40" alt="TinySpeak microphone icon"> TinySpeak

TinySpeak is a lightweight, privacy-focused voice and text chat app for friends and small groups of people you trust. It runs on 64-bit Windows and hosts your server on your own PC. You join with a username and an invite, and any chat history you choose to keep is stored locally.

TinySpeak is designed for fast, stable conversations on older, low-spec PCs. In a three-computer voice call tested by the project owner, TinySpeak ran smoothly on a PC with an Intel Core i3-6100 and 2 GB of RAM.

**AI disclosure.** AI tools were used for a substantial part of this project, including building the user interface and optimizing the app to keep CPU and memory use low.

> **Share invites only with people you trust.** TinySpeak connects devices directly over the Internet when possible. On a direct connection, peers can see each other's public IP addresses. Anyone with a working invite can try to join, so send invites privately and avoid posting them in public channels or forums.

**[Download Patch 20 for Windows x64 (ZIP)](https://github.com/LukaVola/TinySpeak/releases/download/patch-20/TinySpeak-Windows-x64.zip)**

## Start a conversation

1. Download the Windows ZIP above and extract the entire **TinySpeak** folder somewhere you can write to. Open **Launch TinySpeak.exe**. The ZIP includes the voice libraries, host and Windows C++ runtime files.
2. To host, choose **New server**, then **Menu → Copy invite code**. Send the full code to your friends. For Internet invites, everyone should use Patch 12 or newer.
3. To join with a code, open TinySpeak, click **Join** and paste the complete code. With **Menu → Copy invite link**, you can instead send a `tinyspeak://` link. Clicking the link in a compatible app opens TinySpeak and joins the server. Each person opens TinySpeak once after extracting it so Windows can register the link for their account. A browser may ask for confirmation before opening an external app.
4. Double-click the same voice room to talk. The microphone button controls mute. Read or scroll through the channel history, then type in the message box at the bottom and press **Enter** to send.

TinySpeak runs as a standard Windows user. **TinySpeak Host.exe** runs the rooms, chat and voice on the owner's PC. **TinySpeak Link.exe** connects peers over the Internet. Windows Firewall may ask about network access. When updating, close TinySpeak and extract the new version over the same folder.

The host keeps TinySpeak open during the conversation. Friends on different networks paste the same invite. The bundled iroh transport uses a direct connection when possible and can carry both text and voice through a public relay if the router blocks incoming connections. The relay forwards encrypted traffic. It can see connection metadata such as IP addresses and traffic volume. The public relays have rate limits and no uptime guarantee. [About the iroh relay network](https://www.iroh.computer/services/hosting)

The full Internet invite starts with **TS2-** and contains the information needed to reach and authenticate the host. The 16-character server tag identifies the server inside TinySpeak. Existing **TS1-** invites still work on their original direct routes. After updating a host from Patch 11, copy a fresh invite. If the public relay is unavailable at startup, TinySpeak can still offer its direct or LAN invite.

## Audio controls

- A quiet rising tone plays when you or someone else joins your voice room or active chat server. A falling tone plays when someone leaves. These tones follow the app output volume and play through the default Windows output device.
- The microphone icon in the footer toggles mute. A red slash shows when the microphone is muted.
- The footer shows the microphone gain and app output volume as percentages. The thin meter below the microphone slider shows the input signal level, even while muted.
- **5% volume steps (1% from 0% to 5%)** is on by default. It applies to the microphone, app output and participant volume. The microphone is capped at 150% and the app output at 110%. Turn this option off for 1% steps across the full range.
- Right-click a person in your voice room to **Mute for me**, **Unmute for me**, or adjust **Participant volume** from **0% to 150%**. The volume slider previews the change, **OK** saves it, and **Cancel** restores the previous level. Unmuting restores the saved level.
- Open **Menu → Settings → Playback equalizer and normalization…** to adjust four incoming voice bands from **−12 to +12 dB**. The bands cover 1–200 Hz, 201–400 Hz, 401–1,000 Hz and 1,001–20,000 Hz. All four start at 0 dB.
- **Voice normalization strength** limits sudden sounds such as a microphone knock. Choose 0% (off), 25%, 50%, 75% or 100%. Higher settings allow smaller peaks above the recent speech level. A new profile starts at 0%. An existing enabled limiter becomes 100%. The EQ and strength preview live and are saved when you confirm both dialogs.
- Saved levels apply only to your playback. TinySpeak remembers the person across reconnects and username changes when both clients and the relay are on Patch 3 or later. For older clients, TinySpeak uses the name within the current server.
- Choose the input device, output device and microphone mode in **Menu → Settings**. For push-to-talk, click the current binding and press a keyboard key or a mouse button. **F8** is the default.
- **Microphone threshold** suppresses microphone audio below the chosen level in any microphone mode. **30 dB** is the default, **70 dB** is the maximum, and **0 dB** turns the gate off. The setting uses a digital signal scale starting at −70 dBFS. A setting of 30 dB corresponds to −40 dBFS RMS. **OK** saves the setting locally.
- **Microphone bitrate** selects a target of **24, 32, 48, 64, 80 or 128 kbps** for your outgoing Opus audio. The default is **32 kbps**. Changing it during a call briefly reconnects your audio in the same room.

## Servers, invites and logs

Each new server starts with **General** and **Lounge**. Open **Server settings** to see the rooms. The owner can rename the server, create, rename or delete rooms, and set a voice room limit from **1 to 16**. A voice room with fewer than 16 places shows its limit in gray at the right of its name. Text rooms are open to everyone in the server, including rooms saved by Patch 17. The room context menus and the main **Menu** also provide management commands.

Drag the divider beside the room list to resize it. The arrow beside **Rooms** collapses it to a narrow rail. TinySpeak remembers the width and the collapsed state.

**Server settings → Members** lists current and previous members by username and their last known public profile ID. **Export as TXT…** saves those two columns locally. The host keeps the roster with the saved server. Each launch generates a fresh 16-character profile code. **Username and color** displays this code as plain text. Reconnecting or restoring TinySpeak from the tray keeps the same code. A returning member updates the existing row and retains their personal playback settings.

Right-click your own name for **Change username…** and, during a call, **Leave voice room**. The small dot beside a member's profile color is green when they speak, red when their microphone is muted or you have muted them, and white when quiet.

A new installation saves owned servers, room layouts, bookmarks and local chat logs by default. Existing profiles keep their choices. Only messages received by that computer are archived. Under **Menu → Chat logs**, **Export chat as TXT…** saves the channel’s local log as a UTF-8 TXT file, or the current chat view when logging is off. **Open chat TXT…** directly below it opens a saved file, even when disconnected. Large logs have **Previous page** and **Next page** buttons. **Local chat logs…** in the same submenu also lets you read, export or delete automatic archives.

Closing the window keeps TinySpeak in the Windows system tray by default. Calls, hosting and local logging continue while the window controls are unloaded. Click the microphone tray icon or open the desktop shortcut to return. Right-click the icon and choose **Quit TinySpeak** for a full exit. You can also quit from **Menu** or disable close-to-tray in **Settings**.

Direct hosting uses the existing `%LOCALAPPDATA%\TinySpeak\LokalnaProba` profile. Personal playback levels and mutes are stored in `voice-preferences.json`. The profile also stores the host identity used to authenticate invitations.

## Build

For a source build, use an x64 Visual Studio Developer Command Prompt, MSVC with C++20, the Windows SDK and Go 1.26 or newer. Download [LiveKit C++ SDK 1.13.1 for Windows x64](https://github.com/livekit/client-sdk-cpp/releases/tag/v1.13.1), [nlohmann/json 3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0) and [miniaudio 0.11.23](https://github.com/mackron/miniaudio/tree/0.11.23). Place `json.hpp` and `miniaudio.h` in one directory. From the repository root, run the following commands with your own absolute paths.

```bat
client\build.cmd C:\dev\livekit-sdk C:\dev\headers C:\dev\tinyspeak-bin
server\build-windows.cmd C:\dev\tinyspeak-bin
```

For a portable build, copy the x64 MSVC redistributable files `msvcp140.dll`, `msvcp140_atomic_wait.dll`, `vcruntime140.dll` and `vcruntime140_1.dll` into the output directory alongside the two LiveKit DLLs. Run `go test ./...` and `go test -tags embedded_voice ./...` in `server` to test both helper variants. Third-party notices are in `Licenses`.
