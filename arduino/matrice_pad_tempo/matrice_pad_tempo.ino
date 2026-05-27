#include <Keypad.h>
#include <HID-Project.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED configuration
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET     -1
#define OLED_SDA       2
#define OLED_SCL       3
// 128px / 6px-per-char (5px glyph + 1px spacing) = 21 chars at text size 1
#define DISPLAY_CHARS_PER_LINE 21
#define MAX_SERIAL_BUFFER 256

// Layout y-positions (32px height, 3 rows at 10px spacing)
#define ROW0_Y  0   // song line 1
#define ROW1_Y 10   // song line 2
#define ROW2_Y 20   // artist (mute icon overlaps right end)

// Encoder pins
#define ENCODER_PIN_CLK 20
#define ENCODER_PIN_DT  21
#define ENCODER_BTN     19

volatile int encoderPosition = 0;
int lastEncoderState = LOW;

String inputBuffer = "";
String mediaTitle = "";
int volume = 0;

unsigned long lastUpdateTime = 0;
const unsigned long TIMEOUT = 2500;
bool connected = false;

// Display state
bool volumeBeingAdjusted = false;
unsigned long lastEncoderAdjustTime = 0;
bool muteDisplayed = false;
unsigned long muteDisplayStart = 0;

// Last known display data
String line1 = "";
String line2 = "";
String artist = "";

// Artist scroll state
int artistScrollPixel = 0;
unsigned long lastArtistScrollTime = 0;
uint8_t artistScrollPhase = 0;  // 0=pause at start, 1=scrolling, 2=pause at end
#define ARTIST_SCROLL_PAUSE_MS 2000
#define ARTIST_SCROLL_STEP_MS  40

// Track if we've synced volume from PC yet
bool volumeInitialized = false;
bool isMuted = false;

// 8x8 muted-speaker bitmap: speaker body (cols 0-2), gap (col 3), X marker (cols 4-7)
static const uint8_t PROGMEM muteIcon[] = {
    0x00, 0x20, 0x69, 0xE6, 0xE6, 0x69, 0x20, 0x00
};

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const byte ROWS = 2;
const byte COLS = 2;

char hexaKeys[ROWS][COLS] =
{
    {'M', 'R'},   // Mute, Previous track
    {'P', 'F'},   // Play/Pause, Next track
};

byte rowPins[ROWS] = {14, 15};
byte colPins[COLS] = {10, 16};

Keypad customKeypad = Keypad(makeKeymap(hexaKeys), rowPins, colPins, ROWS, COLS);

// Button debounce
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const int debounceDelay = 50;

void drawMuteIcon() {
    if (isMuted) {
        display.drawBitmap(120, ROW2_Y, muteIcon, 8, 8, SSD1306_WHITE);
    }
}

void drawMediaDisplay() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, ROW0_Y);
    display.println(line1);
    display.setCursor(0, ROW1_Y);
    display.println(line2);
    display.setCursor(-artistScrollPixel, ROW2_Y);
    display.println(artist);
    drawMuteIcon();
    display.display();
}

void setup() {
    delay(2000);
    Serial.begin(115200);
    delay(100);
    Consumer.begin();

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    pinMode(ENCODER_PIN_CLK, INPUT_PULLUP);
    pinMode(ENCODER_PIN_DT, INPUT_PULLUP);
    pinMode(ENCODER_BTN, INPUT_PULLUP);
    lastEncoderState = digitalRead(ENCODER_PIN_CLK);

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        while (1); // Stop if OLED fails
    }

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 12);  // vertically centered in 32px
    display.println("Waiting for data...");
    display.display();

    lastUpdateTime = millis();
}

