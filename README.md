# TinySpeak

TinySpeak is a portable voice and text chat app for small groups on 64-bit Windows. Your PC hosts the server while TinySpeak is open. You and your friends only need a username and an invite.

**[Download Patch 16 for Windows x64 (ZIP)](https://github.com/LukaVola/TinySpeak/releases/download/patch-16/TinySpeak-Windows-x64.zip)**

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
- Choose the input device, output device and microphone mode in **Menu → Settings**. The default push-to-talk key is **F8**.
- **Microphone threshold** suppresses microphone audio below the chosen level in any microphone mode. **30 dB** is the default, **70 dB** is the maximum, and **0 dB** turns the gate off. The setting uses a digital signal scale starting at −70 dBFS. A setting of 30 dB corresponds to −40 dBFS RMS. **OK** saves the setting locally.
- **Microphone bitrate** selects a target of **24, 32, 48, 64, 80 or 128 kbps** for your outgoing Opus audio. The default is **32 kbps - normal quality**. Changing it during a call briefly reconnects your audio in the same room.

## Servers, invites and logs

Right-click your voice room or open text channel to leave it. Owners can also rename rooms and channels, and remove someone from a voice room while keeping them on the server.

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
