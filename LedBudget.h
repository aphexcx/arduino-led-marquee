//
// How much current a frame asks of the supply, and how far its colours have to come down to stay
// under a limit.
//
// Pure functions: no hardware access, no Arduino headers, so the same file is compiled into the
// sketch and into the host unit test (test/led_budget_test.cpp).
//
// The model is the usual one for these LEDs: a colour channel at 255 draws 20 mA and the current
// is proportional to the value. So the "load" of a frame is the sum, over its lit LEDs, of the
// three values AS THEY ARE SENT, and its current is load x 20 / 255 mA. It is an upper bound, not
// a measurement: real LEDs draw less at full white, and about 1 mA per LED flows with everything
// dark. It ranks pictures and bounds frames; the limit itself has to be found on the sign.
//
// "As sent" matters. sendBitx8 (PixelBitBanging.h) sends a colour byte that starts with the carry
// flag set as v | v >> 1 (see AdDisplay.h). That is up to one and a half times the value
// (128 goes out as 192), nothing at 0 and 255, and it is not proportional: 11 goes out as 15 but
// 12 as 14. So a colour cannot be brought under a limit by one division; the values are dimmed
// first, taken as they will be sent, and the brightest dimming that fits is searched for.
//

#ifndef LED_MARQUEE_LEDBUDGET_H
#define LED_MARQUEE_LEDBUDGET_H

#include <stdint.h>

// mA drawn by one colour channel of one LED at 255
#define LED_MA_PER_CHANNEL 20

// The load that a current of `ma` mA stands for
#define LED_LOAD_OF_MA(ma) ((uint32_t) (ma) * 255 / LED_MA_PER_CHANNEL)

// What goes out for a colour byte that sendBitx8 starts with the carry flag set
static inline uint8_t ledCarried(uint8_t v) {
    return v | (v >> 1);
}

// A value dimmed by `scale`: 255 leaves it as it is, 0 makes it 0, in between in proportion.
static inline uint8_t ledScaled(uint8_t v, uint8_t scale) {
    return (uint8_t) (((uint16_t) v * ((uint16_t) scale + 1)) >> 8);
}

// What one LED of this colour sends, the three values added up, when dimmed by `scale`.
// Red and blue always go out carried; green does when greenCarried (the chonky style).
// With waveRed the red is not one value: it runs through all of them along the text, so it is
// taken at the largest value any dimmed red can go out as. redMax is that value for this scale
// (see ledBudgetScale, which keeps it up to date; it is not used otherwise).
static inline uint16_t ledSent(uint8_t r, uint8_t g, uint8_t b, uint8_t scale, bool waveRed, bool greenCarried,
                               uint8_t redMax) {
    uint16_t sent = waveRed ? redMax : ledCarried(ledScaled(r, scale));
    uint8_t green = ledScaled(g, scale);
    sent += greenCarried ? ledCarried(green) : green;
    sent += ledCarried(ledScaled(b, scale));
    return sent;
}

// What one lit LED may send, its three values added up, when `litPerSide` LEDs are lit on each
// side of the sign (both sides show the same picture) and the supply has these two limits, each a
// load (see LED_LOAD_OF_MA) and each 0 when there is no such limit: `limitBoth` for the two sides
// together, `limitSide` for one side. 765 (3 x 255) means "anything".
static inline uint16_t ledAllowance(uint16_t litPerSide, uint32_t limitBoth, uint32_t limitSide) {
    uint32_t allowed = 765;
    if (litPerSide == 0) {
        return 765;
    }
    if (limitBoth && limitBoth / 2 / litPerSide < allowed) {
        allowed = limitBoth / 2 / litPerSide;
    }
    if (limitSide && limitSide / litPerSide < allowed) {
        allowed = limitSide / litPerSide;
    }
    return (uint16_t) allowed;
}

// The brightest dimming (0 to 255, 255 = not dimmed) up to which an LED of this colour sends no
// more than `allowed` (see ledAllowance): at the dimming returned AND AT EVERY DIMMING BELOW IT.
// The second half matters because what is sent does not fall evenly with the dimming (12 goes out
// as 14, 11 as 15): a text that comes on over a ramp passes through the dimmings below its own,
// and none of those frames may be over the limit either. 255 when nothing has to be dimmed.
// Under 1 ms on the sign; call it before a message's first frame.
static inline uint8_t ledBudgetScale(uint16_t allowed, uint8_t r, uint8_t g, uint8_t b, bool waveRed,
                                     bool greenCarried) {
    if (allowed >= 765) {
        return 255; // no limit that an LED could reach: nothing to search for
    }
    uint8_t best = 0;   // dimmed to 0 nothing is sent: that is within any limit
    uint8_t redMax = 0; // waveRed: reds dimmed by `scale` are the values 0 to scale; the largest any of them is sent as
    for (uint16_t scale = 0; scale <= 255; scale++) {
        uint8_t carried = ledCarried((uint8_t) scale);
        if (carried > redMax) {
            redMax = carried;
        }
        if (ledSent(r, g, b, (uint8_t) scale, waveRed, greenCarried, redMax) > allowed) {
            break; // the first dimming that is too bright: stay below it
        }
        best = (uint8_t) scale;
    }
    return best;
}

// The dimming of frame number `frame` (0 = the first) of a text that comes up over `rampFrames`
// frames to `scale`: frame 0 has one part in rampFrames, the last frame of the ramp and every
// frame after it the whole. rampFrames 0 or 1: no ramp.
static inline uint8_t ledRampScale(uint8_t scale, uint8_t frame, uint8_t rampFrames) {
    if (rampFrames == 0 || frame >= rampFrames - 1) {
        return scale;
    }
    return (uint8_t) (((uint16_t) scale * (uint16_t) (frame + 1)) / rampFrames);
}

#endif //LED_MARQUEE_LEDBUDGET_H
