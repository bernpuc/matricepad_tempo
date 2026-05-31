# Product Requirements Document
## Matrice Pad Sound Panel

**Version:** 1.0  
**Status:** Draft

---

## 1. Product Overview

The Matrice Pad Sound Panel is a USB-connected hardware peripheral and companion software system that provides physical media playback controls and a real-time now-playing display for a Windows PC. The system consists of a custom ATmega32U4-based board (the **Panel**) and a background **Windows Service** running on the host PC. The Panel connects via USB-C, appears to Windows as both a USB serial device and a HID consumer control device, and requires no driver installation.

---

## 2. Goals

- Give the user tactile, glanceable control over audio playback without switching application focus.
- Display the currently playing track title and artist on the Panel's OLED screen regardless of which audio source is active.
- Mirror the system mute state accurately between the Panel and Windows at all times.

---

## 3. System Architecture

The system has two components that communicate over USB serial at 115200 baud.

| Component | Platform | Role |
|---|---|---|
| Panel Firmware | ATmega32U4 (Arduino Pro Micro) | Drives display, reads controls, sends HID events |
| Windows Service | Windows 10/11 | Polls audio state, sends display data to Panel |

The Panel is the HID authority for volume and mute commands — it sends keystrokes directly to Windows via the HID consumer control interface. The Windows Service is the data authority for media metadata, system volume level, and mute state — it pushes this to the Panel over serial.

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

**5.1.2 Volume Overlay**

When the rotary encoder is turned, the display temporarily shows `Vol: XX%` (textSize 2) for 1 second, then returns to the now-playing screen.

**5.1.3 Mute Overlay**

When the encoder button or the keypad mute key is pressed, the display shows `MUTE` or `UNMUTE` (textSize 3) for 1 second, then returns to the now-playing screen. The OLED contrast is also reduced when muted to provide a visual indicator on the now-playing screen.

**5.1.4 Mute Icon**

A 16×16 speaker-with-X bitmap is overlaid on the now-playing screen whenever the system is muted.

**5.1.5 Connection Lost Screen**

If no serial data is received from the Windows Service for 5 seconds, the display shows a "No connection / Awaiting update..." message.

**5.1.6 Waiting Screen**

On boot, the display shows "Waiting for data..." until the first valid serial packet is received.

---

### 5.2 Physical Controls

**Rotary Encoder**

| Action | Behavior |
|---|---|
| Turn clockwise | HID MEDIA_VOLUME_UP (one step per detent) |
| Turn counter-clockwise | HID MEDIA_VOLUME_DOWN (one step per detent) |
| Press button | HID MEDIA_VOLUME_MUTE; toggles mute icon and display contrast |

**Keypad (2×2)**

| Button | Position | HID Event |
|---|---|---|
| M | Top-left | MEDIA_VOLUME_MUTE |
| R | Top-right | MEDIA_PREVIOUS |
| P | Bottom-left | MEDIA_PLAY_PAUSE |
| F | Bottom-right | MEDIA_NEXT |

All controls send standard HID consumer control events and function without the Windows Service running.

---

### 5.3 Windows Service — Media Metadata

The Windows Service determines the current track title and artist using a priority-based source selection evaluated each polling cycle:

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

---

### 5.4 Windows Service — Audio State

The Windows Service polls the Windows default audio endpoint (speakers) for:

- **Master volume level** (0–100, integer)
- **Mute state** (boolean)

Both are included in every serial packet sent to the Panel. The Panel uses these to keep its local mute state and volume display in sync with the actual system state on every update.

Poll interval: 2 seconds.

---

### 5.5 Serial Protocol

Messages are newline-terminated (`\n`), ASCII only.

**PC → Panel**

```
song||artist||volume||muted\n
```

| Field | Type | Notes |
|---|---|---|
| song | string | Track title, may be empty |
| artist | string | Artist name, may be empty |
| volume | integer | 0–100 |
| muted | integer | 0 = unmuted, 1 = muted |

Sent on every content change, or as a keepalive every 2.5 seconds to prevent the Panel's 5-second connection timeout.

**Panel → PC**

No operational messages. Any unexpected output from the Panel is logged by the Windows Service.

---

### 5.6 Windows Service — Connection Management

- **Auto-detection:** The Panel's USB VID:PID is used to automatically identify the correct COM port. Manual override is supported.
- **Reconnection:** If the serial connection is lost, the service automatically retries with up to 5 attempts and 2-second delays between attempts, then continues retrying indefinitely.
- **Keepalive:** A packet is sent at least every 2.5 seconds regardless of content changes to prevent the Panel from showing the connection-lost screen.

---

## 6. Non-Functional Requirements

- The Windows Service must run as a Windows Service (auto-start, no user session required after initial setup).
- CPU and memory usage must be low enough for continuous background operation on a typical desktop PC.
- The Panel controls (HID volume, mute, media keys) must function independently of the Windows Service.
- No driver installation required; the Panel must enumerate as a standard HID + CDC composite device.

---

## 7. Out of Scope

- Equalizer or audio routing control
- Multi-device support (more than one Panel connected simultaneously)
- macOS or Linux host support
- Wireless connectivity
- Firmware update mechanism over USB
