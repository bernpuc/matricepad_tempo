# MatricePad Tempo

[![GitHub license](https://img.shields.io/github/license/yourusername/your-repo-name.svg)](https://github.com/yourusername/your-repo-name/blob/main/LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/yourusername/your-repo-name.svg?style=social)](https://github.com/yourusername/your-repo-name)
[![GitHub issues](https://img.shields.io/github/issues/yourusername/your-repo-name.svg)](https://github.com/yourusername/your-repo-name/issues)
<!-- Add other relevant badges here (e.g., build status, code coverage) using Shields.io. -->

## Project Description

Firmware for Matrice Pro -Tempo 1.0- boards and server-side python.

This project combines:

*   **Arduino Code:**  Displays audio stream data and monitors potentiometer for volume control and push buttons for playback control.
*   **Python Code:** Scrapes audio stream data - artist, song, volume level and sends to the microcontroller.

## Getting Started

These instructions will get you a copy of the project up and running on your local machine for development and testing purposes.

### Prerequisites

List any software, hardware, or libraries required to run the project.

**Hardware:**

*   Arduino Pro Micro (ATmega32U4, **5V/16MHz**)
*   128×32 SSD1306 OLED display (I2C)
*   Rotary encoder with push button
*   2×2 keypad matrix

**Software:**

*   Arduino IDE — board: *SparkFun Pro Micro 5V/16MHz* (or *Arduino Leonardo*) — **or** `arduino-cli` (see below)
*   Arduino libraries: `Keypad`, `HID-Project` (NicoHood), `Adafruit GFX`, `Adafruit SSD1306`
*   Python 3.10 (required — the `winrt` wheels are built for cp310)
*   Python libraries: `pyserial`, `pycaw`, `comtypes`, `pywin32`, `winrt-runtime`, `winrt-Windows.Foundation`, `winrt-Windows.Media.Control`, `numpy`, `soundcard` (the last two for the frequency bar graph's WASAPI capture + FFT)

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

### Installation

Explain how to set up the project.

**1.  Clone the repository:**

```bash
git clone https://github.com/matricetechnologies/matricepad_tempo.git
