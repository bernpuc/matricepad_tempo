#include "RotaryEncoder.h"

namespace TempoCore {

void initEncoderState(EncoderState &s, int clkPin) {
    s.lastClkState     = digitalRead(clkPin);
    s.lastDebounceTime = 0;
}

bool tickEncoder(EncoderState &s, int clkPin, int dtPin, unsigned long debounceMs, bool &clockwiseOut) {
    int currentStateCLK = digitalRead(clkPin);
    bool fired = false;

    if (s.lastClkState == HIGH && currentStateCLK == LOW &&
            millis() - s.lastDebounceTime >= debounceMs) {
        s.lastDebounceTime = millis();
        clockwiseOut = digitalRead(dtPin) == currentStateCLK;
        fired = true;
    }

    s.lastClkState = currentStateCLK;
    return fired;
}

}
