# Product Requirements Document
## Matrice Pad Sound Panel

**Version:** 1.3  
**Status:** Draft

---

## 1. Product Overview

The Matrice Pad Sound Panel is a USB-connected hardware peripheral and companion software system that provides physical media playback controls and a real-time now-playing display for a Windows PC. The system consists of a custom ATmega32U4-based board (the **Panel**) and a background **Windows App** running on the host PC. The Panel connects via USB-C, appears to Windows as both a USB serial device and a HID consumer control device, and requires no driver installation.

---

## 2. Goals

- Give the user tactile, glanceable control over audio playback without switching application focus.
- Display the currently playing track title and artist on the Panel's OLED screen regardless of which audio source is active.
- Mirror the system mute state accurately between the Panel and Windows at all times.
- Give the user an at-a-glance real-time frequency visualization as an alternative to the now-playing text, switchable without touching the PC.

---

## 3. System Architecture

The system has two components that communicate over USB serial at 115200 baud.

| Component | Platform | Role |
|---|---|---|
| Panel Firmware | ATmega32U4 (Arduino Pro Micro) | Drives display, reads controls, sends HID events |
| Windows App | Windows 10/11 | Polls audio state, sends display data to Panel |

The Panel is the HID authority for system volume and mute commands — it sends keystrokes directly to Windows via the HID consumer control interface, entirely independent of the Windows App. The Windows App is the data authority for media metadata, system volume level, mute state, and the frequency bar levels — it pushes all of this to the Panel over serial, unconditionally, every frame. The Panel sends nothing back to the Windows App; there is no app-volume mode or other serial round-trip.

---

## 4. Hardware

| Component | Details |
|---|---|
| Microcontroller | ATmega32U4, 5V/16MHz (SparkFun Pro Micro footprint) |
| Connection | USB-C (USB serial + HID composite device) |
| Display | 128×32 OLED, I²C (SDA=pin 2, SCL=pin 3) |
| Rotary encoder | CLK=pin 20, DT=pin 21, Button=pin 19 |
| Keypad | 2×2 matrix, rows=pins 14/15, columns=pins 10/16 |

---

## 5. Features

### 5.1 Display

**5.1.1 Now-Playing Screen**

The OLED shows the current track title and artist name. Two layout modes are available, selected at compile time:

- **2-Line Mode** (default): textSize 2. Row 0 = track title, Row 1 = artist. Both scroll horizontally when the text exceeds the screen width (~10 chars visible per row).
- **3-Line Mode**: textSize 1. Rows 0–1 = track title word-wrapped (static), Row 2 = artist (scrolls when >21 chars).

Scrolling behavior: text pauses 2 seconds at each end, then scrolls at 40ms per pixel step in a bounce pattern.

**5.1.2 Frequency Bar Graph View**

An alternative full-screen view: 16 vertical bars spanning the display width, each reflecting a log-spaced frequency band's real-time level (0–100%), driven by WASAPI loopback capture and an FFT on the Windows App side. Elapsed/duration (`M:SS/M:SS`) is shown in the upper-right corner when available (browser-based playback only — see 5.3). Toggled by the encoder button (5.2); mute/pause icons overlay this view exactly as they do the now-playing screen.

**5.1.3 Volume Overlay**

When the rotary encoder is turned, the display temporarily shows `Vol: XX%` (system master volume) for 1 second over whichever view (now-playing text or bar graph) is currently active, then reverts to it. System-volume-only — there is no separate app-volume mode.

**5.1.4 Mute Icon**

When the system is muted, a 32×32 status icon is drawn centred on the display (x=64, y=16): a solid white filled circle with a thin black ring inset from its edge, a speaker body, and an X mark drawn in black. The white fill covers any scrolling text or bars within the icon boundary. No text banner — the icon alone is the mute indicator, and OLED contrast is also reduced for a secondary visual cue.

**5.1.5 Pause Icon**

When playback is paused (and the system is not muted), the same 32×32 centred circle (with its inset ring) is drawn with a solid black right-pointing triangle (▶) inside — the standard "press to play" indicator. Mute takes precedence: if both muted and paused, only the mute icon is shown.

**5.1.6 Connection Lost Screen**

If no serial data is received from the Windows App for 5 seconds, the display shows a "No connection / Awaiting update..." message.

**5.1.7 Waiting Screen**

On boot, the display shows "Waiting for data..." until the first valid serial packet is received. The now-playing text view is shown by default.

---

### 5.2 Physical Controls

**Rotary Encoder**

| Turn | Behavior |
|---|---|
| Clockwise | HID `MEDIA_VOLUME_UP` |
| Counter-clockwise | HID `MEDIA_VOLUME_DOWN` |

| Action | Behavior |
|---|---|
| Press button | Toggle the display between the now-playing text view and the frequency bar graph view (5.1.1 / 5.1.2). No banner — the full-screen content swap is its own feedback. |

The encoder operates entirely independently of the Windows App via HID; there is no app-volume mode or serial round-trip.

**Keypad (2×2)**

| Button | Position | HID Event |
|---|---|---|
| M | Top-left | MEDIA_VOLUME_MUTE |
| R | Top-right | MEDIA_PREVIOUS |
| P | Bottom-left | MEDIA_PLAY_PAUSE |
| F | Bottom-right | MEDIA_NEXT |

