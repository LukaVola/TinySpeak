# TinySpeak

TinySpeak is a portable voice and text chat app for small groups on 64-bit Windows. Your PC hosts the server while TinySpeak is open. You and your friends only need a username and an invite.

**[Download Patch 11 for Windows x64 (ZIP)](https://github.com/LukaVola/TinySpeak/releases/download/patch-11/TinySpeak-Windows-x64.zip)**

## Start a conversation

1. Download the Windows ZIP above and extract the entire **TinySpeak** folder somewhere you can write to. Open **Launch TinySpeak.exe**. The ZIP includes the voice libraries, host and Windows C++ runtime files.
2. To host, choose **New server**, then **Menu → Copy invite code**. Send the full code to your friends.
3. To join, open TinySpeak, click **Join** and paste the code. Double-click the same voice room to talk. The microphone button controls mute. Press **Enter** to send a text message.

TinySpeak runs as a standard Windows user. Only the owner runs **TinySpeak Host.exe**, which handles both incoming chat and voice. Windows Firewall may ask the owner to allow incoming connections. On managed PCs, an administrator may need to approve this. When updating, close TinySpeak and extract the new version over the same folder so path-based firewall rules continue to apply.

The host's TinySpeak must stay open. The app tries to open a direct IPv4 route using the router's UPnP support. A publicly reachable host can also accept direct connections. CGNAT, unsupported port mapping, and firewall restrictions can limit the invite to the local network. TinySpeak does not require a VPN for direct connections.

The complete host invite contains the server tag, the host address and a certificate fingerprint that authenticates the host. Share a fresh invite if the host changes networks or its IP address changes. The server tag displayed next to the server name stays 16 characters.

## Audio controls

- A quiet rising tone plays when you or someone else joins your voice room or active chat server. A falling tone plays when someone leaves. These tones follow the app output volume and play through the default Windows output device.
- The microphone icon in the footer toggles mute. A red slash shows when the microphone is muted.
- The footer shows the microphone gain and app output volume as percentages. The thin meter below the microphone slider shows the input signal level, even while muted.
- **5% volume steps (1% from 0% to 5%)** is on by default. It applies to the microphone, app output and participant volume. The microphone is capped at 150% and the app output at 110%. Turn this option off for 1% steps across the full range.
- Right-click a person in your voice room to **Mute for me**, **Unmute for me**, or adjust **Participant volume**. The volume slider previews the change, **OK** saves it, and **Cancel** restores the previous level. Unmuting restores the saved level.
- Saved levels apply only to your playback. TinySpeak remembers the person across reconnects and username changes when both clients and the relay are on Patch 3 or later. For older clients, TinySpeak uses the name within the current server.
- Choose the input device, output device and microphone mode in **Menu → Settings**. The default push-to-talk key is **F8**.
- **Microphone threshold** suppresses microphone audio below the chosen level in any microphone mode. **30 dB** is the default, **70 dB** is the maximum, and **0 dB** turns the gate off. The setting uses a digital signal scale starting at −70 dBFS. A setting of 30 dB corresponds to −40 dBFS RMS. **OK** saves the setting locally.
- **Microphone bitrate** selects a target of **24, 32, 48, or 64 kbps** for your outgoing Opus audio. The default is **32 kbps - normal quality**. Changing it during a call briefly reconnects your audio in the same room.

## Servers, invites and logs

Right-click your voice room or open text channel to leave it. Owners can also rename rooms and channels, and remove someone from a voice room while keeping them on the server.

Owners can save servers, room layouts and chat logs locally. Guests can save their own logs, then open, export or delete them under **Menu → Local chat logs**. The chat view groups messages by day, and the TXT export keeps full timestamps.

Direct hosting uses the existing `%LOCALAPPDATA%\TinySpeak\LokalnaProba` profile. Personal playback levels and mutes are stored in `voice-preferences.json`. The profile also stores the host identity used to authenticate invitations.

## Build

For a source build, use an x64 Visual Studio Developer Command Prompt, MSVC with C++20, the Windows SDK and Go 1.26 or newer. Download [LiveKit C++ SDK 1.13.1 for Windows x64](https://github.com/livekit/client-sdk-cpp/releases/tag/v1.13.1), [nlohmann/json 3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0) and [miniaudio 0.11.23](https://github.com/mackron/miniaudio/tree/0.11.23). Place `json.hpp` and `miniaudio.h` in one directory. From the repository root, run the following commands with your own absolute paths.

```bat
client\build.cmd C:\dev\livekit-sdk C:\dev\headers C:\dev\tinyspeak-bin
server\build-windows.cmd C:\dev\tinyspeak-bin
```

For a portable build, copy the x64 MSVC redistributable files `msvcp140.dll`, `msvcp140_atomic_wait.dll`, `vcruntime140.dll` and `vcruntime140_1.dll` into the output directory alongside the two LiveKit DLLs. Run `go test ./...` and `go test -tags embedded_voice ./...` in `server` to test both helper variants. Third-party notices are in `Licenses`.
