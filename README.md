# MatricePad Tempo

[![GitHub license](https://img.shields.io/github/license/yourusername/your-repo-name.svg)](https://github.com/yourusername/your-repo-name/blob/main/LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/yourusername/your-repo-name.svg?style=social)](https://github.com/yourusername/your-repo-name)
[![GitHub issues](https://img.shields.io/github/issues/yourusername/your-repo-name.svg)](https://github.com/yourusername/your-repo-name/issues)
<!-- Add other relevant badges here (e.g., build status, code coverage) using Shields.io. -->

## Project Description

Firmware for Matrice Pro -Tempo 1.0- boards, plus a Windows host app that feeds them now-playing media info, volume/mute, and a real-time frequency spectrum over USB serial.

This project combines:

*   **Arduino Firmware:** Drives a 128×32 OLED (now-playing text or a 16-bar frequency graph), reads a rotary encoder and 2×2 keypad, and sends HID volume/media-key events straight to Windows. Also responds to a `VERSION?` handshake so the Windows host can detect a firmware/companion version mismatch.
*   **Windows Host — `MatricePadApp/` (.NET 10, active):** Polls Windows audio state and now-playing media info, captures WASAPI loopback audio for the bar graph, and streams it all to the board. Installed via an NSIS installer that registers a Task Scheduler "at logon" task — see `MatricePadApp/build-installer.ps1`.
*   **`MatricePad.SerialCore/`:** Shared library holding board discovery (VID:PID), the DTR/RTS-aware serial connection pattern, and the version handshake — used by both `MatricePadApp/` and the Firmware Updater below, so this logic exists in exactly one place.
*   **`MatricePadApp.FirmwareUpdater/`:** A separate tool (Start Menu shortcut after install: "Matrice Pad Tempo Firmware Updater") that checks the connected board's firmware version and can flash the bundled firmware if it's out of date, with explicit confirmation at every step.

See `docs/PRD.md` and the other `docs/spec-*.md` files for the full design, and `CLAUDE.md` for repo-specific developer notes.

## Getting Started

These instructions will get you a copy of the project up and running on your local machine for development and testing purposes.

### Prerequisites

**Hardware:**

*   Arduino Pro Micro (ATmega32U4, **5V/16MHz**)
*   128×32 SSD1306 OLED display (I2C)
*   Rotary encoder with push button
*   2×2 keypad matrix

**Software:**

*   Arduino IDE — board: *SparkFun Pro Micro 5V/16MHz* (or *Arduino Leonardo*) — **or** `arduino-cli` (see below)
*   Arduino libraries: `Keypad`, `HID-Project` (NicoHood), `Adafruit GFX`, `Adafruit SSD1306`
*   .NET 10 SDK (Windows host, `MatricePadApp/`)
*   NSIS (only needed to build the installer — `MatricePadApp/build-installer.ps1`)

### Building/Uploading the Firmware

The sketch depends on `arduino/libraries/TempoCore` (shared scroll/icon/serial-parsing code, in-repo — see `CLAUDE.md`). Two ways to build:

**Via `arduino-cli`** (no IDE library setup needed):
```powershell
arduino-cli core install arduino:avr
arduino-cli lib install "Keypad" "HID-Project" "Adafruit GFX Library" "Adafruit SSD1306"
./arduino/build.ps1 -Sketch arduino/matrice_pad_tempo               # compile only
./arduino/build.ps1 -Sketch arduino/matrice_pad_tempo -Port COM7    # compile + upload
```
If double-tapping the Pro Micro's reset pin to reach the bootloader (see upload tip below), `build.ps1` waits up to 20s (`-DiscoveryTimeout`) for the bootloader port to reappear instead of arduino-cli's default 1s, which is too tight to hit manually.

**Via the Arduino IDE:** the IDE only auto-discovers libraries from its sketchbook folder, not this repo, so link `TempoCore` into the sketchbook once:
```powershell
New-Item -ItemType Junction -Path "<your sketchbook>\libraries\TempoCore" -Target "<repo path>\arduino\libraries\TempoCore"
```
(Find your sketchbook path via File → Preferences → "Sketchbook location" in the IDE.) After that, open `arduino/matrice_pad_tempo/matrice_pad_tempo.ino` directly and Upload as normal.

### Running the Windows Host

**1. Clone the repository:**

```bash
git clone https://github.com/bernpuc/matricepad_tempo.git
```

**2. Production (.NET) — install so it runs at logon:**

```powershell
cd MatricePadApp
.\build-installer.ps1
# then run Package\Matrice Pad Sound Panel <version> Installer.exe (prompts UAC)
```

This installs to `C:\Program Files\MatricePad\`, registers a Task Scheduler "at logon" task, and launches immediately. It also installs the Firmware Updater (below) and its own Start Menu shortcut. To run the companion without installing:

```powershell
cd MatricePadApp
dotnet run
```

**3. Firmware Updater — checks/flashes the board's firmware:**

```powershell
./MatricePadApp.FirmwareUpdater/stage-firmware.ps1   # regenerates the bundled .hex + avrdude -- run first, or after any .ino change
cd MatricePadApp.FirmwareUpdater
dotnet run
```

Stops the running companion (needs exclusive access to the board's serial port), checks the connected board's firmware version, and offers to flash the bundled firmware if it's out of date — always with explicit confirmation. See `docs/spec-firmwareUpdater.md`.
