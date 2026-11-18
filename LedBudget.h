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
// a measurement: real LEDs draw less at full white. And it is only what the lit channels add: about
// 1 mA per LED flows with everything dark (some 0.84 A for the sign's 840 LEDs, all the time), and
// that is NOT in the load or in the limits. A limit of 5500 mA means 5.5 A above that idle current.
// The model ranks pictures and bounds frames; the limit itself has to be found on the sign.
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

// How many bits of a byte are set: the lit LEDs of a column
static inline uint8_t ledBits(uint8_t v) {
    uint8_t n = 0;
    for (; v; v &= v - 1) {
        n++;
    }
    return n;
}

// What goes out for a colour byte that sendBitx8 starts with the carry flag set
static inline uint8_t ledCarried(uint8_t v) {
    return v | (v >> 1);
}

// A value dimmed by `scale`: 255 leaves it as it is, 0 makes it 0, in between in proportion.
static inline uint8_t ledScaled(uint8_t v, uint8_t scale) {
    return (uint8_t) (((uint16_t) v * ((uint16_t) scale + 1)) >> 8);
}

// A value after the sign's brightness shift (the 'B' utility message: every value is shifted
// right by it before it is sent).
// The sketch never stores a shift above 8 (BRIGHTNESS_SHIFT_MAX): the handler of the message
// brings the number into 0 to 8 before it stores it, and the sender and this estimate both read
// that stored value. At 8 nothing is left of a value.
static inline uint8_t ledShifted(uint8_t v, uint8_t shift) {
    return shift >= 8 ? 0 : (uint8_t) (v >> shift);
}

// What one LED of this colour sends at most, the three values added up, when dimmed by `scale`
// and then shifted right by the brightness shift.
// Red and blue always go out carried; green does when greenCarried (the chonky style). With a
// brightness shift the carry of every byte is whatever the shifting left in the flag, so all
// three are then taken as carried: an upper bound.
// With waveRed the red is not one value: it runs through all of them along the text, so it is
// taken at the largest value any dimmed red can go out as. redMax is that value for this scale
// (see ledBudgetScale, which keeps it up to date; it is not used otherwise).
static inline uint16_t ledSent(uint8_t r, uint8_t g, uint8_t b, uint8_t scale, bool waveRed, bool greenCarried,
                               uint8_t redMax, uint8_t shift) {
    uint16_t sent = waveRed ? redMax : ledCarried(ledShifted(ledScaled(r, scale), shift));
    uint8_t green = ledShifted(ledScaled(g, scale), shift);
    sent += (greenCarried || shift) ? ledCarried(green) : green;
    sent += ledCarried(ledShifted(ledScaled(b, scale), shift));
    return sent;
}

// What one lit LED may send, its three values added up, when `litPerSide` LEDs are lit on each
// side of the sign (or, where the two sides differ, on the fuller one) and the supply has these two limits, each a
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
// more than `allowed` (see ledAllowance). 255 when nothing has to be dimmed.
// What is sent does not fall evenly with the dimming (12 goes out as 14, 11 as 15). A text that
// comes on over a ramp passes through the dimmings below its own, and none of those frames may be
// over the limit either: for such a text the answer is the brightest dimming at which it fits
// AND AT EVERY DIMMING BELOW IT.
// `shift` is the sign's brightness shift at the moment (0 = none): a text that the shift already
// brings under the limit is not dimmed a second time.
// `everyDimmingBelow`: true for a picture that passes through the dimmings below its own (a text
// that comes on over a ramp, a fade). Then the answer is the brightest dimming BELOW WHICH EVERY
// DIMMING FITS, see above. False for a picture that is only ever shown at its own dimming: then it
// is simply the brightest dimming that fits.
// About 1.5 ms on the sign when there is something to search for; call it before a frame, not in one.
static inline uint8_t ledBudgetScale(uint16_t allowed, uint8_t r, uint8_t g, uint8_t b, bool waveRed,
                                     bool greenCarried, uint8_t shift, bool everyDimmingBelow) {
    if (allowed >= 765) {
        return 255; // no limit that an LED could reach: nothing to search for
    }
    uint8_t best = 0;   // dimmed to 0 nothing is sent: that is within any limit
    uint8_t redMax = 0; // waveRed: reds dimmed by `scale` are the values 0 to scale; the largest any of them is sent as
    for (uint16_t scale = 0; scale <= 255; scale++) {
        uint8_t carried = ledCarried(ledShifted((uint8_t) scale, shift));
        if (carried > redMax) {
            redMax = carried;
        }
        if (ledSent(r, g, b, (uint8_t) scale, waveRed, greenCarried, redMax, shift) > allowed) {
            if (everyDimmingBelow) {
                break; // the first dimming that is too bright: stay below it
            }
            continue;  // this one does not fit; a brighter one still may (11 goes out as 15, 12 as 14)
        }
        best = (uint8_t) scale;
    }
    return best;
}

// The dimming of frame number `frame` (0 = the first) of a text that comes up over `rampFrames`
// frames to `scale`: frame 0 has one part in rampFrames, the last frame of the ramp and every
// frame after it the whole. rampFrames 0 or 1: no ramp. ("Frame" is a place in time: the ramp's
// time cut into as many parts. A text whose frames come faster or slower than that still comes on
// over the ramp's time: see ledRampFrame.)
static inline uint8_t ledRampScale(uint8_t scale, uint8_t frame, uint8_t rampFrames) {
    if (rampFrames == 0 || frame >= rampFrames - 1) {
        return scale;
    }
    return (uint8_t) (((uint16_t) scale * (uint16_t) (frame + 1)) / rampFrames);
}

// Which part of the ramp a frame belongs to that is drawn `elapsed` after the text first came on
// (elapsed and partTicks in the same unit of time, partTicks > 0). From rampFrames on the ramp is over.
static inline uint8_t ledRampFrame(uint32_t elapsed, uint32_t partTicks, uint8_t rampFrames) {
    uint32_t frame = elapsed / partTicks;
    return frame >= rampFrames ? rampFrames : (uint8_t) frame;
}

#endif //LED_MARQUEE_LEDBUDGET_H
