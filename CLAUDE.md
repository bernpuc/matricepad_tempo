# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MatricePad Tempo is a two-component embedded system:

- **Arduino firmware** (`arduino/matrice_pad_tempo/matrice_pad_tempo.ino`) — runs on the Matrice Pro board. Drives a 128×32 OLED (song/artist text view or a 16-bar frequency graph, toggled by the encoder's pushbutton), reads a rotary encoder for volume control, handles a 2×2 keypad matrix that sends HID consumer media keys, and communicates with the PC over USB serial at 115200 baud.
- **`MatricePadApp/`** (.NET 10) — the Windows host. Polls Windows audio state and now-playing media info, captures WASAPI loopback audio for the bar graph, and sends it all to the Arduino; there is no data sent back from the Arduino during normal operation (volume/media keys go straight to Windows via HID) except for the version handshake described below. Installed via its NSIS installer (`MatricePadApp/build-installer.ps1`) as a Task Scheduler "at logon" task. Design doc: `docs/spec-windows-app.md`.

  This started as a Python prototype (`template.py` / `tempo_core/`) before being ported to .NET; the prototype is preserved at the `legacy-python-era` tag if you need to compare behavior, but is no longer part of the working tree.
- **`MatricePad.SerialCore/`** — a small shared .NET class library holding logic that must not drift between `MatricePadApp` and `MatricePadApp.FirmwareUpdater` (below): VID:PID board discovery, the DTR/RTS-aware `SerialPort` open pattern, and the `VERSION?`/`PONG` handshake. See its "Serial Port Gotchas" note below before touching either consumer's connection code.
- **`MatricePadApp.FirmwareUpdater/`** (.NET 10, WPF) — a separate, manually-launched tool that checks the connected board's firmware version and can flash the bundled firmware if it's out of date. Design doc: `docs/spec-firmwareUpdater.md`. Not installed/wired into the NSIS installer yet (deferred, see that doc §7) — run via `dotnet run` from the project folder for now.

## Code Organization

`arduino/matrice_pad_tempo/` is a single, self-contained sketch — no separate library, no sibling sketch variants. `matrice_pad_tempo.ino` holds the main logic (display drawing, serial parsing, encoder/keypad handling); `ScrollText.h/.cpp` (marquee scroll engine), `StatusIcons.h/.cpp` (mute/pause circle glyph + overlay banner drawing), `SerialFraming.h/.cpp` (null-terminated `\|\|`-delimited field parsing), and `RotaryEncoder.h/.cpp` (debounced quadrature rotation detection) sit alongside it as plain tab files that `arduino-cli`/the Arduino IDE compile together automatically — the Arduino IDE shows each as its own tab.

Compile/upload through `arduino/build.ps1` (a thin `arduino-cli` wrapper with sensible FQBN/discovery-timeout defaults) rather than a bare `arduino-cli compile`/`upload`:

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

Messages are newline-terminated (`\n`), sent PC → Arduino only (the encoder button no longer round-trips app-volume commands — see below), except for the version handshake below, which is the one request/response exchange.

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

**Version handshake:** once per connection, right after opening the port, `MatricePadApp`'s `SerialManager` sends a bare `VERSION?` line and firmware responds `PONG||<protocolVersion>||<firmwareVersion>` (e.g. `PONG||1||1.1.0`). `protocolVersion` is the wire-format version (`PROTOCOL_VERSION` in the `.ino` / `ExpectedProtocolVersion` in `SerialManager.cs`) — bump both together whenever the packet's field count/order changes; `firmwareVersion` is a human-readable sketch version, diagnostic only. A mismatch is logged as a warning but never blocks the connection; firmware that predates this feature just ignores the unrecognized `VERSION?` line (it has no `||`, so it falls through the existing parser harmlessly) and the companion logs that no handshake response arrived, then proceeds normally.

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

## Arduino Libraries

Required in the Arduino IDE / library manager:

- `Keypad` (Mark Stanley, Alexander Brevig)
- `HID-Project` (NicoHood) — consumer control HID; provides `Consumer.write(MEDIA_*)` for media keys
- `Adafruit GFX Library`
- `Adafruit SSD1306`

**Keypad button layout** (left to right): mute/unmute (`M`), previous track (`R`), play/pause (`P`), next track (`F`) — mapped directly to HID consumer keys on the Arduino, no host involvement.

Target board: **Arduino Pro Micro (ATmega32U4, 5V/16MHz)**. In Arduino IDE select *SparkFun Pro Micro 5V/16MHz* or *Arduino Leonardo* (same chip). Upload baud rate is 57600 via avr109 bootloader.

**Upload tip:** The Pro Micro resets its USB after a new sketch starts. If the port disappears after flashing, click Upload in the IDE and double-tap the reset pin the moment "Uploading..." appears — the IDE will catch the 8-second bootloader window on COM7.

**Bootloader USB identity:** this board's bootloader enumerates as a SparkFun Pro Micro bootloader (VID:PID `1B4F:9205`), a different device from the running sketch's Arduino Leonardo identity (`2341:8036`) — see "Serial Port Gotchas" above. Relevant if you're ever scripting bootloader detection directly instead of going through `arduino-cli`/`MatricePadApp.FirmwareUpdater`.

Pin assignments: OLED on SDA=2/SCL=3, encoder on CLK=20/DT=21/BTN=19, keypad rows on 14,15 and columns on 10,16.

**Display layout:** `#define DISPLAY_LINES` in the sketch selects between two layouts:
- `2` (default) — textSize 2, song on row 0 / artist on row 1, both scroll when long
- `3` — textSize 1, song word-wrapped across rows 0–1 (static), artist scrolls on row 2

## Media Source Priority (`MatricePadApp/Services/MediaInfoProvider.cs`)

Three mutually exclusive cases, checked each poll:

| Foreground window title | Source used |
|---|---|
| `""` (browser active) | WinRT (`GlobalSystemMediaTransportControls`) |
| `"No media playing"` | Nothing active → send blank song/artist (or retain last-known track as paused, if it had a title) |
| any other string | Non-browser app (StreamPlayer, Zune, VLC) → parse window title |
