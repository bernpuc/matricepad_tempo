#include <Keypad.h>
#include <Keyboard.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED configuration
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET     -1
#define OLED_SDA       2
#define OLED_SCL       3

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

// Track if we’ve synced volume from PC yet
bool volumeInitialized = false;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const byte ROWS = 2;
const byte COLS = 2;

char hexaKeys[ROWS][COLS] =
{
    {KEY_F18 , KEY_F18 },
    {KEY_F18 , KEY_F18 },
};

byte rowPins[ROWS] = {14, 15};
byte colPins[COLS] = {10, 16};

Keypad customKeypad = Keypad(makeKeymap(hexaKeys), rowPins, colPins, ROWS, COLS);

// Button debounce
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const int debounceDelay = 50;

void setup() {
    Serial.begin(115200);
    Keyboard.begin();

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
    display.setCursor(0, 0);
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
                artist = inputBuffer.substring(firstSep + 2, secondSep);
                volume = inputBuffer.substring(secondSep + 2).toInt();

                if (!volumeInitialized) {
                    encoderPosition = volume;
                    volumeInitialized = true;
                }

                songTitle.trim();
                splitTitleIntoLines(songTitle, line1, line2, 28);

                if (!volumeBeingAdjusted && !muteDisplayed) {
                    display.clearDisplay();
                    display.setTextSize(1);
                    display.setCursor(0, 0);
                    display.println(line1);
                    display.setCursor(0, 8);
                    display.println(line2);

                    if (artist.length() > 28) {
                        artist = artist.substring(0, 25) + "...";
                    }

                    display.setCursor(0, 24);
                    display.println(artist);
                    display.display();
                }

                connected = true;
                lastUpdateTime = millis();
            }

            inputBuffer = "";
        } else {
            inputBuffer += c;
        }
    }

    // --- Handle Encoder Button (Mute) ---
    int reading = digitalRead(ENCODER_BTN);

    if (reading != lastButtonState) {
        lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (reading == LOW && lastButtonState == HIGH) {
            Serial.println("MUTE");  // Send to Python

            // Display large "MUTE"
            display.clearDisplay();
            display.setTextSize(3);
            display.setCursor(10, 5);
            display.println("MUTE");
            display.display();

            muteDisplayed = true;
            muteDisplayStart = millis();
        }
    }

    lastButtonState = reading;

    // Clear mute display after 1s
    if (muteDisplayed && (millis() - muteDisplayStart > 1000)) {
        muteDisplayed = false;
        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println(line1);
        display.setCursor(0, 8);
        display.println(line2);
        display.setCursor(0, 24);
        display.println(artist);
        display.display();
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
        display.setCursor(10, 10);
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
            display.clearDisplay();
            display.setTextSize(1);
            display.setCursor(0, 0);
            display.println(line1);
            display.setCursor(0, 8);
            display.println(line2);

            if (artist.length() > 28) {
                artist = artist.substring(0, 25) + "...";
            }

            display.setCursor(0, 24);
            display.println(artist);
            display.display();
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
            if (k.kstate == PRESSED || k.kstate == HOLD) {
                Keyboard.press(k.kchar);
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
