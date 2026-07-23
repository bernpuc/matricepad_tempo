#pragma once
#include <Arduino.h>

// Debounced quadrature-encoder rotation detection. Button-press handling (the
// encoder's own pushbutton) is separate -- see handleDisplayModeButton() in
// the main sketch.

struct EncoderState {
    int           lastClkState;
    unsigned long lastDebounceTime;
};

// Call once in setup() after configuring clkPin as an input.
void initEncoderState(EncoderState &s, int clkPin);

// Call every loop() iteration. Returns true exactly once per detent (on the
// CLK falling edge, after debounceMs has elapsed since the last one), with
// clockwiseOut set to the turn direction.
bool tickEncoder(EncoderState &s, int clkPin, int dtPin, unsigned long debounceMs, bool &clockwiseOut);
