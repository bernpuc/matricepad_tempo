# Software Specification: Panel Firmware
## Matrice Pad Sound Panel — ATmega32U4

**Version:** 1.1

---

## 1. Overview

The Panel firmware runs on an ATmega32U4 (SparkFun Pro Micro, 5V/16MHz). It drives a 128×32 OLED display, reads a rotary encoder and a 2×2 keypad, sends HID consumer control events to the host, and receives now-playing data from the Windows Service over USB serial.

The firmware has no connectivity dependency — all HID controls function regardless of whether the Windows Service is running.

---

## 2. Hardware Pin Map

| Signal | Pin |
|---|---|
| OLED SDA | 2 |
| OLED SCL | 3 |
| Encoder CLK | 20 |
| Encoder DT | 21 |
| Encoder Button | 19 |
| Keypad Row 0 | 14 |
| Keypad Row 1 | 15 |
| Keypad Col 0 | 10 |
| Keypad Col 1 | 16 |

---

## 3. Libraries

| Library | Purpose |
|---|---|
| Adafruit SSD1306 | OLED driver |
| Adafruit GFX | Text and bitmap rendering |
| HID-Project (NicoHood) | HID consumer control interface |
| Keypad (Mark Stanley) | 2×2 matrix key scanning |

### 3.1 Shared Code (`TempoCore`)

The marquee scroll engine, status-icon/overlay drawing, and `\|\|`-delimited serial field parsing live in an in-repo Arduino library, `arduino/libraries/TempoCore` (not the sketchbook), so future feature sketches can reuse them without duplication:

| Header | Contents |
|---|---|
| `ScrollText.h` | `LineScroll` struct, `resetScroll()`, `tickScroll()` |
| `StatusIcons.h` | `drawCircleIcon()`, `applyMuteContrast()`, `showOverlayBanner()` |
| `SerialFraming.h` | `findSep()`, `trimInPlace()`, `copyField()`, `splitTitleIntoLines()` |

Building/uploading requires resolving this library path explicitly — via `arduino/build.ps1` (wraps `arduino-cli` with `--libraries arduino/libraries`), or by linking `TempoCore` into the Arduino IDE's sketchbook `libraries/` folder. See the README for both paths.

---

## 4. Configuration Constants

| Constant | Default | Description |
|---|---|---|
| `DISPLAY_LINES` | `2` | Layout mode. `2` = two large rows, `3` = three small rows |
| `SCROLL_PAUSE_MS` | `2000` | Milliseconds to pause at each scroll end |
| `SCROLL_STEP_MS` | `40` | Milliseconds between scroll pixel steps |
| `TIMEOUT` | `5000` | Milliseconds before connection-lost screen |
| `MAX_SERIAL_BUFFER` | `256` | Maximum serial input buffer size in bytes |
| `ENCODER_DEBOUNCE_MS` | `10` | Encoder debounce window in milliseconds |
| `debounceDelay` | `50` | Button debounce window in milliseconds |

---

## 5. State and Global Variables

| Variable | Type | Description |
|---|---|---|
| `isMuted` | `bool` | Current mute state, synced from Windows Service |
| `isPaused` | `bool` | Current pause state, synced from Windows Service |
| `volume` | `int` | Last system volume received from Windows Service (0–100) |
| `appVolume` | `int` | Last active application volume received from Windows Service (0–100) |
| `currentMode` | `VolumeMode` | Encoder mode: `SYSTEM_VOL` or `APP_VOL`; defaults to `SYSTEM_VOL` on boot |
| `connected` | `bool` | True when serial data has been received within TIMEOUT |
| `line1` | `String` | Track title (2-line mode), or title line 1 (3-line mode) |
| `line2` | `String` | Title line 2 (3-line mode only) |
| `artist` | `String` | Artist name |
| `volumeBeingAdjusted` | `bool` | True while volume overlay is visible |
| `lastEncoderAdjustTime` | `unsigned long` | Timestamp of last encoder turn, for overlay timeout |
| `muteDisplayed` | `bool` | True while any transient overlay is visible (mode, mute) |
| `muteDisplayStart` | `unsigned long` | Timestamp of overlay start |
| `inputBuffer` | `String` | Accumulates serial characters until `\n` |
| `lastEncoderState` | `int` | Previous CLK pin state for edge detection |
| `lastEncoderDebounceTime` | `unsigned long` | Last encoder edge timestamp |
| `lastButtonState` | `int` | Debounced encoder button state |
| `lastRawButton` | `int` | Raw encoder button state for debounce logic |
| `lastDebounceTime` | `unsigned long` | Last encoder button edge timestamp |
| `lastUpdateTime` | `unsigned long` | Timestamp of last valid serial packet |

