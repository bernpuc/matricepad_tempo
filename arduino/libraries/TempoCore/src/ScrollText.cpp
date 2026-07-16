#include "ScrollText.h"

namespace TempoCore {

void resetScroll(LineScroll &s) {
    s.pixel    = 0;
    s.dir      = 0;
    s.lastTime = millis();
}

bool tickScroll(LineScroll &s, int contentPx, int screenWidth,
                 unsigned long pauseMs, unsigned long stepMs) {
    if (contentPx <= screenWidth) return false;
    int maxPx = contentPx - screenWidth;
    unsigned long now = millis();

    if (s.dir == 0) {
        if (now - s.lastTime >= pauseMs) {
            s.dir      = (s.pixel == 0) ? 1 : -1;
            s.lastTime = now;
        }
    } else {
        if (now - s.lastTime >= stepMs) {
            s.lastTime = now;
            s.pixel   += s.dir;
            if (s.pixel >= maxPx) { s.pixel = maxPx; s.dir = 0; }
            else if (s.pixel <= 0) { s.pixel = 0;    s.dir = 0; }
            return true;
        }
    }
    return false;
}

}
