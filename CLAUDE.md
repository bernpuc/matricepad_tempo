# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MatricePad Tempo is a two-component embedded system:

- **Arduino firmware** (`arduino/matrice_pad_tempo/matrice_pad_tempo.ino`) — runs on the Matrice Pro board. Drives a 128×32 OLED (song/artist text view or a 16-bar frequency graph, toggled by the encoder's pushbutton), reads a rotary encoder for volume control, handles a 2×2 keypad matrix that sends HID consumer media keys, and communicates with the PC over USB serial at 115200 baud.
- **Windows host** — polls Windows audio state and now-playing media info, captures WASAPI loopback audio for the bar graph, and sends it all to the Arduino; there is no data sent back from the Arduino during normal operation (volume/media keys go straight to Windows via HID) except for the version handshake described below. Two implementations exist:
  - **`MatricePadApp/`** (.NET 10) — the active production host, installed via its NSIS installer (`MatricePadApp/build-installer.ps1`) as a Task Scheduler "at logon" task. Design doc: `docs/spec-windows-app.md`.
  - **`template.py`** / `tempo_core/` (Python) — the original implementation. Retained in the repo for reference; no longer runs at startup. The wire protocol and behavior described throughout this file (serial framing, media-source priority, COM-init stagger, etc.) apply to both implementations — `MatricePadApp` is a from-scratch C# port of the same design, not a different protocol.
- **`MatricePad.SerialCore/`** — a small shared .NET class library holding logic that must not drift between `MatricePadApp` and `MatricePadApp.FirmwareUpdater` (below): VID:PID board discovery, the DTR/RTS-aware `SerialPort` open pattern, and the `VERSION?`/`PONG` handshake. See its "Serial Port Gotchas" note below before touching either consumer's connection code.
- **`MatricePadApp.FirmwareUpdater/`** (.NET 10, WPF) — a separate, manually-launched tool that checks the connected board's firmware version and can flash the bundled firmware if it's out of date. Design doc: `docs/spec-firmwareUpdater.md`. Not installed/wired into the NSIS installer yet (deferred, see that doc §7) — run via `dotnet run` from the project folder for now.

## Code Organization

Shared logic lives in two places so feature-specific variants (e.g. a duration/elapsed-time display, a frequency bar graph) can be added as their own sketch/script pair without duplicating the boring 80%:

- **`arduino/libraries/TempoCore/`** — an in-repo Arduino library (not the sketchbook). Holds `ScrollText` (the marquee scroll engine), `StatusIcons` (mute/pause circle glyph + overlay banner drawing), `SerialFraming` (null-terminated `\|\|`-delimited field parsing), and `RotaryEncoder` (debounced quadrature rotation detection). `arduino/matrice_pad_tempo/matrice_pad_tempo.ino` is the baseline sketch built on top of it; `arduino/matrice_pad_tempo_spectrum/` is a standalone bars-only sketch built the same way; future feature sketches go in sibling folders under `arduino/` and `#include <TempoCore.h>` the same way.
- **`tempo_core/`** (Python package at repo root) — `serial_link.py` (port discovery/connection), `audio_state.py` (pycaw volume/session polling), `media_sources.py` (WinRT + window-title parsing, plus `get_smoothed_elapsed()` for the position stair-step described below), `audio_capture.py` (WASAPI loopback capture + FFT → bar levels), `debug.py` (shared `DEBUG`/`debugPrint`). `template.py` is the baseline entrypoint built on top of it (both media info and audio capture); `spectrum_main.py` is a standalone bars-only entrypoint; future feature scripts live alongside them at repo root and import from `tempo_core` the same way.

**COM threading gotcha:** `audio_capture.py` (soundcard/WASAPI) and `media_sources.py` (WinRT) each do their own first-time COM initialization on their own background thread. Starting both threads at the same instant is a real crash (observed SIGSEGV) — always stagger their `start_*_thread()` calls by ~1s, as both `template.py` and `spectrum_main.py` do. Separately, `audio_capture.py` imports `comtypes` before `soundcard` so the COM apartment mode (STA vs MTA) is claimed by `comtypes` first regardless of what else imports `soundcard` later.

Because `arduino-cli` doesn't resolve libraries outside the sketchbook by default, compile/upload through `arduino/build.ps1` (wraps `arduino-cli` with `--libraries arduino/libraries`) rather than a bare `arduino-cli compile`/`upload`:

```powershell
./arduino/build.ps1 -Sketch arduino/matrice_pad_tempo
./arduino/build.ps1 -Sketch arduino/matrice_pad_tempo -Port COM7   # compile + upload
```

## Serial Port Gotchas (`MatricePad.SerialCore`)

Found the hard way while building the version handshake and firmware updater — both consumers of `MatricePad.SerialCore` rely on these, don't reintroduce them by opening a `SerialPort` some other way:

- **DTR/RTS must be explicitly asserted.** `.NET`'s `SerialPort.DtrEnable`/`RtsEnable` default to `false`. The board's native-USB CDC stack can leave incoming data unacknowledged until DTR is actively asserted — the port opens without error, but the board never receives a single byte, and whether it happens to work anyway depends on leftover driver state from a prior session (this caused real, confusing intermittent failures before the fix). `ArduinoSerial.Open()` sets both on every open; don't construct a bare `new SerialPort(...)` against this board elsewhere.
- **The bootloader is a different USB device than the running sketch, with its own COM port and its own VID:PID.** Entering the bootloader (the 1200bps touch) does not mean the same port name reappears — it must be rediscovered by VID:PID, same as the application-mode board.
- **This board's bootloader identifies as a SparkFun Pro Micro bootloader (`1B4F:9205`), not an Arduino Leonardo bootloader (`2341:0036`).** The sketch is compiled with the `arduino:avr:leonardo` FQBN (same ATmega32U4 chip/protocol), so application mode enumerates as "Arduino Leonardo" (`2341:8036`) — but the underlying hardware is a SparkFun Pro Micro, and its bootloader keeps SparkFun's own USB identity regardless of which FQBN compiled the sketch running on it. `MatricePadApp.FirmwareUpdater` checks both `1B4F:9205` and `2341:0036` for this reason.

## Serial Protocol

Messages are newline-terminated (`\n`), sent PC → Arduino only (the baseline sketch's encoder button no longer round-trips app-volume commands — see below), except for the version handshake below, which is the one request/response exchange on the baseline sketch.

**Baseline sketch** (`matrice_pad_tempo.ino` / `template.py`):
```
song||artist||volume||muted||paused||bar0,bar1,...,bar15||elapsedSec||durationSec
```
| Field | Meaning |
|---|---|
| `song`, `artist` | ASCII-sanitized media title/artist, may be empty |
| `volume` | System master volume, 0–100 |
| `muted` | `0`/`1` |
| `paused` | `0`/`1`, real PC-reported playback status |
| `bar0..bar15` | 16 frequency-bar levels, each 0–100, comma-joined sub-field |
| `elapsedSec`, `durationSec` | WinRT-only; both `0` when no timeline is available (non-browser source, nothing playing) |

The encoder button toggles the Arduino between a **TEXT** view (song/artist, scrolling) and a **BARS** view (the 16-bar graph + elapsed/duration in the upper-right) — both are always kept up to date from the same packet regardless of which is on-screen.

**Version handshake** (baseline sketch only): once per connection, right after opening the port, `MatricePadApp`'s `SerialManager` sends a bare `VERSION?` line and firmware responds `PONG||<protocolVersion>||<firmwareVersion>` (e.g. `PONG||1||1.1.0`). `protocolVersion` is the wire-format version (`PROTOCOL_VERSION` in the `.ino` / `ExpectedProtocolVersion` in `SerialManager.cs`) — bump both together whenever the baseline packet's field count/order changes; `firmwareVersion` is a human-readable sketch version, diagnostic only. A mismatch is logged as a warning but never blocks the connection; firmware that predates this feature just ignores the unrecognized `VERSION?` line (it has no `||`, so it falls through the existing parser harmlessly) and the companion logs that no handshake response arrived, then proceeds normally.

**Spectrum sketch** (`matrice_pad_tempo_spectrum.ino` / `spectrum_main.py`) — standalone, bars-only, simpler wire format with no song/artist/volume fields:
```
bar0,bar1,...,bar15,elapsedSec,durationSec
```

## Running the Windows Host (`MatricePadApp/`, production)

```powershell
cd MatricePadApp
dotnet run                     # run directly, no install

.\build-installer.ps1          # publish self-contained + build the NSIS installer
# then run Package\Matrice Pad Sound Panel <version> Installer.exe (prompts UAC) --
# installs to C:\Program Files\MatricePad\, registers the MatricePadApp Task
# Scheduler task (at logon, current user), and launches immediately
```

COM port is auto-detected by USB VID:PID via WMI; override via `appsettings.json`'s `MatricePad:ComPort`. Logs go to `%APPDATA%\MatricePad\logs\`.

## Running the Firmware Updater (`MatricePadApp.FirmwareUpdater/`)

```powershell
./MatricePadApp.FirmwareUpdater/stage-firmware.ps1   # regenerates Firmware/ (gitignored build artifact + vendored avrdude) -- run this first, or after any .ino change
cd MatricePadApp.FirmwareUpdater
dotnet run
```

Stops the running `MatricePadApp` companion (needs exclusive port access), detects the connected board, checks its firmware version via the handshake above, and offers to flash the bundled `.hex` if it's out of date -- always with explicit confirmation, never automatically. Restarts the companion afterward regardless of outcome. Design doc: `docs/spec-firmwareUpdater.md`.

## Running the Python Script (legacy, reference only)

The venv is at `.venv310/` (Python 3.10 — required for winrt cp310 wheels). Not run at startup anymore -- kept for comparison/debugging. Activate and run:

```powershell
.\.venv310\Scripts\Activate.ps1
python template.py
```

COM port is auto-detected by USB VID:PID. Pass `--port COMx` to override. Pass `--debug` for verbose output.

## Python Dependencies (legacy)

Installed in `.venv310/`. Key packages:

- `pyserial` — serial communication
- `pycaw` — Windows Core Audio API (volume get/set via `AudioDevice.EndpointVolume`)
- `comtypes` — COM interface bridge used by pycaw
- `pywin32` (`win32gui`, `win32process`) — window enumeration to find active audio session title
- `winrt` (`winrt-windows-media-control`) — now-playing metadata via `GlobalSystemMediaTransportControls`
- `numpy` — FFT for the frequency bar graph
- `soundcard` — WASAPI loopback capture for the frequency bar graph

## Arduino Libraries

Required in the Arduino IDE / library manager:

- `Keypad` (Mark Stanley, Alexander Brevig)
- `HID-Project` (NicoHood) — consumer control HID; provides `Consumer.write(MEDIA_*)` for media keys
- `Adafruit GFX Library`
- `Adafruit SSD1306`

**Keypad button layout** (left to right): mute/unmute (`M`), previous track (`R`), play/pause (`P`), next track (`F`) — mapped as HID consumer keys, no Python involvement.

Target board: **Arduino Pro Micro (ATmega32U4, 5V/16MHz)**. In Arduino IDE select *SparkFun Pro Micro 5V/16MHz* or *Arduino Leonardo* (same chip). Upload baud rate is 57600 via avr109 bootloader.

**Upload tip:** The Pro Micro resets its USB after a new sketch starts. If the port disappears after flashing, click Upload in the IDE and double-tap the reset pin the moment "Uploading..." appears — the IDE will catch the 8-second bootloader window on COM7.

**Bootloader USB identity:** this board's bootloader enumerates as a SparkFun Pro Micro bootloader (VID:PID `1B4F:9205`), a different device from the running sketch's Arduino Leonardo identity (`2341:8036`) — see "Serial Port Gotchas" above. Relevant if you're ever scripting bootloader detection directly instead of going through `arduino-cli`/`MatricePadApp.FirmwareUpdater`.

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