`VolumeMode` is an enum: `SYSTEM_VOL = 0`, `APP_VOL = 1`.

---

## 6. Display Layouts

### 6.1 Two-Line Mode (`DISPLAY_LINES == 2`)

- textSize 2 (16px tall characters, ~12px wide)
- Row 0 at Y=0: track title, scrolls horizontally
- Row 1 at Y=16: artist, scrolls horizontally
- `CHAR_WIDTH_PX = 12`

### 6.2 Three-Line Mode (`DISPLAY_LINES == 3`)

- textSize 1 (8px tall characters, 6px wide)
- Row 0 at Y=0: title word-wrapped line 1, static
- Row 1 at Y=10: title word-wrapped line 2, static
- Row 2 at Y=20: artist, scrolls horizontally
- `CHAR_WIDTH_PX = 6`
- Word-wrap algorithm: find the last space at or before character position `DISPLAY_CHARS_PER_LINE` (21). If no space found, hard-truncate with `...` at position 18.

---

## 7. Scroll State Machine

Each scrolling line has a `LineScroll` struct: `{ int pixel, unsigned long lastTime, int8_t dir }`.

- `dir == 0`: paused. After `SCROLL_PAUSE_MS` elapses, advance to scrolling.
  - If `pixel == 0`: set `dir = +1` (scroll forward)
  - If `pixel == maxPx`: set `dir = -1` (scroll backward)
- `dir != 0`: scrolling. After `SCROLL_STEP_MS` elapses, increment `pixel` by `dir`.
  - Clamp to `[0, maxPx]` and set `dir = 0` when either end is reached.
- `maxPx = (content length in pixels) - SCREEN_WIDTH`
- No scrolling if content fits within screen width (maxPx ≤ 0).

In 2-line mode: `scroll[0]` = title, `scroll[1]` = artist.
In 3-line mode: `scroll[0]` = artist only.

`resetScroll()` sets `pixel = 0`, `dir = 0`, `lastTime = millis()`.

---

## 8. Serial Protocol — Receive

Format: `song||artist||volume||muted||appvolume||paused\n` (ASCII, newline-terminated)

Parsing on receipt of `\n`:
1. Find `firstSep` = index of first `||`
2. Find `secondSep` = index of second `||` (search from `firstSep + 2`)
3. Find `thirdSep` = index of third `||` (search from `secondSep + 2`)
4. Find `fourthSep` = index of fourth `||` (search from `thirdSep + 2`)
5. Find `fifthSep` = index of fifth `||` (search from `fourthSep + 2`)
6. Reject packet if any separator is missing
7. Extract:
   - `songTitle` = `inputBuffer[0 .. firstSep)`
   - `newArtist` = `inputBuffer[firstSep+2 .. secondSep)`
   - `volume` = `inputBuffer[secondSep+2 .. thirdSep)` as int
   - `newMuted` = `inputBuffer[thirdSep+2 .. fourthSep)` as int, non-zero = true
   - `appVolume` = `inputBuffer[fourthSep+2 .. fifthSep)` as int
   - `isPaused` = `inputBuffer[fifthSep+2 ..]` as int, non-zero = true
8. Trim `songTitle`
9. Update display strings: if `songTitle != line1`, set `line1 = songTitle` and `resetScroll(scroll[0])`; same for artist
10. Update mute state: if `newMuted != isMuted`, set `isMuted = newMuted` and call `applyMuteContrast()`
11. Set `connected = true`, `lastUpdateTime = millis()`
12. Trigger `drawMediaDisplay()` if neither `volumeBeingAdjusted` nor `muteDisplayed`

Buffer management: append each received character to `inputBuffer` up to `MAX_SERIAL_BUFFER`. Clear buffer on each `\n`.

---

## 9. HID Output

All HID events use the HID-Project `Consumer` interface.