Keypad controls always send standard HID consumer control events and function without the Windows App running.

---

### 5.3 Windows App — Media Metadata

The Windows App determines the current track title and artist using a priority-based source selection evaluated each polling cycle:

| Condition | Source | Method |
|---|---|---|
| A browser process has an active audio session | WinRT `GlobalSystemMediaTransportControls` | Provides structured title and artist fields |
| A non-browser process has an active audio session | Process window title | Parsed via regex for `Artist - Title` patterns |
| No active audio session (paused or stopped) | Last known WinRT playing session | Retains most recently playing track metadata |
| No session data for ~9 seconds | None | Display is cleared |

**Browser metadata parsing:** WinRT title strings are parsed to separate artist and title, handling:
- YouTube `Artist - Song Title` format
- YouTube Music topic channels (clean WinRT artist field)
- VEVO channel names
- Qualifier stripping: `(Official Music Video)`, `[HD]`, `(feat. ...)`, `- YouTube`, etc.
- Inverted titles where the song phrase appears before the artist

**Window title parsing:** Supports:
- `Artist - Song Title`
- `TrackNumber Artist - Song Title`
- `TrackNumber Artist - Song Title - Application Name` (e.g. VLC)

**Character encoding:** All text is sanitized to 7-bit ASCII before transmission. Common Unicode punctuation (curly quotes, em-dashes, ellipsis) is mapped to ASCII equivalents. Accented characters are decomposed (NFKD) and the accent stripped. Remaining non-ASCII characters are dropped silently.

**Elapsed/duration:** For WinRT (browser) sources only, the service also reads the session's timeline (position/duration). Browsers typically only report position on discrete events (seek/pause/play), not continuously, so the service extrapolates the live position forward using wall-clock time since the last reported update while actively playing — matching how Windows' own Now Playing widget behaves. Both values are 0 when no WinRT timeline is available (non-browser source, nothing playing).

---

### 5.4 Windows App — Audio State

The Windows App polls for:

- **System master volume level** (0–100, integer) — from the default audio endpoint, polled frequently (sub-second) so the Panel's volume overlay (5.1.3) reflects a just-made adjustment rather than a stale value
- **System mute state** (boolean) — from the default audio endpoint

Both are included in every serial packet sent to the Panel, which uses them to keep its display and contrast in sync. There is no active-application-volume tracking — that concept was dropped along with app-volume mode (5.2).

---

### 5.5 Windows App — Frequency Bar Graph

The Windows App continuously captures the default output device's audio via WASAPI loopback and computes a 16-band log-spaced FFT spectrum for the Panel's bar graph view (5.1.2):

- Capture in fixed-size chunks (~1024 samples), mixed to mono, Hann-windowed
- FFT magnitude normalized against the window's energy, converted to dB, clamped to a fixed floor/ceiling and mapped to a 0–100 level per band
- 16 bands log-spaced between roughly 60 Hz and 16 kHz
- Attack/decay smoothing frame-to-frame (instant rise, gradual fall) so bars don't flicker

This capture and the WinRT media-info polling (5.3) each perform their own first-time audio-subsystem initialization; starting them concurrently on separate threads is a known crash risk (observed as a hard native crash in the Python prototype) and must be staggered.

---

### 5.6 Serial Protocol

Messages are newline-terminated (`\n`), ASCII only.

**PC → Panel**

```
song||artist||volume||muted||paused||bar0,bar1,...,bar15||elapsedSec||durationSec\n
```

| Field | Type | Notes |
|---|---|---|
| song | string | Track title, may be empty |
| artist | string | Artist name, may be empty |
| volume | integer | 0–100, system master volume |
| muted | integer | 0 = unmuted, 1 = muted |
| paused | integer | 0 = playing or unknown, 1 = paused |
| bar0..bar15 | integer | 16 frequency-bar levels, 0–100, comma-joined sub-field |
| elapsedSec, durationSec | integer | WinRT-only; both 0 when unavailable |

Sent on every content change, or as a keepalive every 2.5 seconds to prevent the Panel's 5-second connection timeout.

**Panel → PC:** none. The Panel never sends data back — volume/media-key HID events go straight to Windows, and the display-mode toggle (5.2) is entirely local to the Panel.

---

### 5.7 Windows App — Connection Management

- **Auto-detection:** The Panel's USB VID:PID is used to automatically identify the correct COM port. Manual override is supported.
- **Reconnection:** If the serial connection is lost, the service automatically retries with up to 5 attempts and 2-second delays between attempts, then continues retrying indefinitely.
- **Keepalive:** A packet is sent at least every 2.5 seconds regardless of content changes to prevent the Panel from showing the connection-lost screen.

---

## 6. Non-Functional Requirements

- The Windows App must auto-start at user logon (Task Scheduler "At log on" trigger) and run for the duration of the login session; it is a per-user process, not a Windows Service, and does not need to run before login or across user switches.
- CPU and memory usage must be low enough for continuous background operation on a typical desktop PC.
- The Panel controls (HID volume, mute, media keys) must function independently of the Windows App.
- No driver installation required; the Panel must enumerate as a standard HID + CDC composite device.

---

## 7. Out of Scope

- Equalizer or audio routing control
- Multi-device support (more than one Panel connected simultaneously)
- macOS or Linux host support
- Wireless connectivity
- Firmware update mechanism over USB
