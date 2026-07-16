#pragma once
#include <Arduino.h>

// Marquee scroll engine shared by every display mode: a line pauses at each
// end for SCROLL_PAUSE_MS, then steps one pixel every SCROLL_STEP_MS until it
// reaches the far end, where it pauses again and reverses.
namespace TempoCore {

struct LineScroll {
    int           pixel;
    unsigned long lastTime;
    int8_t        dir;   // 0 = paused, +1 = scrolling forward, -1 = scrolling backward
};

void resetScroll(LineScroll &s);

// Advances one scroll state. Returns true when the display needs a redraw.
// contentPx is the full rendered width of the text; screenWidth is the visible
// window. No-ops (returns false) when the content already fits on screen.
bool tickScroll(LineScroll &s, int contentPx, int screenWidth,
                 unsigned long pauseMs, unsigned long stepMs);

}
