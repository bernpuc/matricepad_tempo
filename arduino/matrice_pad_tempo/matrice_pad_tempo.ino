#include <Keypad.h>
#include <HID-Project.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

// ── Display layout switch ─────────────────────────────────────────────────────
// Set to 2 or 3 to select the display layout.
//
// DISPLAY_LINES 2  (default)
//   textSize 2 — large text, ~10 chars visible per row.
//   Row 0: song title   (scrolls when > ~10 chars)
//   Row 1: artist name  (scrolls when > ~10 chars)
//   Both rows fill the 32px height exactly (2 × 16px).
//   Best for quick readability from a distance.
//
// DISPLAY_LINES 3
//   textSize 1 — small text, 21 chars visible per row.
//   Rows 0–1: song title word-wrapped (static)
//   Row 2:    artist name (scrolls when > 21 chars)
//   Better for long titles that would otherwise scroll constantly.
#define DISPLAY_LINES 2

// ── OLED ──────────────────────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  32
#define OLED_RESET      -1
#define OLED_SDA         2
#define OLED_SCL         3
// 128px / 6px-per-char (5px glyph + 1px spacing) = 21 chars at text size 1
#define DISPLAY_CHARS_PER_LINE 21
#define MAX_SERIAL_BUFFER     256
#define MAX_FIELD_LEN          96

// ── Row y-positions ───────────────────────────────────────────────────────────
#if DISPLAY_LINES == 2
  // textSize 2: 16px tall chars — two rows fill 32px exactly
  #define ROW0_Y       0
  #define ROW1_Y      16
  #define CHAR_WIDTH_PX 12   // 10px glyph + 2px spacing at textSize 2
#else
  #define ROW0_Y       0
  #define ROW1_Y      10
  #define ROW2_Y      20
  #define CHAR_WIDTH_PX  6   //  5px glyph + 1px spacing at textSize 1
#endif

// ── Scroll timing ─────────────────────────────────────────────────────────────
#define SCROLL_PAUSE_MS 2000
#define SCROLL_STEP_MS    40

// ── Encoder pins ──────────────────────────────────────────────────────────────
#define ENCODER_PIN_CLK 20
#define ENCODER_PIN_DT  21
#define ENCODER_BTN     19

// ── Globals ───────────────────────────────────────────────────────────────────
int lastEncoderState = LOW;
unsigned long lastEncoderDebounceTime = 0;
const unsigned long ENCODER_DEBOUNCE_MS = 10;

// Fixed-size serial receive buffer (no String/heap churn on parse).
char inputBuffer[MAX_SERIAL_BUFFER];
int  inputLen = 0;

int volume    = 0;
int appVolume = 0;

unsigned long lastUpdateTime = 0;
const unsigned long TIMEOUT = 5000;
bool connected = false;

// ── Transient overlay state ───────────────────────────────────────────────────
// A single active "overlay" replaces the media display for a fixed duration
// (volume readout, mute/unmute banner, or SYS/APP mode banner), then reverts.
// Only one can be showing at a time — raising a new one simply replaces
// whichever was active, which is exactly what happened by accident before
// this was unified (whichever handler drew last "won" the screen anyway).
enum OverlayKind { OVERLAY_NONE, OVERLAY_VOLUME, OVERLAY_MUTE, OVERLAY_MODE };
OverlayKind   activeOverlay    = OVERLAY_NONE;
unsigned long overlayStart     = 0;
unsigned long overlayDurationMs = 0;

char line1[MAX_FIELD_LEN]  = "";
char line2[MAX_FIELD_LEN]  = "";   // used in 3-line mode only
char artist[MAX_FIELD_LEN] = "";

bool isMuted  = false;
bool isPaused = false;

enum VolumeMode { SYSTEM_VOL, APP_VOL };
VolumeMode currentMode = SYSTEM_VOL;

// ── Scroll state ──────────────────────────────────────────────────────────────
struct LineScroll {
    int           pixel;
    unsigned long lastTime;
    int8_t        dir;   // 0 = paused, +1 = scrolling forward, -1 = scrolling backward
};

#if DISPLAY_LINES == 2
LineScroll scroll[2];   // [0] = song line   [1] = artist line
#else
LineScroll scroll[1];   // [0] = artist line only
#endif


Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const byte ROWS = 2;
const byte COLS = 2;
char hexaKeys[ROWS][COLS] = {
    {'M', 'R'},
    {'P', 'F'},
};
byte rowPins[ROWS] = {14, 15};
byte colPins[COLS] = {10, 16};
Keypad customKeypad = Keypad(makeKeymap(hexaKeys), rowPins, colPins, ROWS, COLS);

int lastButtonState = HIGH;
int lastRawButton   = HIGH;
unsigned long lastDebounceTime = 0;
const int debounceDelay = 50;

