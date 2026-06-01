# Software Specification: Windows Service
## Matrice Pad Sound Panel — .NET 9

**Version:** 1.1

---

## 1. Overview

The Windows Service is a .NET 9 Worker Service that runs as a Windows background service. It monitors Windows audio state, retrieves now-playing metadata, and pushes data to the Panel over USB serial. It has no UI.

---

## 2. Technology Stack

| Concern | Technology |
|---|---|
| Service host | .NET 9 Worker Service (`BackgroundService`) |
| Windows Service integration | `UseWindowsService()` |
| WinRT media transport | `Windows.Media.Control` via .NET WinRT interop |
| Audio API | `NAudio.CoreAudioApi` (MMDevice, AudioSessionManager) |
| Window enumeration | P/Invoke: `user32.dll` `EnumWindows`, `GetWindowText`, `IsWindowVisible`; `kernel32.dll` `GetWindowThreadProcessId` |
| Serial port | `System.IO.Ports.SerialPort` |
| COM port detection | `System.IO.Ports.SerialPort.GetPortNames()` + WMI `Win32_PnPEntity` for VID/PID matching |
| Configuration | `appsettings.json` + `IOptions<T>` |
| Logging | `Microsoft.Extensions.Logging` → Windows Event Log |
| DI container | `Microsoft.Extensions.DependencyInjection` (built-in) |

---

## 3. Project Structure

```
MatricePadService/
├── Program.cs
├── appsettings.json
├── Worker.cs                         # Main BackgroundService loop
├── Services/
│   ├── Interfaces/
│   │   ├── ISerialManager.cs
│   │   ├── IMediaInfoProvider.cs
│   │   └── IAudioStateProvider.cs
│   ├── SerialManager.cs
│   ├── MediaInfoProvider.cs          # WinRT + window title logic
│   └── AudioStateProvider.cs         # NAudio volume + mute
├── Models/
│   ├── MediaInfo.cs
│   ├── AudioState.cs
│   └── SerialPacket.cs
└── Native/
    └── Win32.cs                      # P/Invoke declarations
```

---

## 4. Configuration (`appsettings.json`)

```json
{
  "MatricePad": {
    "ComPort": null,
    "BaudRate": 115200,
    "KeepaliveIntervalMs": 2500,
    "ConnectionTimeoutMs": 5000,
    "ReconnectDelayMs": 2000,
    "MainLoopIntervalMs": 200,
    "VolumeAudioPollIntervalMs": 2000,
    "WindowTitlePollIntervalMs": 1000,
    "WinRtPollIntervalMs": 3000,
    "PausedSessionClearCountdown": 3,
    "ArduinoVidPidList": [
      "1B4F:9206", "1B4F:9205", "2341:8036"
    ],
    "BrowserProcessNames": [
      "chrome", "firefox", "msedge", "opera"
    ],
    "NoisyWindowTitles": [
      "libvlcsharp.wpf", "libvlcsharp", "vlcsharp"
    ]
  }
}
```

`ComPort: null` enables auto-detection. All intervals are in milliseconds.

---

## 5. Models

### `MediaInfo`

```csharp
record MediaInfo(
    string Title,
    string Artist,
    string AlbumArtist,
    PlaybackStatus Status
);

enum PlaybackStatus { Unknown = -1, Playing = 4, Paused = 5 }
```

### `AudioState`

```csharp
record AudioState(int Volume, bool IsMuted, int AppVolume);
```

`AppVolume` is the active audio session's volume (0–100); 0 if no active session.

### `SerialPacket`

```csharp
record SerialPacket(string Song, string Artist, int Volume, bool IsMuted, int AppVolume)
{
    public string Encode() =>
        $"{Song}||{Artist}||{Volume}||{(IsMuted ? 1 : 0)}||{AppVolume}\n";
}
```

---

## 6. `ISerialManager`

```csharp
interface ISerialManager
{
    bool IsConnected { get; }
    void Send(string data);
    event EventHandler<string> LineReceived;
}
```

**`SerialManager` behavior:**

