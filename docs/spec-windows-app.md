# Software Specification: Windows App
## Matrice Pad Sound Panel — .NET 9

**Version:** 1.3

---

## 1. Overview

The Windows App is a .NET 9 console/background application that runs **per-user, started at logon** -- not a Windows Service. It monitors Windows audio state, retrieves now-playing metadata, captures loopback audio for the Panel's frequency bar graph, and pushes all of it to the Panel over USB serial, unconditionally. It has no UI (aside from an optional tray icon, out of scope for this spec). It never receives data back from the Panel -- HID volume/media-key events and the Panel's display-mode toggle are entirely independent of this app.

**Why not a Windows Service:** an earlier design considered a true LocalSystem Windows Service. A throwaway probe (`SessionZeroProbe`, see below) confirmed that WinRT's `GlobalSystemMediaTransportControlsSessionManager` fails outright when running in Session 0 (`COMException: The specified service does not exist as an installed service.`) -- the API depends on a broker that only activates in an interactive user session. Desktop window enumeration (used for the non-browser title-parsing fallback, section 8) has the same fundamental limitation: there is no desktop to enumerate in Session 0. Interestingly, NAudio's `MMDeviceEnumerator`/default-endpoint access *did* succeed under the same LocalSystem/Session 0 conditions, so the failure is specific to desktop/session-scoped APIs (WinRT media brokering, window enumeration), not audio access broadly. Since the only real benefit of a true service -- working before login / across user switches / with nobody logged in (e.g. an HTPC-style box) -- isn't a requirement here ("runs whenever I'm logged in" is sufficient), the simplest correct architecture is a single per-user process, avoiding a service/helper split and IPC entirely.

---

## 2. Technology Stack