// ── Serial parse helpers (no String allocation — mutates inputBuffer in place) ─
char* findSep(char *s) {
    return strstr(s, "||");
}

void trimInPlace(char *s) {
    int len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    int start = 0;
    while (s[start] && isspace((unsigned char)s[start])) start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
}

// ── Scroll helpers ────────────────────────────────────────────────────────────
void resetScroll(LineScroll &s) {
    s.pixel    = 0;
    s.dir      = 0;
    s.lastTime = millis();
}

// Advance one scroll state. Returns true when the display needs a redraw.
bool tickScroll(LineScroll &s, int contentPx) {
    if (contentPx <= SCREEN_WIDTH) return false;
    int maxPx = contentPx - SCREEN_WIDTH;
    unsigned long now = millis();

    if (s.dir == 0) {
        if (now - s.lastTime >= SCROLL_PAUSE_MS) {
            s.dir      = (s.pixel == 0) ? 1 : -1;
            s.lastTime = now;
        }
    } else {
        if (now - s.lastTime >= SCROLL_STEP_MS) {
            s.lastTime = now;
            s.pixel   += s.dir;
            if (s.pixel >= maxPx) { s.pixel = maxPx; s.dir = 0; }
            else if (s.pixel <= 0) { s.pixel = 0;    s.dir = 0; }
            return true;
        }
    }
    return false;
}

// ── Drawing ───────────────────────────────────────────────────────────────────
void drawCircleIcon(bool isMute) {
    display.fillCircle(64, 16, 15, SSD1306_WHITE);
    if (isMute) {
        // Speaker body
        display.fillRect(50, 13, 5, 7, SSD1306_BLACK);
        // Speaker cone (horn)
        display.fillTriangle(54, 13, 54, 20, 62, 24, SSD1306_BLACK);
        display.fillTriangle(54, 13, 62,  8, 62, 24, SSD1306_BLACK);
        // X mark (two thick diagonal lines each)
        display.drawLine(65, 12, 72, 20, SSD1306_BLACK);
        display.drawLine(66, 12, 73, 20, SSD1306_BLACK);
        display.drawLine(65, 20, 72, 12, SSD1306_BLACK);
        display.drawLine(66, 20, 73, 12, SSD1306_BLACK);
    } else {
        // Paused: solid right-pointing play arrow
        display.fillTriangle(76, 16, 58, 6, 58, 26, SSD1306_BLACK);
    }
}

void applyMuteContrast() {
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(isMuted ? 10 : 255);
}

void drawMediaDisplay() {
    display.clearDisplay();
    display.setTextWrap(false);

#if DISPLAY_LINES == 2
    display.setTextSize(2);
    display.setCursor(-scroll[0].pixel, ROW0_Y);
    display.println(line1);
    display.setCursor(-scroll[1].pixel, ROW1_Y);
    display.println(artist);
#else
    display.setTextSize(1);
    display.setCursor(0, ROW0_Y);
    display.println(line1);
    display.setCursor(0, ROW1_Y);
    display.println(line2);
    display.setCursor(-scroll[0].pixel, ROW2_Y);
    display.println(artist);
#endif

    if (isMuted) {
        drawCircleIcon(true);
    } else if (isPaused) {
        drawCircleIcon(false);
    }
    display.display();
}

// Draws a full-screen single-line banner and marks it as the active overlay
// for durationMs. checkOverlayTimeout() reverts to drawMediaDisplay() once
// that time has elapsed. Used for the volume readout, mute/unmute banner, and
// SYS/APP mode banner — the one place their shared draw+timer boilerplate lives.
void showOverlay(OverlayKind kind, unsigned long durationMs, int textSize, int x, int y, const char *text) {
    display.clearDisplay();
    display.setTextSize(textSize);
    display.setCursor(x, y);
    display.println(text);
    display.display();

    activeOverlay    = kind;
    overlayStart     = millis();
    overlayDurationMs = durationMs;
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    delay(2000);
    Serial.begin(115200);
    delay(100);
    Consumer.begin();

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    pinMode(ENCODER_PIN_CLK, INPUT_PULLUP);
    pinMode(ENCODER_PIN_DT,  INPUT_PULLUP);
    pinMode(ENCODER_BTN,     INPUT_PULLUP);
    lastEncoderState = digitalRead(ENCODER_PIN_CLK);

    for (int i = 0; i < (int)(sizeof(scroll) / sizeof(scroll[0])); i++) {
        scroll[i] = {0, 0, 0};   // pixel=0, lastTime=0, dir=0
    }

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        while (1);
    }

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 12);
    display.println("Waiting for data...");
    display.display();

    lastUpdateTime = millis();
}