- On `Send()`: if not connected, attempt to connect first. Write ASCII-encoded bytes.
- Connection establishment: iterate known VID:PID pairs via WMI `Win32_PnPEntity` to find the COM port. If `ComPort` is set in config, use it directly. Retry up to 5 times with `ReconnectDelayMs` delay. Wait 2 seconds after opening before sending.
- If a `SerialException` is thrown during `Send()`, close the port and mark as disconnected. The next `Send()` call will reconnect.
- Read loop: a background thread calls `ReadLine()` in a loop. Any non-empty line received is raised via `LineReceived`.
- `LineReceived` subscribers handle operational messages:
  - `APPVOL:+` — increase active session volume by 2%
  - `APPVOL:-` — decrease active session volume by 2%
  - All other lines are logged at Debug level.

---

## 7. `IAudioStateProvider`

```csharp
interface IAudioStateProvider
{
    AudioState GetCurrent();
    void AdjustAppVolume(int delta);
}
```

**`AudioStateProvider` behavior:**

- On construction: acquire the default audio render endpoint via `MMDeviceEnumerator` targeting `DataFlow.Render` / `Role.Multimedia`.
- `GetCurrent()`: returns cached value. Refreshes cache when `VolumeAudioPollIntervalMs` has elapsed since last read.
- Cache refresh: read `AudioEndpointVolume.MasterVolumeLevelScalar` (map to 0–100 integer), `AudioEndpointVolume.Mute`, and the active session's `SimpleAudioVolume.MasterVolume` (map to 0–100 integer). Active session is the first `AudioSessionState.Active` session with an associated process.
- `AdjustAppVolume(int delta)`: find the active session, read its current `SimpleAudioVolume.MasterVolume`, add `delta / 100.0f`, clamp to `[0.0, 1.0]`, write back. Invalidates the app volume cache so the next `GetCurrent()` reflects the change immediately.

---

## 8. `IMediaInfoProvider`

```csharp
interface IMediaInfoProvider
{
    MediaInfo? GetCurrent();
}
```

**`MediaInfoProvider` behavior:**

Maintains two internal fields updated by background work:

- `_winRtInfo`: updated by the WinRT polling loop every `WinRtPollIntervalMs`
- `_lastPlaying`: last `_winRtInfo` where `Status == Playing`
- `_windowTitle`: updated by window title polling every `WindowTitlePollIntervalMs`

`GetCurrent()` applies source priority and returns a `MediaInfo?`:

| Condition | Source |
|---|---|
| `_windowTitle == ""` (browser active) | `_winRtInfo` |
| `_windowTitle == "No media playing"` | `_lastPlaying` (paused persistence) |
| `_windowTitle` is any other string | Window title parser |

Returns `null` when no source yields a title (display cleared).

**WinRT polling loop** (runs on a dedicated `Task` with its own `SynchronizationContext`):

1. Call `GlobalSystemMediaTransportControlsSessionManager.RequestAsync()`
2. Iterate all sessions; select the first with `PlaybackStatus.Playing`
3. If none found, fall back to `GetCurrentSession()`
4. Call `TryGetMediaPropertiesAsync()` on the chosen session
5. Read `PlaybackStatus` from `GetPlaybackInfo()`
6. Store result in `_winRtInfo` under lock
7. If `Status == Playing`, also update `_lastPlaying` under lock
8. If result is `null` for `PausedSessionClearCountdown` consecutive polls, clear both `_winRtInfo` and `_lastPlaying` under lock
9. Sleep `WinRtPollIntervalMs`; catch and log all exceptions, continue loop

**Window title polling** (inline, called from main worker loop on its schedule):

1. Enumerate active audio sessions via `NAudio` `AudioSessionManager`
2. For each active session with an associated process:
   - If process name is in `BrowserProcessNames`: return `""` immediately
   - Otherwise enumerate visible windows for that process PID (P/Invoke `EnumWindows`)
   - Collect window titles, excluding entries in `NoisyWindowTitles` (case-insensitive)
   - Return the longest collected title
3. If no active session found: return `"No media playing"`

---

## 9. Title Parsing

### 9.1 ASCII Sanitization

