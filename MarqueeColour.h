//
// The colours of a marquee message's letters: the colour wheel they move through, and their flash
// on the beat. marquee() in the sketch and the jump game's score and runner (GameFrame.h) use these
// two functions, so the game moves and flashes as a marquee message does.
//
// Pure: no hardware and no Arduino functions, only the gamma table (Gamma.h). The host test of the
// game (test/game_frame_test.cpp) runs the same file.
//

#ifndef LED_MARQUEE_MARQUEECOLOUR_H
#define LED_MARQUEE_MARQUEECOLOUR_H

#include "Color.h"
#include "Gamma.h"

#define JAB_MAX_BRIGHTNESS 0xff //(255 (100%))
//#define JAB_MAX_BRIGHTNESS 0x7f //(127)
#define JAB_MIN_BRIGHTNESS 0x00
#define JAB_STEPS (JAB_MAX_BRIGHTNESS-JAB_MIN_BRIGHTNESS)

// The wheel has this many sectors of JAB_STEPS + 1 steps each: green to red, red to blue, blue to green
#define MARQUEE_WHEEL_SECTORS 3

const unsigned int FADE_MILLIS = 250; // number of ms beat flash takes to fade out

// The colour at `step` (0 to JAB_STEPS) of sector `sector` (0 to MARQUEE_WHEEL_SECTORS - 1) of the
// wheel. One channel ramps up and another down, both through the gamma table.
static Color marqueeWheel(uint sector, uint step) {
    const uint rampup = GAMMA(JAB_MIN_BRIGHTNESS + step);
    const uint rampdown = GAMMA(JAB_MIN_BRIGHTNESS + (JAB_STEPS - step));
    switch (sector) {
        case 0:
            return Color{rampup, rampdown, JAB_MIN_BRIGHTNESS};
        case 1:
            return Color{rampdown, JAB_MIN_BRIGHTNESS, rampup};
        default:
            return Color{JAB_MIN_BRIGHTNESS, rampup, rampdown};
    }
}

// One channel of a colour `sinceBeat` ms after a beat: full brightness at the beat, then back to
// `value` over FADE_MILLIS, in a straight line; from FADE_MILLIS on, `value` itself.
static uint beatFade(uint value, unsigned long sinceBeat) {
    if (sinceBeat >= FADE_MILLIS) {
        return value;
    }
    return (uint) (((unsigned long) JAB_MAX_BRIGHTNESS * (FADE_MILLIS - sinceBeat) + (unsigned long) value * sinceBeat)
                   / FADE_MILLIS);
}

#endif //LED_MARQUEE_MARQUEECOLOUR_H
