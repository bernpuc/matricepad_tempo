#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <string.h>
#include <stdlib.h>

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

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

char inputBuffer[MAX_SERIAL_BUFFER];
int  inputLen = 0;

int barLevels[NUM_BARS] = {0};   // each 0-100, received from the PC

bool connected = false;
unsigned long lastUpdateTime = 0;

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
    display.display();
}

void setup() {
    delay(2000);
    Serial.begin(115200);
    delay(100);

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
// complete frame -- no scroll/overlay timing involved, so unlike the baseline
// sketch there's no need to gate the draw behind a separate tick handler.
void handleSerialInput() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            inputBuffer[inputLen] = '\0';

            if (inputLen > 0) {
                parseFrame(inputBuffer);
                drawBars();
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
    checkSerialTimeout();
}
