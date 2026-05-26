# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MatricePad Tempo is a two-component embedded system:

- **Arduino firmware** (`arduino/matrice_pad_tempo/matrice_pad_tempo.ino`) — runs on the Matrice Pro board. Drives a 128×32 OLED, reads a rotary encoder for volume control, handles a 2×2 keypad matrix that sends HID keyboard events (F13–F16), and communicates with the PC over USB serial at 115200 baud.
- **Python host script** (`windows/tempo_v1.0.py`) — runs on Windows. Polls Windows audio state and sends media info to the Arduino. Handles incoming volume/mute commands from the Arduino.

## Serial Protocol

Messages are newline-terminated (`\n`).

| Direction   | Format                          | Meaning                               |
|-------------|----------------------------------|---------------------------------------|
| PC → Arduino | `song\|\|artist\|\|volume\n`   | Media title, artist string, volume 0–100 |
| Arduino → PC | `VOL:<0-100>\n`                 | User adjusted volume via encoder      |
| Arduino → PC | `MUTE\n`                        | User pressed encoder button           |

The Arduino only syncs `encoderPosition` from the first valid PC message (`volumeInitialized` flag).

## Running the Python Script

The venv is at `.venv/` (Python 3.14). Activate and run:

```powershell
.\.venv\Scripts\Activate.ps1
python windows\tempo_v1.0.py
```

The COM port is hardcoded as `COM10` in `tempo_v1.0.py:10` — change it to match the Arduino's assigned port.

## Python Dependencies

Installed in `.venv/`. Key packages:

- `pyserial` — serial communication
- `pycaw` — Windows Core Audio API (volume get/set)
- `comtypes` — COM interface bridge used by pycaw
- `pywin32` (`win32gui`, `win32process`) — window enumeration to find active audio session title
- `winrt` (`winrt-windows-media-control`) — available in venv, targeted by the `bpp-winrt_mediainfo` branch as a replacement media-info source

## Arduino Libraries

Required in the Arduino IDE / library manager:

- `Keypad` (Mark Stanley, Alexander Brevig)
- `Keyboard` (built-in HID)
- `Adafruit GFX Library`
- `Adafruit SSD1306`

Pin assignments: OLED on SDA=2/SCL=3, encoder on CLK=20/DT=21/BTN=19, keypad rows on 14,15 and columns on 10,16.

## Active Branch Context

Branch `bpp-winrt_mediainfo` is investigating replacing the `pycaw`+`win32gui` approach with the WinRT `GlobalSystemMediaTransportControls` API for richer, more reliable now-playing metadata. The WinRT packages are already installed in the venv.
