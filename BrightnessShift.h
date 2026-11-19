//
// The number of the 'B' utility message ("!B3" in the app): how far every colour value is shifted
// right before it is sent. 0 is full brightness, every step halves it, 8 leaves nothing.
//
// A pure function: no hardware access, no Arduino headers, so the same file is compiled into the
// sketch and into the host unit test (test/brightness_shift_test.cpp).
//

#ifndef LED_MARQUEE_BRIGHTNESSSHIFT_H
#define LED_MARQUEE_BRIGHTNESSSHIFT_H

#include <stdint.h>

// A colour value is 8 bits: shifted by 8 it is 0
#define BRIGHTNESS_SHIFT_MAX 8

// The brightness shift a text asks for, 0 to `max`. The text is read as atoi() reads it (blanks, a
// sign, digits; anything else ends the number; no digits is 0), except that the number cannot wrap:
// the count stops growing once it is above `max`, so "32776" and "4294967304" are simply more than
// `max`, where atoi() on this processor makes -32760 and 8 of them.
// More than max is max (as dark as it gets). Below 0 is 0, as a text that is not a number is.
static inline uint8_t brightnessShiftFromText(const char* s, uint8_t max) {
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) {
        s++;
    }
    bool negative = false;
    if (*s == '+' || *s == '-') {
        negative = *s == '-';
        s++;
    }
    uint16_t value = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        if (value <= max) {
            value = (uint16_t) (value * 10 + (*s - '0')); // at most max * 10 + 9: 2559 for the largest max
        }
    }
    if (negative) {
        return 0;
    }
    return value > max ? max : (uint8_t) value;
}

#endif //LED_MARQUEE_BRIGHTNESSSHIFT_H