Applied to all strings before inclusion in a `SerialPacket`:

1. Translate common Unicode punctuation to ASCII equivalents (curly quotes → straight, em-dash → hyphen, ellipsis → `...`, etc.)
2. Unicode NFKD normalization (decomposes accented characters)
3. Encode to ASCII with `EncoderFallback.ReplacementFallback` using empty string (drop non-ASCII)

### 9.2 Window Title Parser

Tries patterns in order, first match wins:

| Pattern | Captures |
|---|---|
| `^\d+ (.*) - (.*) - .*$` | track, artist, song (application name suffix dropped) |
| `^\d+ (.*) - (.*)$` | track, artist, song |
| `^(.*) - (.*)$` | artist, song |

Returns `("", "")` if no pattern matches.

### 9.3 WinRT / YouTube Title Parser

Input: raw WinRT `title` string + `artist` string.

1. If title matches a non-music pattern (lofi, playlist, mix, live stream, 24/7, etc.): return `(artist, stripped_title)`
2. Strip qualifier suffixes: `(Official Music Video)`, `[Official Video]`, `(Official Audio)`, `(Lyric Video)`, `[4K...]`, `[HD]`, `(Full Album)`, `(feat. ...)`, `(ft. ...)`, `ft. ...` at end, `- YouTube` / `- YouTube Music` at end
3. If no ` - ` delimiter in stripped title and artist is non-empty and not a VEVO name: return `(artist, title)` (Topic channel / YouTube Music)
4. If no ` - ` delimiter: return `(artist, title)`
5. Split on first ` - `: `left` / `right`
6. Apply qualifier stripping to `right`
7. If `left` does not look like an artist name, looks like a song phrase, and `right` looks like an artist name: return `(right, left)` (inverted format)
8. Default: return `(left, right)`

**Artist name heuristics:** contains `&`, `+`, `and`, `feat.`, `ft.`; starts with `The <Capital>`; single all-caps word ≥3 chars; 1–3 words all title-cased.

**Song phrase heuristics:** starts with common song-phrase starters (`In The`, `Somebody`, `What `, etc.) with ≥3 words; or ≥5 words total.

---

## 10. Main Worker Loop (`Worker.cs`)

Runs every `MainLoopIntervalMs` (200ms):

1. Get `AudioState` from `IAudioStateProvider` (includes `Volume`, `IsMuted`, `AppVolume`)
2. Get `MediaInfo?` from `IMediaInfoProvider`
3. Build `SerialPacket`:
   - If `MediaInfo` is null: `song = ""`, `artist = ""`
   - Otherwise: apply title parser per source selection, apply ASCII sanitization
   - Always populate `Volume`, `IsMuted`, `AppVolume` from `AudioState`
4. If packet content differs from last sent, or `KeepaliveIntervalMs` has elapsed: call `ISerialManager.Send(packet.Encode())`
5. Update last-sent packet and timestamp

`APPVOL:+` / `APPVOL:-` messages from the Panel are handled on the serial read thread via the `LineReceived` event → `IAudioStateProvider.AdjustAppVolume(±2)`. This is independent of the main loop.

---

## 11. Service Lifecycle

- Installed and managed via `sc.exe` or PowerShell `New-Service`
- Start type: Automatic (delayed)
- `UseWindowsService()` handles `OnStart`/`OnStop` integration
- On `StopAsync`: signal the WinRT polling loop to exit, dispose serial port cleanly

---

## 12. Logging

| Event | Level |
|---|---|
| Service start/stop | Information |
| Serial connection established/lost | Information |
| Serial reconnect attempts | Warning |
| Packet sent (content change) | Debug |
| Keepalive sent | Debug |
| WinRT metadata retrieved | Debug |
| WinRT session errors | Warning |
| Unhandled exceptions in loops | Error |
| `APPVOL:+/-` received, session adjusted | Debug |
| `APPVOL:+/-` received, no active session | Warning |
| Lines received from Panel (other) | Debug |

Target: Windows Event Log (`Application` source `MatricePadService`) for Warning and above; optionally a rolling file sink for Debug in development.
