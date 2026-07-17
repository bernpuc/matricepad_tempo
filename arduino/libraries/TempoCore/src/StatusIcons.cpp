#include "StatusIcons.h"

namespace TempoCore {

void drawCircleIcon(Adafruit_SSD1306 &display, bool isMute) {
    display.fillCircle(64, 16, 15, SSD1306_WHITE);
    // Thin black ring inset from the outer edge, breaking up the flat white
    // disc with a bit of definition.
    display.drawCircle(64, 16, 14, SSD1306_BLACK);
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

void applyMuteContrast(Adafruit_SSD1306 &display, bool isMuted) {
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(isMuted ? 10 : 255);
}

void showOverlayBanner(Adafruit_SSD1306 &display, int textSize, int x, int y, const char *text) {
    display.clearDisplay();
    display.setTextSize(textSize);
    display.setCursor(x, y);
    display.println(text);
    display.display();
}

}