void loop() {
    // --- Handle Serial from PC ---
    while (Serial.available()) {
        char c = Serial.read();

        if (c == '\n') {
            int firstSep = inputBuffer.indexOf("||");
            int secondSep = inputBuffer.indexOf("||", firstSep + 2);

            if (firstSep != -1 && secondSep != -1) {
                String songTitle = inputBuffer.substring(0, firstSep);
                String newArtist = inputBuffer.substring(firstSep + 2, secondSep);
                volume = inputBuffer.substring(secondSep + 2).toInt();

                if (!volumeInitialized) {
                    encoderPosition = volume;
                    volumeInitialized = true;
                }

                songTitle.trim();
                splitTitleIntoLines(songTitle, line1, line2, DISPLAY_CHARS_PER_LINE);

                if (newArtist != artist) {
                    artist = newArtist;
                    artistScrollPixel = 0;
                    artistScrollPhase = 0;
                    lastArtistScrollTime = millis();
                }

                if (!volumeBeingAdjusted && !muteDisplayed) {
                    drawMediaDisplay();
                }

                connected = true;
                lastUpdateTime = millis();
            }

            inputBuffer = "";
        } else {
            if (inputBuffer.length() < MAX_SERIAL_BUFFER) {
                inputBuffer += c;
            }
        }
    }

    // --- Handle Encoder Button (Mute) ---
    int reading = digitalRead(ENCODER_BTN);

    if (reading != lastButtonState) {
        lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (reading == LOW && lastButtonState == HIGH) {
            isMuted = !isMuted;
            Serial.println("MUTE");  // Send to Python

            // Display large "MUTE" / "UNMUTE" feedback, vertically centered
            display.clearDisplay();
            display.setTextSize(3);
            display.setCursor(10, 4);  // vertically centered in 32px for text size 3 (24px tall)
            display.println(isMuted ? "MUTE" : "UNMUTE");
            display.display();

            muteDisplayed = true;
            muteDisplayStart = millis();
        }
    }

    lastButtonState = reading;

    // Clear mute display after 1s
    if (muteDisplayed && (millis() - muteDisplayStart > 1000)) {
        muteDisplayed = false;
        drawMediaDisplay();
    }

    // --- Handle Encoder Rotation ---
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
        display.setCursor(10, 8);  // vertically centered in 32px for text size 2 (16px tall)
        display.print("Vol: ");
        display.print(encoderPosition);
        display.print("%");
        display.display();

        volumeBeingAdjusted = true;
        lastEncoderAdjustTime = millis();
    }

    lastEncoderState = currentStateCLK;

    // --- Restore OLED after volume inactivity ---
    if (volumeBeingAdjusted && (millis() - lastEncoderAdjustTime > 1000)) {
        volumeBeingAdjusted = false;
        if (!muteDisplayed) {
            drawMediaDisplay();
        }
    }

    // --- Artist scroll tick ---
    if (connected && !volumeBeingAdjusted && !muteDisplayed) {
        int fullWidth = (int)artist.length() * 6;  // 6px per char at textSize 1
        if (fullWidth > SCREEN_WIDTH) {
            int maxScroll = fullWidth - SCREEN_WIDTH;
            unsigned long now = millis();
            if (artistScrollPhase == 0) {
                if (now - lastArtistScrollTime >= ARTIST_SCROLL_PAUSE_MS) {
                    artistScrollPhase = 1;
                    lastArtistScrollTime = now;
                }
            } else if (artistScrollPhase == 1) {
                if (now - lastArtistScrollTime >= ARTIST_SCROLL_STEP_MS) {
                    lastArtistScrollTime = now;
                    artistScrollPixel++;
                    if (artistScrollPixel >= maxScroll) {
                        artistScrollPixel = maxScroll;
                        artistScrollPhase = 2;
                    }
                    drawMediaDisplay();
                }
            } else {
                if (now - lastArtistScrollTime >= ARTIST_SCROLL_PAUSE_MS) {
                    artistScrollPixel = 0;
                    artistScrollPhase = 0;
                    lastArtistScrollTime = now;
                    drawMediaDisplay();
                }
            }
        }
    }

    // --- Serial Timeout ---
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

    // --- Handle Keypad ---
    if (customKeypad.getKeys()) {
        for (int i = 0; i < LIST_MAX; i++) {
            Key k = customKeypad.key[i];
            if (k.kstate == PRESSED) {
                switch (k.kchar) {
                    case 'M': isMuted = !isMuted; Consumer.write(MEDIA_VOLUME_MUTE); break;
                    case 'R': Consumer.write(MEDIA_PREVIOUS);    break;
                    case 'P': Consumer.write(MEDIA_PLAY_PAUSE);  break;
                    case 'F': Consumer.write(MEDIA_NEXT);        break;
                }
            }
        }
    }
}

// --- Split text into two lines without cutting words ---
void splitTitleIntoLines(const String &title, String &line1, String &line2, int maxLen) {
    if (title.length() <= maxLen) {
        line1 = title;
        line2 = "";
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
        line1 = title.substring(0, maxLen - 3) + "...";
        line2 = "";
    } else {
        line1 = title.substring(0, splitPos);
        line2 = title.substring(splitPos + 1);
        if (line2.length() > maxLen) {
            line2 = line2.substring(0, maxLen - 3) + "...";
        }
    }
}
