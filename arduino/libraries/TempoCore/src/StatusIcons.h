#pragma once
#include <Adafruit_SSD1306.h>

// Full-screen circle glyph (mute/pause) and the transient text banner used for
// volume/mute/mode overlays. Callers own the overlay's active/duration state —
// these functions only draw a frame.
namespace TempoCore {

// Draws the 30px status circle centered at (64, 16): a mute glyph (speaker +
// X) when isMute is true, or a solid play-triangle (paused) otherwise.
void drawCircleIcon(Adafruit_SSD1306 &display, bool isMute);

// Dims the display contrast while muted so the circle reads as a clear cue.
void applyMuteContrast(Adafruit_SSD1306 &display, bool isMuted);

// Clears the screen and draws a single line of text at the given size/cursor,
// then pushes the frame. Used for volume readouts and mute/mode banners.
void showOverlayBanner(Adafruit_SSD1306 &display, int textSize, int x, int y, const char *text);

}
