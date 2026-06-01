#include <Keypad.h>
#include <HID-Project.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

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

String inputBuffer = "";
int volume    = 0;
int appVolume = 0;

unsigned long lastUpdateTime = 0;
const unsigned long TIMEOUT = 5000;
bool connected = false;

bool volumeBeingAdjusted = false;
unsigned long lastEncoderAdjustTime = 0;
bool muteDisplayed = false;
unsigned long muteDisplayStart = 0;

String line1  = "";
String line2  = "";   // used in 3-line mode only
String artist = "";

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

// ── Main loop ─────────────────────────────────────────────────────────────────
void loop() {

    // --- Serial from PC ---
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            int firstSep  = inputBuffer.indexOf("||");
            int secondSep = inputBuffer.indexOf("||", firstSep + 2);
            int thirdSep  = inputBuffer.indexOf("||", secondSep + 2);
            int fourthSep = inputBuffer.indexOf("||", thirdSep + 2);
            int fifthSep  = inputBuffer.indexOf("||", fourthSep + 2);

            if (firstSep != -1 && secondSep != -1 && thirdSep != -1 && fourthSep != -1 && fifthSep != -1) {
                String songTitle = inputBuffer.substring(0, firstSep);
                String newArtist = inputBuffer.substring(firstSep + 2, secondSep);
                volume    = inputBuffer.substring(secondSep + 2, thirdSep).toInt();
                bool newMuted = inputBuffer.substring(thirdSep + 2, fourthSep).toInt() != 0;
                appVolume = inputBuffer.substring(fourthSep + 2, fifthSep).toInt();
                isPaused  = inputBuffer.substring(fifthSep + 2).toInt() != 0;
                if (newMuted != isMuted) {
                    isMuted = newMuted;
                    applyMuteContrast();
                }

                songTitle.trim();

#if DISPLAY_LINES == 2
                if (songTitle != line1) {
                    line1 = songTitle;
                    resetScroll(scroll[0]);
                }
                if (newArtist != artist) {
                    artist = newArtist;
                    resetScroll(scroll[1]);
                }
#else
                splitTitleIntoLines(songTitle, line1, line2, DISPLAY_CHARS_PER_LINE);
                if (newArtist != artist) {
                    artist = newArtist;
                    resetScroll(scroll[0]);
                }
#endif

                if (!volumeBeingAdjusted && !muteDisplayed) {
                    drawMediaDisplay();
                }

                connected      = true;
                lastUpdateTime = millis();
            }

            inputBuffer = "";
        } else {
            if (inputBuffer.length() < MAX_SERIAL_BUFFER) {
                inputBuffer += c;
            }
        }
    }

    // --- Encoder button (mode toggle: SYSTEM <-> APP volume) ---
    int reading = digitalRead(ENCODER_BTN);
    if (reading != lastRawButton) {
        lastDebounceTime = millis();
        lastRawButton = reading;
    }
    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (reading == LOW && lastButtonState == HIGH) {
            currentMode = (currentMode == SYSTEM_VOL) ? APP_VOL : SYSTEM_VOL;

            display.clearDisplay();
            display.setTextSize(2);
            display.setCursor(4, 8);
            display.println(currentMode == SYSTEM_VOL ? "SYS VOL" : "APP VOL");
            display.display();

            muteDisplayed    = true;
            muteDisplayStart = millis();
        }
        lastButtonState = reading;
    }

    if (muteDisplayed && (millis() - muteDisplayStart > 1000)) {
        muteDisplayed = false;
        drawMediaDisplay();
    }

    // --- Encoder rotation (volume) ---
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

        display.clearDisplay();
        display.setTextSize(2);
        display.setCursor(10, 8);
        display.print(currentMode == SYSTEM_VOL ? "Vol: " : "App: ");
        display.print(currentMode == SYSTEM_VOL ? volume : appVolume);
        display.print("%");
        display.display();

        volumeBeingAdjusted   = true;
        lastEncoderAdjustTime = millis();
    }
    lastEncoderState = currentStateCLK;

    if (volumeBeingAdjusted && (millis() - lastEncoderAdjustTime > 1000)) {
        volumeBeingAdjusted = false;
        if (!muteDisplayed) drawMediaDisplay();
    }

    // --- Scroll tick ---
    if (connected && !volumeBeingAdjusted && !muteDisplayed) {
        bool redraw = false;
#if DISPLAY_LINES == 2
        redraw |= tickScroll(scroll[0], (int)line1.length()  * CHAR_WIDTH_PX);
        redraw |= tickScroll(scroll[1], (int)artist.length() * CHAR_WIDTH_PX);
#else
        redraw |= tickScroll(scroll[0], (int)artist.length() * CHAR_WIDTH_PX);
#endif
        if (redraw) drawMediaDisplay();
    }

    // --- Serial timeout ---
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

    // --- Keypad ---
    if (customKeypad.getKeys()) {
        for (int i = 0; i < LIST_MAX; i++) {
            Key k = customKeypad.key[i];
            if (k.kstate == PRESSED) {
                switch (k.kchar) {
                    case 'M':
                        isMuted = !isMuted;
                        applyMuteContrast();
                        Consumer.write(MEDIA_VOLUME_MUTE);
                        display.clearDisplay();
                        display.setTextSize(3);
                        display.setCursor(10, 4);
                        display.println(isMuted ? "MUTE" : "UNMUTE");
                        display.display();
                        muteDisplayed    = true;
                        muteDisplayStart = millis();
                        break;
                    case 'R': Consumer.write(MEDIA_PREVIOUS);   break;
                    case 'P': Consumer.write(MEDIA_PLAY_PAUSE); break;
                    case 'F': Consumer.write(MEDIA_NEXT);       break;
                }
            }
        }
    }
}

// ── Utilities ─────────────────────────────────────────────────────────────────
void splitTitleIntoLines(const String &title, String &out1, String &out2, int maxLen) {
    if ((int)title.length() <= maxLen) {
        out1 = title;
        out2 = "";
        return;
    }
    int splitPos = -1;
    for (int i = maxLen; i >= 0; i--) {
        if (title.charAt(i) == ' ') {
            splitPos = i;
            break;
        }
    }
    if (splitPos == -1) {
        out1 = title.substring(0, maxLen - 3) + "...";
        out2 = "";
    } else {
        out1 = title.substring(0, splitPos);
        out2 = title.substring(splitPos + 1);
        if ((int)out2.length() > maxLen) {
            out2 = out2.substring(0, maxLen - 3) + "...";
        }
    }
}
