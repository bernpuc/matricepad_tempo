# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MatricePad Tempo is a two-component embedded system:

- **Arduino firmware** (`arduino/matrice_pad_tempo/matrice_pad_tempo.ino`) — runs on the Matrice Pro board. Drives a 128×32 OLED, reads a rotary encoder for volume control, handles a 2×2 keypad matrix that sends HID consumer media keys, and communicates with the PC over USB serial at 115200 baud.
- **Python host script** (`template.py`) — runs on Windows. Polls Windows audio state via WinRT and pycaw, sends media info to the Arduino, and handles incoming volume/mute commands from the Arduino.

## Code Organization

Shared logic lives in two places so feature-specific variants (e.g. a duration/elapsed-time display, a frequency bar graph) can be added as their own sketch/script pair without duplicating the boring 80%:

- **`arduino/libraries/TempoCore/`** — an in-repo Arduino library (not the sketchbook). Holds `ScrollText` (the marquee scroll engine), `StatusIcons` (mute/pause circle glyph + overlay banner drawing), and `SerialFraming` (null-terminated `\|\|`-delimited field parsing). `arduino/matrice_pad_tempo/matrice_pad_tempo.ino` is the baseline sketch built on top of it; future feature sketches go in sibling folders under `arduino/` and `#include <TempoCore.h>` the same way.
- **`tempo_core/`** (Python package at repo root) — `serial_link.py` (port discovery/connection), `audio_state.py` (pycaw volume/session polling), `media_sources.py` (WinRT + window-title parsing), `debug.py` (shared `DEBUG`/`debugPrint`). `template.py` is the baseline entrypoint built on top of it; future feature scripts (e.g. `duration_main.py`) live alongside it at repo root and import from `tempo_core` the same way.

Because `arduino-cli` doesn't resolve libraries outside the sketchbook by default, compile/upload through `arduino/build.ps1` (wraps `arduino-cli` with `--libraries arduino/libraries`) rather than a bare `arduino-cli compile`/`upload`:

```powershell
./arduino/build.ps1 -Sketch arduino/matrice_pad_tempo
./arduino/build.ps1 -Sketch arduino/matrice_pad_tempo -Port COM7   # compile + upload
```

## Serial Protocol

Messages are newline-terminated (`\n`).

| Direction   | Format                          | Meaning                               |
|-------------|----------------------------------|---------------------------------------|
| PC → Arduino | `song\|\|artist\|\|volume\n`   | Media title, artist string, volume 0–100 |
| Arduino → PC | `VOL:<0-100>\n`                 | User adjusted volume via encoder      |
| Arduino → PC | `MUTE\n`                        | User pressed encoder button           |

The Arduino only syncs `encoderPosition` from the first valid PC message (`volumeInitialized` flag).

## Running the Python Script

The venv is at `.venv310/` (Python 3.10 — required for winrt cp310 wheels). Activate and run:

```powershell
.\.venv310\Scripts\Activate.ps1
python template.py
```

COM port is auto-detected by USB VID:PID. Pass `--port COMx` to override. Pass `--debug` for verbose output.

## Python Dependencies

Installed in `.venv310/`. Key packages:

- `pyserial` — serial communication
- `pycaw` — Windows Core Audio API (volume get/set via `AudioDevice.EndpointVolume`)
- `comtypes` — COM interface bridge used by pycaw
- `pywin32` (`win32gui`, `win32process`) — window enumeration to find active audio session title
- `winrt` (`winrt-windows-media-control`) — now-playing metadata via `GlobalSystemMediaTransportControls`

## Arduino Libraries

Required in the Arduino IDE / library manager:

- `Keypad` (Mark Stanley, Alexander Brevig)
- `HID-Project` (NicoHood) — consumer control HID; provides `Consumer.write(MEDIA_*)` for media keys
- `Adafruit GFX Library`
- `Adafruit SSD1306`

**Keypad button layout** (left to right): mute/unmute (`M`), previous track (`R`), play/pause (`P`), next track (`F`) — mapped as HID consumer keys, no Python involvement.

Target board: **Arduino Pro Micro (ATmega32U4, 5V/16MHz)**. In Arduino IDE select *SparkFun Pro Micro 5V/16MHz* or *Arduino Leonardo* (same chip). Upload baud rate is 57600 via avr109 bootloader.

**Upload tip:** The Pro Micro resets its USB after a new sketch starts. If the port disappears after flashing, click Upload in the IDE and double-tap the reset pin the moment "Uploading..." appears — the IDE will catch the 8-second bootloader window on COM7.

Pin assignments: OLED on SDA=2/SCL=3, encoder on CLK=20/DT=21/BTN=19, keypad rows on 14,15 and columns on 10,16.

**Display layout:** `#define DISPLAY_LINES` in the sketch selects between two layouts:
- `2` (default) — textSize 2, song on row 0 / artist on row 1, both scroll when long
- `3` — textSize 1, song word-wrapped across rows 0–1 (static), artist scrolls on row 2

## Media Source Priority (`tempo_core/media_sources.py`)

Three mutually exclusive cases, checked each loop iteration:

| `get_audio_playing_window_title()` returns | Source used |
|---|---|
| `""` | Browser is active → use WinRT (`GlobalSystemMediaTransportControls`) |
| `"No media playing"` | Nothing active → send blank song/artist |
| any other string | Non-browser app (StreamPlayer, Zune, VLC) → parse window title |

WinRT runs in a background thread started by `media_sources.start_media_info_thread()` (3s poll, persistent event loop); read its results via `get_shared_media_info()`/`get_last_playing_media_info()`. Window title and volume are polled from the main loop with caching (1s and 2s respectively). Serial packets are only written on content change or every 2.5s keepalive; the Arduino connection timeout is 5s.
