# Software Specification: Panel Firmware
## Matrice Pad Sound Panel — ATmega32U4

**Version:** 1.2

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
| `RotaryEncoder.h` | `EncoderState` struct, `initEncoderState()`, `tickEncoder()` (debounced quadrature rotation) |

Building/uploading requires resolving this library path explicitly — via `arduino/build.ps1` (wraps `arduino-cli` with `--libraries arduino/libraries`), or by linking `TempoCore` into the Arduino IDE's sketchbook `libraries/` folder. See the README for both paths.

---

## 4. Configuration Constants

| Constant | Default | Description |
|---|---|---|
| `DISPLAY_LINES` | `2` | Layout mode. `2` = two large rows, `3` = three small rows |
| `SCROLL_PAUSE_MS` | `2000` | Milliseconds to pause at each scroll end |
| `SCROLL_STEP_MS` | `40` | Milliseconds between scroll pixel steps |
| `TIMEOUT` | `5000` | Milliseconds before connection-lost screen |
| `MAX_SERIAL_BUFFER` | `384` | Maximum serial input buffer size in bytes (bumped from 256 to fit the bars sub-field) |
| `ENCODER_DEBOUNCE_MS` | `10` | Encoder debounce window in milliseconds |
| `debounceDelay` | `50` | Button debounce window in milliseconds |
| `NUM_BARS` | `16` | Frequency bars in the BARS view |
| `BAR_WIDTH` / `BAR_GAP` | `6` / `2` | Bar width and gap in pixels (8px slot × 16 = 128px) |
| `TIME_CHAR_WIDTH_PX` | `6` | Char width for the BARS view's elapsed/duration text (textSize 1) |

---

## 5. State and Global Variables

| Variable | Type | Description |
|---|---|---|
| `isMuted` | `bool` | Current mute state, synced from Windows Service |
| `isPaused` | `bool` | Current pause state, synced from Windows Service |
| `volume` | `int` | Last system volume received from Windows Service (0–100) |
| `currentDisplayMode` | `DisplayMode` | `MODE_TEXT` or `MODE_BARS`; toggled by the encoder button, defaults to `MODE_TEXT` on boot |
| `barLevels[16]` | `int[]` | Frequency bar levels (0–100), received from the PC |
| `elapsedSec` / `durationSec` | `int` | WinRT-only; both 0 when no timeline is available |
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

`DisplayMode` is an enum: `MODE_TEXT = 0`, `MODE_BARS = 1`.

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

### 6.3 BARS View (`currentDisplayMode == MODE_BARS`)

- 16 vertical bars spanning the full 128×32 screen, each an 8px slot (6px `fillRect` bar + 2px gap), height = `barLevels[i] * 32 / 100`, growing bottom-up
- Elapsed/duration text (`M:SS/M:SS`) at textSize 1, right-aligned in the upper-right corner, drawn only when `durationSec > 0`
- Mute/pause icon drawn on top exactly as in TEXT mode
- Toggled by the encoder button (`handleDisplayModeButton()`); a shared `drawCurrentView()` dispatcher picks TEXT or BARS so every state-change handler (serial input, mute, volume overlay) redraws whichever view is active without needing mode-specific logic

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

Format: `song||artist||volume||muted||paused||bar0,bar1,...,bar15||elapsedSec||durationSec\n` (ASCII, newline-terminated)

Parsing on receipt of `\n`:
1. Find 7 `||` separators the same way as before (each search starting 2 chars past the previous one), isolating 8 fields: `songTitle`, `newArtist`, `volume`, `newMuted`, `isPaused`, `barsBlob`, `elapsedSec`, `durationSec`
2. Reject packet if any separator is missing
3. Extract `volume`, `newMuted` (non-zero = true), `isPaused` (non-zero = true), `elapsedSec`, `durationSec` as ints
4. Trim `songTitle`
5. Update display strings: if `songTitle != line1`, set `line1 = songTitle` and `resetScroll(scroll[0])`; same for artist
6. Update mute state: if `newMuted != isMuted`, set `isMuted = newMuted` and call `applyMuteContrast()`
7. Tokenize `barsBlob` on `,` (`strtok`, up to 16 tokens) into `barLevels[16]`, clamped to `[0, 100]`
8. Set `connected = true`, `lastUpdateTime = millis()`
9. Trigger `drawCurrentView()` if no overlay is active (updates whichever view -- TEXT or BARS -- is currently on-screen; the other view's underlying data is still refreshed above regardless)

Buffer management: append each received character to `inputBuffer` up to `MAX_SERIAL_BUFFER`. Clear buffer on each `\n`.

---

## 9. HID Output

All HID events use the HID-Project `Consumer` interface.

| Trigger | HID Event |
|---|---|
| Encoder turn clockwise | `MEDIA_VOLUME_UP` |
| Encoder turn counter-clockwise | `MEDIA_VOLUME_DOWN` |
| Keypad M | `MEDIA_VOLUME_MUTE` |
| Keypad R | `MEDIA_PREVIOUS` |
| Keypad P | `MEDIA_PLAY_PAUSE` |
| Keypad F | `MEDIA_NEXT` |

System-volume-only -- there is no app-volume mode or serial round-trip for encoder turns (see section 11 for what the encoder button does instead).

---

## 10. Encoder Handling

Edge detection on CLK pin (falling edge = HIGH→LOW transition), via `TempoCore::tickEncoder()`.

On falling edge (with debounce `ENCODER_DEBOUNCE_MS`):
- Read DT pin; determine direction (`clockwise = DT == CLK`)
- Clockwise → `Consumer.write(MEDIA_VOLUME_UP)`; counter-clockwise → `Consumer.write(MEDIA_VOLUME_DOWN)`
- Draw overlay: `Vol: XX%` using `volume`, over whichever view (TEXT or BARS) is currently active

Volume overlay clears after 1000ms.

---

## 11. Encoder Button Handling

Debounce using `debounceDelay` (50ms). On confirmed press (LOW edge):
- Toggle `currentDisplayMode`: `MODE_TEXT` ↔ `MODE_BARS`
- Clear any active overlay and call `drawCurrentView()` immediately

No banner -- swapping the whole screen's content between TEXT and BARS is its own obvious feedback.

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
1. Process serial input (read all available bytes; updates both views' underlying data)
2. Encoder button: debounce, press detection, TEXT/BARS toggle
3. Encoder rotation detection (volume overlay)
4. Clear volume overlay if 1000ms elapsed
5. Scroll tick (TEXT mode only, when `connected` and no overlay active)
6. Connection timeout check
7. Keypad scan (mute/prev/play-pause/next)
