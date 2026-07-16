#pragma once
#include <Arduino.h>

// Debounced quadrature-encoder rotation detection, shared by any sketch that
// reads the same CLK/DT rotary encoder. Button-press handling (the encoder's
// own pushbutton) is sketch-specific and not covered here -- see the baseline
// sketch's handleModeButton() for that pattern.
namespace TempoCore {

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

}
