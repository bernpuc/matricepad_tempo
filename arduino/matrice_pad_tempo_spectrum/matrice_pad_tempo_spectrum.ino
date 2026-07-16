#include <Keypad.h>
#include <HID-Project.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TempoCore.h>
#include <string.h>
#include <stdlib.h>

using namespace TempoCore;

// ── OLED ──────────────────────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  32
#define OLED_RESET      -1
#define OLED_SDA         2
#define OLED_SCL         3
#define MAX_SERIAL_BUFFER 256

// ── Bar layout ────────────────────────────────────────────────────────────────
// 16 bars * 8px slot (6px bar + 2px gap) = 128px, exactly fills the screen width.
#define NUM_BARS    16
#define BAR_WIDTH    6
#define BAR_GAP      2
#define BAR_SLOT    (BAR_WIDTH + BAR_GAP)

#define TIMEOUT 5000

// ── Encoder pins ──────────────────────────────────────────────────────────────
// System volume only in this sketch -- no SYS/APP mode toggle, so the
// encoder's pushbutton (pin 19) is unused here.
#define ENCODER_PIN_CLK 20
#define ENCODER_PIN_DT  21
const unsigned long ENCODER_DEBOUNCE_MS = 10;
EncoderState encoderState;

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

char inputBuffer[MAX_SERIAL_BUFFER];
int  inputLen = 0;

int barLevels[NUM_BARS] = {0};   // each 0-100, received from the PC

bool connected = false;
unsigned long lastUpdateTime = 0;

bool isMuted  = false;
bool isPaused = false;   // local toggle on keypress -- this sketch has no PC-reported playback status to confirm against, unlike the baseline's media protocol

// ── Transient overlay state ───────────────────────────────────────────────────
// A single active "Vol" banner replaces the bar graph for a fixed duration,
// then reverts. Mute has no banner -- the mute icon already shows that state.
bool          overlayActive     = false;
unsigned long overlayStart      = 0;
unsigned long overlayDurationMs = 0;

// Parses a "n,n,n,...\n" line (already null-terminated, no trailing \n) into
// barLevels. Missing/malformed trailing fields are left at their previous
// value rather than reset to 0, since a dropped/short frame shouldn't blank
// the display.
void parseFrame(char *line) {
    char *token = strtok(line, ",");
    for (int i = 0; i < NUM_BARS && token != nullptr; i++) {
        int level = atoi(token);
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        barLevels[i] = level;
        token = strtok(nullptr, ",");
    }
}

void drawBars() {
    display.clearDisplay();
    for (int i = 0; i < NUM_BARS; i++) {
        int barHeightPx = (barLevels[i] * SCREEN_HEIGHT) / 100;
        if (barHeightPx > 0) {
            int x = i * BAR_SLOT;
            display.fillRect(x, SCREEN_HEIGHT - barHeightPx, BAR_WIDTH, barHeightPx, SSD1306_WHITE);
        }
    }
    if (isMuted) {
        drawCircleIcon(display, true);
    } else if (isPaused) {
        drawCircleIcon(display, false);
    }
    display.display();
}

// Draws a full-screen single-line banner and marks it as the active overlay
// for durationMs. checkOverlayTimeout() reverts to drawBars() once that time
// has elapsed.
void showOverlay(unsigned long durationMs, int textSize, int x, int y, const char *text) {
    showOverlayBanner(display, textSize, x, y, text);

    overlayActive     = true;
    overlayStart       = millis();
    overlayDurationMs = durationMs;
}

void setup() {
    delay(2000);
    Serial.begin(115200);
    delay(100);
    Consumer.begin();

    pinMode(ENCODER_PIN_CLK, INPUT_PULLUP);
    pinMode(ENCODER_PIN_DT,  INPUT_PULLUP);
    initEncoderState(encoderState, ENCODER_PIN_CLK);

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

// Reads "n,n,n,...\n" lines from the PC and redraws the bar graph on each
// complete frame, unless a transient overlay (volume/mute banner) is showing.
void handleSerialInput() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            inputBuffer[inputLen] = '\0';

            if (inputLen > 0) {
                parseFrame(inputBuffer);
                if (!overlayActive) {
                    drawBars();
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

// Reads the rotary encoder and sends a system volume-up/down HID key, showing
// a brief "Vol" tick -- no percentage, since this sketch never receives system
// volume back from the PC (unlike the baseline sketch's media protocol).
void handleEncoderRotation() {
    bool clockwise;
    if (tickEncoder(encoderState, ENCODER_PIN_CLK, ENCODER_PIN_DT, ENCODER_DEBOUNCE_MS, clockwise)) {
        Consumer.write(clockwise ? MEDIA_VOLUME_UP : MEDIA_VOLUME_DOWN);
        showOverlay(1000, 2, 34, 8, "Vol");
    }
}

// Scans the 2x2 media keypad. Mute is handled locally (banner + HID mute key);
// prev/play-pause/next are sent as plain HID consumer keys.
void handleKeypad() {
    if (customKeypad.getKeys()) {
        for (int i = 0; i < LIST_MAX; i++) {
            Key k = customKeypad.key[i];
            if (k.kstate == PRESSED) {
                switch (k.kchar) {
                    case 'M':
                        isMuted = !isMuted;
                        applyMuteContrast(display, isMuted);
                        Consumer.write(MEDIA_VOLUME_MUTE);
                        if (!overlayActive) drawBars();
                        break;
                    case 'R': Consumer.write(MEDIA_PREVIOUS);   break;
                    case 'P':
                        isPaused = !isPaused;
                        Consumer.write(MEDIA_PLAY_PAUSE);
                        if (!overlayActive) drawBars();
                        break;
                    case 'F': Consumer.write(MEDIA_NEXT);       break;
                }
            }
        }
    }
}

// Clears the active overlay (volume/mute banner) once its duration has
// elapsed, and restores the bar graph.
void checkOverlayTimeout() {
    if (overlayActive && (millis() - overlayStart > overlayDurationMs)) {
        overlayActive = false;
        drawBars();
    }
}

// Declares the PC connection lost after TIMEOUT ms of silence and shows a
// "No connection" screen. Cleared again as soon as a valid frame arrives.
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

void loop() {
    handleSerialInput();
    handleEncoderRotation();
    handleKeypad();
    checkOverlayTimeout();
    checkSerialTimeout();
}