| Concern | Technology |
|---|---|
| App host | .NET 9 console app / generic host (`BackgroundService` under `Host.CreateApplicationBuilder`) |
| Startup | Task Scheduler task with an "At log on" trigger (installer-registered), running as the interactive user |
| WinRT media transport | `Windows.Media.Control` via .NET WinRT interop |
| Audio API | `NAudio.CoreAudioApi` (MMDevice) for volume/mute; `NAudio.Wave.WasapiLoopbackCapture` for the bar graph |
| FFT | `MathNet.Numerics` (or equivalent) for the bar graph's spectrum |
| Window enumeration | P/Invoke: `user32.dll` `EnumWindows`, `GetWindowText`, `IsWindowVisible`; `kernel32.dll` `GetWindowThreadProcessId` |
| Serial port | `System.IO.Ports.SerialPort` |
| COM port detection | `System.IO.Ports.SerialPort.GetPortNames()` + WMI `Win32_PnPEntity` for VID/PID matching |
| Configuration | `appsettings.json` + `IOptions<T>` |
| Logging | `Microsoft.Extensions.Logging` → rolling file sink (no Windows Event Log -- that's a service-oriented sink; a per-user app logs to `%APPDATA%`) |
| DI container | `Microsoft.Extensions.DependencyInjection` (built-in) |

---

## 3. Project Structure

```
MatricePadApp/
├── Program.cs
├── appsettings.json
├── Worker.cs                         # Main BackgroundService loop
├── Services/
│   ├── Interfaces/
│   │   ├── ISerialManager.cs
│   │   ├── IMediaInfoProvider.cs
│   │   ├── IAudioStateProvider.cs
│   │   └── IAudioCaptureProvider.cs
│   ├── SerialManager.cs
│   ├── MediaInfoProvider.cs          # WinRT + window title logic
│   ├── AudioStateProvider.cs         # NAudio volume + mute
│   └── AudioCaptureProvider.cs       # WASAPI loopback + FFT -> bar levels
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
    "MainLoopIntervalMs": 50,
    "SystemVolumePollIntervalMs": 150,
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
    ],
    "AudioCapture": {
      "SampleRate": 44100,
      "ChunkSize": 1024,
      "NumBars": 16,
      "BandMinHz": 60,
      "BandMaxHz": 16000,
      "DbFloor": -60,
      "DbCeiling": 0,
      "DecayStepPerFrame": 8
    }
  }
}
```

`ComPort: null` enables auto-detection. All intervals are in milliseconds. `MainLoopIntervalMs` dropped from 200ms to 50ms (~20fps) to keep the bar graph smooth; `VolumeAudioPollIntervalMs` was split out and shortened to `SystemVolumePollIntervalMs` (150ms) now that the volume overlay needs to reflect a just-made adjustment within its 1-second display window -- app-volume polling is gone entirely along with app-volume mode.

---

## 5. Models

### `MediaInfo`

```csharp
record MediaInfo(
    string Title,
    string Artist,
    string AlbumArtist,
    PlaybackStatus Status,
    int PositionSec,
    int DurationSec
);

enum PlaybackStatus { Unknown = -1, Playing = 4, Paused = 5 }
```

`PositionSec`/`DurationSec` are WinRT-only (0 when unavailable) and `PositionSec` is already wall-clock-extrapolated -- see section 8.

### `AudioState`

```csharp
record AudioState(int Volume, bool IsMuted);
```

No app-volume field -- that concept was dropped along with app-volume mode.

### `SerialPacket`

```csharp
record SerialPacket(string Song, string Artist, int Volume, bool IsMuted, bool Paused, int[] BarLevels, int ElapsedSec, int DurationSec)
{
    public string Encode() =>
        $"{Song}||{Artist}||{Volume}||{(IsMuted ? 1 : 0)}||{(Paused ? 1 : 0)}||{string.Join(',', BarLevels)}||{ElapsedSec}||{DurationSec}\n";
}
```

`BarLevels` is always 16 elements, each 0–100.

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
- Read loop: a background thread calls `ReadLine()` in a loop. Any non-empty line received is raised via `LineReceived` and logged at Debug level -- the Panel has no operational commands to send back, so this exists purely for diagnostic visibility.

---

## 7. `IAudioStateProvider`

```csharp
interface IAudioStateProvider
{
    AudioState GetCurrent();
}
```

**`AudioStateProvider` behavior:**

- On construction: acquire the default audio render endpoint via `MMDeviceEnumerator` targeting `DataFlow.Render` / `Role.Multimedia`.
- `GetCurrent()`: returns cached value. Refreshes cache when `SystemVolumePollIntervalMs` (150ms) has elapsed since last read -- short enough that the Panel's `Vol: XX%` overlay (visible for only 1 second) reflects a just-made adjustment rather than a stale pre-turn value.
- Cache refresh: read `AudioEndpointVolume.MasterVolumeLevelScalar` (map to 0–100 integer) and `AudioEndpointVolume.Mute`. No session enumeration needed -- both are cheap reads on the already-open endpoint interface.

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
6. Read `GetTimelineProperties()` for `Position`/`EndTime`/`LastUpdatedTime`
7. Store result (including `PositionSec`/`DurationSec`, see below) in `_winRtInfo` under lock
8. If `Status == Playing`, also update `_lastPlaying` under lock
9. If result is `null` for `PausedSessionClearCountdown` consecutive polls, clear both `_winRtInfo` and `_lastPlaying` under lock
10. Sleep `WinRtPollIntervalMs`; catch and log all exceptions, continue loop

**Elapsed-time smoothing:** Browsers (Chrome, etc.) generally only call the Media Session position-reporting API on discrete events (seek/pause/play), not continuously -- so `Position` from step 6 is a snapshot as of `LastUpdatedTime`, not a live value, and polling it raw every `WinRtPollIntervalMs` produces a stair-step that jumps once per poll. Two layers of smoothing correct this, matching the Python prototype:
- When building `MediaInfo` in the polling loop (step 7), if `Status == Playing`, extrapolate: `PositionSec = (int)(Position + (UtcNow - LastUpdatedTime)).TotalSeconds`.
- Consumers reading `GetCurrent()` on a faster cadence (e.g. the main worker loop, section 10) should further extrapolate from the last *observed change* using their own wall-clock delta, capped just past one `WinRtPollIntervalMs` so a source that never updates its position at all settles to a small, bounded offset instead of drifting forever.

**Window title polling** (inline, called from main worker loop on its schedule):

1. Enumerate active audio sessions via `NAudio` `AudioSessionManager`
2. For each active session with an associated process:
   - If process name is in `BrowserProcessNames`: return `""` immediately
   - Otherwise enumerate visible windows for that process PID (P/Invoke `EnumWindows`)
   - Collect window titles, excluding entries in `NoisyWindowTitles` (case-insensitive)
   - Return the longest collected title
3. If no active session found: return `"No media playing"`

---

## 9. `IAudioCaptureProvider`

```csharp
interface IAudioCaptureProvider
{
    int[] GetBarLevels();   // always 16 elements, each 0-100
}
```

**`AudioCaptureProvider` behavior** (runs on a dedicated background thread, started with a ~1s delay relative to the WinRT polling loop's startup -- see the concurrency note below):

1. Open the default render device's loopback capture (`WasapiLoopbackCapture`) at `AudioCapture.SampleRate`, buffered in `AudioCapture.ChunkSize`-sample chunks.
2. On each chunk: mix to mono, apply a Hann window, run an FFT, normalize the magnitude by the window's energy.
3. Bin the magnitude spectrum into `AudioCapture.NumBars` log-spaced bands between `BandMinHz` and `BandMaxHz` (aggregate via max magnitude per band).
4. Convert each band to dB, clamp to `[DbFloor, DbCeiling]`, map linearly to a 0–100 level.
5. Smooth frame-to-frame: `level = max(instantLevel, previousLevel - DecayStepPerFrame)` -- instant rise, gradual fall, so bars don't flicker.
6. Store the 16 levels under a lock; `GetBarLevels()` returns a copy.

**Concurrency note:** this provider and `IMediaInfoProvider`'s WinRT polling loop each perform their own first-time audio-subsystem initialization on their own thread. In the Python prototype, starting the equivalent of both at the same instant produced a hard native crash (SIGSEGV) from a COM apartment-initialization race between the loopback-capture library and the WinRT interop layer. Whether .NET's WASAPI/WinRT interop has the same failure mode is unconfirmed, but starting these two background loops with a deliberate stagger (e.g. ~1s) is a cheap precaution worth carrying forward.

---

## 10. Title Parsing

### 10.1 ASCII Sanitization

Applied to all strings before inclusion in a `SerialPacket`:

1. Translate common Unicode punctuation to ASCII equivalents (curly quotes → straight, em-dash → hyphen, ellipsis → `...`, etc.)
2. Unicode NFKD normalization (decomposes accented characters)
3. Encode to ASCII with `EncoderFallback.ReplacementFallback` using empty string (drop non-ASCII)

### 10.2 Window Title Parser

Tries patterns in order, first match wins:

| Pattern | Captures |
|---|---|
| `^\d+ (.*) - (.*) - .*$` | track, artist, song (application name suffix dropped) |
| `^\d+ (.*) - (.*)$` | track, artist, song |
| `^(.*) - (.*)$` | artist, song |

Returns `("", "")` if no pattern matches.

### 10.3 WinRT / YouTube Title Parser

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

## 11. Main Worker Loop (`Worker.cs`)

Runs every `MainLoopIntervalMs` (50ms, ~20fps -- fast enough for a smooth bar graph):

1. Get `AudioState` from `IAudioStateProvider` (`Volume`, `IsMuted`)
2. Get `MediaInfo?` from `IMediaInfoProvider`
3. Get bar levels from `IAudioCaptureProvider.GetBarLevels()`
4. Compute `ElapsedSec`/`DurationSec`: further wall-clock extrapolation of `MediaInfo.PositionSec` per the smoothing note in section 8, and `MediaInfo.DurationSec` (both 0 if `MediaInfo` is null)
5. Build `SerialPacket`:
   - If `MediaInfo` is null: `song = ""`, `artist = ""`
   - Otherwise: apply title parser per source selection, apply ASCII sanitization
   - Always populate `Volume`, `IsMuted` from `AudioState`, `BarLevels` from step 3, `ElapsedSec`/`DurationSec` from step 4
   - Derive `Paused`: `true` when the browser source reports `PlaybackStatus.Paused`, or when the no-active-session case is falling back to `_lastPlaying` (paused persistence); `false` for actively playing or window-title sources
6. If packet content differs from last sent, or `KeepaliveIntervalMs` has elapsed: call `ISerialManager.Send(packet.Encode())`
7. Update last-sent packet and timestamp

There is nothing analogous to the old `APPVOL:+/-` handling -- the Panel sends nothing back, so the main loop is the only producer of serial traffic.

---

## 12. App Lifecycle

- Registered by the installer as a Task Scheduler task with an "At log on" trigger, running as the interactive user (not SYSTEM) -- no elevation needed to run, only to register the task during install.
- Runs as an ordinary user-mode process for the duration of the login session; exits on logoff (nothing needs to persist across logoff, per section 1).
- On process exit (logoff, `Ctrl+C` if run interactively, or Task Scheduler stop): signal the WinRT polling loop and audio capture loop to exit, dispose serial port and loopback capture cleanly via `IHostApplicationLifetime`/`CancellationToken`.

---

## 13. Logging

| Event | Level |
|---|---|
| App start/stop | Information |
| Serial connection established/lost | Information |
| Serial reconnect attempts | Warning |
| Packet sent (content change) | Debug |
| Keepalive sent | Debug |
| WinRT metadata retrieved | Debug |
| WinRT session errors | Warning |
| Audio capture device open/close | Information |
| Audio capture errors (e.g. device disconnected) | Warning |
| Unhandled exceptions in loops | Error |
| Lines received from Panel (diagnostic only, no commands) | Debug |

Target: a rolling file sink under `%APPDATA%\MatricePad\logs\` for Warning and above; optionally Debug in development. No Windows Event Log sink -- that's a service-oriented target and this runs per-user, not as a service.