// ── Loop step handlers ─────────────────────────────────────────────────────────
// Each function owns one independent concern of loop(). They communicate only
// through the globals declared above — no parameters are passed between them.

// Reads and parses "song||artist||volume||muted||appVolume||paused" lines from
// the PC, updating display/volume/mute/pause state on a complete message.
void handleSerialInput() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            inputBuffer[inputLen] = '\0';

            char *pTitle  = inputBuffer;
            char *pArtist = nullptr;
            char *pVolume = nullptr;
            char *pMuted  = nullptr;
            char *pAppVol = nullptr;
            char *pPaused = nullptr;

            char *sep1 = findSep(pTitle);
            char *sep2 = nullptr, *sep3 = nullptr, *sep4 = nullptr, *sep5 = nullptr;
            if (sep1) {
                *sep1 = '\0';
                pArtist = sep1 + 2;
                sep2 = findSep(pArtist);
            }
            if (sep2) {
                *sep2 = '\0';
                pVolume = sep2 + 2;
                sep3 = findSep(pVolume);
            }
            if (sep3) {
                *sep3 = '\0';
                pMuted = sep3 + 2;
                sep4 = findSep(pMuted);
            }
            if (sep4) {
                *sep4 = '\0';
                pAppVol = sep4 + 2;
                sep5 = findSep(pAppVol);
            }
            if (sep5) {
                *sep5 = '\0';
                pPaused = sep5 + 2;
            }

            if (pPaused != nullptr) {
                volume    = atoi(pVolume);
                bool newMuted = atoi(pMuted) != 0;
                appVolume = atoi(pAppVol);
                isPaused  = atoi(pPaused) != 0;
                if (newMuted != isMuted) {
                    isMuted = newMuted;
                    applyMuteContrast();
                }

                trimInPlace(pTitle);

#if DISPLAY_LINES == 2
                if (strcmp(pTitle, line1) != 0) {
                    strncpy(line1, pTitle, MAX_FIELD_LEN - 1);
                    line1[MAX_FIELD_LEN - 1] = '\0';
                    resetScroll(scroll[0]);
                }
                if (strcmp(pArtist, artist) != 0) {
                    strncpy(artist, pArtist, MAX_FIELD_LEN - 1);
                    artist[MAX_FIELD_LEN - 1] = '\0';
                    resetScroll(scroll[1]);
                }
#else
                splitTitleIntoLines(pTitle, line1, line2, DISPLAY_CHARS_PER_LINE);
                if (strcmp(pArtist, artist) != 0) {
                    strncpy(artist, pArtist, MAX_FIELD_LEN - 1);
                    artist[MAX_FIELD_LEN - 1] = '\0';
                    resetScroll(scroll[0]);
                }
#endif

                if (activeOverlay == OVERLAY_NONE) {
                    drawMediaDisplay();
                }

                connected      = true;
                lastUpdateTime = millis();
            }

            inputLen = 0;
        } else {
            if (inputLen < MAX_SERIAL_BUFFER - 1) {
                inputBuffer[inputLen++] = c;
            }
        }
    }
}

// Debounces the encoder's push-button and toggles SYSTEM/APP volume mode on
// each confirmed press, showing a brief "SYS VOL"/"APP VOL" banner.
void handleModeButton() {
    int reading = digitalRead(ENCODER_BTN);
    if (reading != lastRawButton) {
        lastDebounceTime = millis();
        lastRawButton = reading;
    }
    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (reading == LOW && lastButtonState == HIGH) {
            currentMode = (currentMode == SYSTEM_VOL) ? APP_VOL : SYSTEM_VOL;
            showOverlay(OVERLAY_MODE, 1000, 2, 4, 8, currentMode == SYSTEM_VOL ? "SYS VOL" : "APP VOL");
        }
        lastButtonState = reading;
    }
}

// Reads the rotary encoder, sends a volume-up/down HID key (SYSTEM mode) or an
// APPVOL serial command (APP mode), and shows a transient "Vol: NN%" readout.
void handleEncoderRotation() {
    int currentStateCLK = digitalRead(ENCODER_PIN_CLK);
    if (lastEncoderState == HIGH && currentStateCLK == LOW &&
            millis() - lastEncoderDebounceTime >= ENCODER_DEBOUNCE_MS) {
        lastEncoderDebounceTime = millis();
        bool clockwise = digitalRead(ENCODER_PIN_DT) == currentStateCLK;

        if (currentMode == SYSTEM_VOL) {
            Consumer.write(clockwise ? MEDIA_VOLUME_UP : MEDIA_VOLUME_DOWN);
        } else {
            Serial.println(clockwise ? "APPVOL:+" : "APPVOL:-");
        }

        char overlayText[16];
        snprintf(overlayText, sizeof(overlayText), "%s%d%%",
                 currentMode == SYSTEM_VOL ? "Vol: " : "App: ",
                 currentMode == SYSTEM_VOL ? volume : appVolume);
        showOverlay(OVERLAY_VOLUME, 1000, 2, 10, 8, overlayText);
    }
    lastEncoderState = currentStateCLK;
}

