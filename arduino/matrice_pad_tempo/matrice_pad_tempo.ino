#include <Keypad.h>
#include <HID-Project.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── Display layout switch ─────────────────────────────────────────────────────
// 2 = two-line layout:   song on row 0, artist on row 1 — both scroll when long
// 3 = three-line layout: song word-wrapped across rows 0-1, artist scrolls row 2
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
  #define MUTE_ICON_Y ROW1_Y
  #define CHAR_WIDTH_PX 12   // 10px glyph + 2px spacing at textSize 2
#else
  #define ROW0_Y       0
  #define ROW1_Y      10
  #define ROW2_Y      20
  #define MUTE_ICON_Y ROW2_Y
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
volatile int encoderPosition = 0;
int lastEncoderState = LOW;

String inputBuffer = "";
int volume = 0;

unsigned long lastUpdateTime = 0;
const unsigned long TIMEOUT = 2500;
bool connected = false;

bool volumeBeingAdjusted = false;
unsigned long lastEncoderAdjustTime = 0;
bool muteDisplayed = false;
unsigned long muteDisplayStart = 0;

String line1  = "";
String line2  = "";   // used in 3-line mode only
String artist = "";

bool volumeInitialized = false;
bool isMuted = false;

// ── Scroll state ──────────────────────────────────────────────────────────────
struct LineScroll {
    int           pixel;
    unsigned long lastTime;
    uint8_t       phase;   // 0 = pause at start  1 = scrolling  2 = pause at end
};

#if DISPLAY_LINES == 2
LineScroll scroll[2];   // [0] = song line   [1] = artist line
#else
LineScroll scroll[1];   // [0] = artist line only
#endif

// ── Mute icon ─────────────────────────────────────────────────────────────────
// 8×8 bitmap: speaker body (cols 0-2), gap (col 3), X marker (cols 4-7)
static const uint8_t PROGMEM muteIcon[] = {
    0x00, 0x20, 0x69, 0xE6, 0xE6, 0x69, 0x20, 0x00
};

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
unsigned long lastDebounceTime = 0;
const int debounceDelay = 50;

// ── Scroll helpers ────────────────────────────────────────────────────────────
void resetScroll(LineScroll &s) {
    s.pixel    = 0;
    s.phase    = 0;
    s.lastTime = millis();
}

// Advance one scroll state. Returns true when the display needs a redraw.
bool tickScroll(LineScroll &s, int contentPx) {
    if (contentPx <= SCREEN_WIDTH) return false;
    int maxPx = contentPx - SCREEN_WIDTH;
    unsigned long now = millis();
    if (s.phase == 0) {
        if (now - s.lastTime >= SCROLL_PAUSE_MS) {
            s.phase    = 1;
            s.lastTime = now;
        }
    } else if (s.phase == 1) {
        if (now - s.lastTime >= SCROLL_STEP_MS) {
            s.lastTime = now;
            if (++s.pixel >= maxPx) {
                s.pixel = maxPx;
                s.phase = 2;
            }
            return true;
        }
    } else {
        if (now - s.lastTime >= SCROLL_PAUSE_MS) {
            s.pixel    = 0;
            s.phase    = 0;
            s.lastTime = now;
            return true;
        }
    }
    return false;
}

// ── Drawing ───────────────────────────────────────────────────────────────────
void drawMuteIcon() {
    if (isMuted) {
        display.drawBitmap(120, MUTE_ICON_Y, muteIcon, 8, 8, SSD1306_WHITE);
    }
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

    drawMuteIcon();
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
        scroll[i] = {0, 0, 0};
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

            if (firstSep != -1 && secondSep != -1) {
                String songTitle = inputBuffer.substring(0, firstSep);
                String newArtist = inputBuffer.substring(firstSep + 2, secondSep);
                volume = inputBuffer.substring(secondSep + 2).toInt();

                if (!volumeInitialized) {
                    encoderPosition   = volume;
                    volumeInitialized = true;
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

    // --- Encoder button (mute) ---
    int reading = digitalRead(ENCODER_BTN);
    if (reading != lastButtonState) {
        lastDebounceTime = millis();
    }
    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (reading == LOW && lastButtonState == HIGH) {
            isMuted = !isMuted;
            Serial.println("MUTE");

            display.clearDisplay();
            display.setTextSize(3);
            display.setCursor(10, 4);
            display.println(isMuted ? "MUTE" : "UNMUTE");
            display.display();

            muteDisplayed    = true;
            muteDisplayStart = millis();
        }
    }
    lastButtonState = reading;

    if (muteDisplayed && (millis() - muteDisplayStart > 1000)) {
        muteDisplayed = false;
        drawMediaDisplay();
    }

    // --- Encoder rotation (volume) ---
    int currentStateCLK = digitalRead(ENCODER_PIN_CLK);
    if (lastEncoderState == HIGH && currentStateCLK == LOW) {
        if (digitalRead(ENCODER_PIN_DT) != currentStateCLK) {
            encoderPosition -= 2;
        } else {
            encoderPosition += 2;
        }
        encoderPosition = constrain(encoderPosition, 0, 100);

        Serial.print("VOL:");
        Serial.println(encoderPosition);

        display.clearDisplay();
        display.setTextSize(2);
        display.setCursor(10, 8);
        display.print("Vol: ");
        display.print(encoderPosition);
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
                    case 'M': isMuted = !isMuted; Consumer.write(MEDIA_VOLUME_MUTE); break;
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