| Trigger | Condition | HID Event |
|---|---|---|
| Encoder turn clockwise | `currentMode == SYSTEM_VOL` | `MEDIA_VOLUME_UP` |
| Encoder turn counter-clockwise | `currentMode == SYSTEM_VOL` | `MEDIA_VOLUME_DOWN` |
| Keypad M | — | `MEDIA_VOLUME_MUTE` |
| Keypad R | — | `MEDIA_PREVIOUS` |
| Keypad P | — | `MEDIA_PLAY_PAUSE` |
| Keypad F | — | `MEDIA_NEXT` |

When `currentMode == APP_VOL`, encoder turns send serial messages instead of HID events (see section 10).

---

## 10. Encoder Handling

Edge detection on CLK pin (falling edge = HIGH→LOW transition).

On falling edge (with debounce `ENCODER_DEBOUNCE_MS`):
- Read DT pin; determine direction (`clockwise = DT == CLK`)
- **System volume mode** (`currentMode == SYSTEM_VOL`):
  - Clockwise → `Consumer.write(MEDIA_VOLUME_UP)`
  - Counter-clockwise → `Consumer.write(MEDIA_VOLUME_DOWN)`
  - Draw overlay: `Vol: XX%` using `volume`
- **App volume mode** (`currentMode == APP_VOL`):
  - Clockwise → `Serial.println("APPVOL:+")`
  - Counter-clockwise → `Serial.println("APPVOL:-")`
  - Draw overlay: `App: XX%` using `appVolume`
- Set `volumeBeingAdjusted = true`, `lastEncoderAdjustTime = millis()`

Volume overlay clears after 1000ms of no encoder activity.

---

## 11. Encoder Button Handling

Debounce using `debounceDelay` (50ms). On confirmed press (LOW edge):
- Toggle `currentMode`: `SYSTEM_VOL` ↔ `APP_VOL`
- Draw mode overlay: `SYS VOL` or `APP VOL`, textSize 2, cursor at (4, 8)
- Set `muteDisplayed = true`, `muteDisplayStart = millis()`

Mode overlay clears after 1000ms.

---

## 12. Mute Contrast

`applyMuteContrast()` sends an SSD1306 contrast command directly:
- Muted: contrast = 10
- Unmuted: contrast = 255

---

## 13. Status Icons

Two icons share the same position and drawing function `drawCircleIcon(bool isMute)`. Both are 32×32, drawn centred on the display (circle centre x=64, y=16, radius=15).

**Rendering approach:** `drawMediaDisplay()` draws text first, then calls `drawCircleIcon` on top. `fillCircle` with `SSD1306_WHITE` paints a solid white disc that covers any scrolling text within the icon boundary. The symbol is then drawn in `SSD1306_BLACK` inside the disc.

**Mute icon** (`isMute == true`, shown when `isMuted == true`):
- Speaker body: `fillRect(50, 13, 5, 7)` — solid rectangle at left of disc
- Speaker horn: two `fillTriangle` calls forming the flared cone to the right of the body
- X mark: four `drawLine` calls (two pairs of parallel diagonals) for visible thickness

**Pause icon** (`isMute == false`, shown when `isPaused == true` and `isMuted == false`):
- Equilateral triangle pointing right, inset ~3 px from circle edge: `fillTriangle(76, 16, 58, 6, 58, 26)` — inscribed radius 12 (circle radius 15)

**Priority:** `isMuted` takes precedence. If both `isMuted` and `isPaused` are true, only the mute icon is shown.

---

## 14. Connection Timeout

If `millis() - lastUpdateTime > TIMEOUT` and `connected == true`:
- Set `connected = false`
- Display: `No connection` (row 0) / `Awaiting update...` (row 1), textSize 1

---

## 15. Setup Sequence

1. `delay(2000)` — allow USB enumeration to settle
2. `Serial.begin(115200)`
3. `Consumer.begin()`
4. Configure encoder and button pins as `INPUT_PULLUP`
5. Initialize scroll state array to zeros
6. Initialize OLED; halt if `display.begin()` fails
7. Display "Waiting for data..." message
8. Set `lastUpdateTime = millis()`

---

## 16. Main Loop Order

Each iteration of `loop()`:
1. Process serial input (read all available bytes)
2. Encoder button debounce and press detection
3. Clear mute overlay if 1000ms elapsed
4. Encoder rotation detection
5. Clear volume overlay if 1000ms of no encoder activity
6. Scroll tick (only when `connected`, not adjusting volume, not in mute overlay)
7. Connection timeout check