// Clears whichever overlay (volume/mute/mode banner) is active once its
// duration has elapsed, and restores the normal media display.
void checkOverlayTimeout() {
    if (activeOverlay != OVERLAY_NONE && (millis() - overlayStart > overlayDurationMs)) {
        activeOverlay = OVERLAY_NONE;
        drawMediaDisplay();
    }
}

// Advances the song/artist marquee scroll and redraws only when a step moved.
// Skipped while a transient overlay (volume/mute/mode banner) is on screen, or
// before the first PC message has ever arrived (nothing to scroll yet).
void handleScrollTick() {
    if (connected && activeOverlay == OVERLAY_NONE) {
        bool redraw = false;
#if DISPLAY_LINES == 2
        redraw |= tickScroll(scroll[0], (int)strlen(line1)  * CHAR_WIDTH_PX);
        redraw |= tickScroll(scroll[1], (int)strlen(artist) * CHAR_WIDTH_PX);
#else
        redraw |= tickScroll(scroll[0], (int)strlen(artist) * CHAR_WIDTH_PX);
#endif
        if (redraw) drawMediaDisplay();
    }
}

// Declares the PC connection lost after TIMEOUT ms of silence and shows a
// "No connection" screen. Cleared again as soon as a valid message arrives
// (see handleSerialInput(), which sets connected = true).
void checkSerialTimeout() {
    if (connected && (millis() - lastUpdateTime > TIMEOUT)) {
        connected = false;
        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("No connection");
        display.setCursor(0, 16);
        display.println("Awaiting update...");
        display.display();
    }
}

// Scans the 2x2 media keypad. Mute is handled locally (banner + HID mute key);
// prev/play-pause/next are sent as plain HID consumer keys with no Arduino-side
// state to track — the PC's own player reports the resulting state back on the
// next serial update.
void handleKeypad() {
    if (customKeypad.getKeys()) {
        for (int i = 0; i < LIST_MAX; i++) {
            Key k = customKeypad.key[i];
            if (k.kstate == PRESSED) {
                switch (k.kchar) {
                    case 'M':
                        isMuted = !isMuted;
                        applyMuteContrast();
                        Consumer.write(MEDIA_VOLUME_MUTE);
                        showOverlay(OVERLAY_MUTE, 1000, 3, 10, 4, isMuted ? "MUTE" : "UNMUTE");
                        break;
                    case 'R': Consumer.write(MEDIA_PREVIOUS);   break;
                    case 'P': Consumer.write(MEDIA_PLAY_PAUSE); break;
                    case 'F': Consumer.write(MEDIA_NEXT);       break;
                }
            }
        }
    }
}

// ── Main loop ─────────────────────────────────────────────────────────────────
// Order matters: checkOverlayTimeout() must run after the handlers that can
// raise an overlay, and before handleScrollTick()/checkSerialTimeout() so a
// just-cleared overlay doesn't suppress a redraw those steps would otherwise
// trigger.
void loop() {
    handleSerialInput();
    handleModeButton();
    handleEncoderRotation();
    checkOverlayTimeout();
    handleScrollTick();
    checkSerialTimeout();
    handleKeypad();
}

// ── Utilities ─────────────────────────────────────────────────────────────────
// Copies at most (MAX_FIELD_LEN - 1) chars from src into fixed-size dst, always
// null-terminating. No heap allocation.
static void copyField(char *dst, const char *src, int len) {
    if (len > MAX_FIELD_LEN - 1) len = MAX_FIELD_LEN - 1;
    if (len < 0) len = 0;
    strncpy(dst, src, len);
    dst[len] = '\0';
}

void splitTitleIntoLines(const char *title, char *out1, char *out2, int maxLen) {
    int titleLen = (int)strlen(title);
    if (titleLen <= maxLen) {
        copyField(out1, title, titleLen);
        out2[0] = '\0';
        return;
    }
    int splitPos = -1;
    for (int i = maxLen; i >= 0; i--) {
        if (title[i] == ' ') {
            splitPos = i;
            break;
        }
    }
    if (splitPos == -1) {
        copyField(out1, title, maxLen - 3);
        strcat(out1, "...");
        out2[0] = '\0';
    } else {
        copyField(out1, title, splitPos);

        const char *rest = title + splitPos + 1;
        int restLen = (int)strlen(rest);
        if (restLen > maxLen) {
            copyField(out2, rest, maxLen - 3);
            strcat(out2, "...");
        } else {
            copyField(out2, rest, restLen);
        }
    }
}
