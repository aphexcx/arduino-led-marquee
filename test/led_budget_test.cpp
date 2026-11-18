// Host unit test for LedBudget.h (how much a frame asks of the supply, and how far a colour has to
// be dimmed to stay under a limit). No Arduino, no framework: plain asserts.
//
//   clang++ -std=c++11 -Wall -Wextra -o /tmp/led_budget_test test/led_budget_test.cpp \
//       && /tmp/led_budget_test
//
// It prints "led_budget_test: all N tests passed" and exits 0, or stops at the first failed assert.

#undef NDEBUG

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "../LedBudget.h"

static int tests = 0;

static void testCarriedAndScaled() {
    // what sendBitx8 makes of a byte that starts with the carry set
    assert(ledCarried(0) == 0 && ledCarried(255) == 255);
    assert(ledCarried(0x80) == 0xC0); // half goes out as three quarters
    assert(ledCarried(0xAA) == 0xFF);
    assert(ledCarried(0x55) == 0x7F);
    assert(ledCarried(25) == 29 && ledCarried(128) == 192 && ledCarried(161) == 241);
    // not proportional and not even rising: this is why a limit cannot be met by one division
    assert(ledCarried(11) == 15 && ledCarried(12) == 14);
    for (int v = 0; v <= 255; v++) {
        assert(ledCarried((uint8_t) v) >= v);
        assert(ledCarried((uint8_t) v) <= v + v / 2 + 1);
    }

    // dimming: 255 leaves a value alone, 0 makes it 0, and it never makes anything brighter
    for (int v = 0; v <= 255; v++) {
        assert(ledScaled((uint8_t) v, 255) == v);
        assert(ledScaled((uint8_t) v, 0) == 0);
        for (int s = 0; s < 255; s++) {
            assert(ledScaled((uint8_t) v, (uint8_t) s) <= v);
            assert(ledScaled((uint8_t) v, (uint8_t) s) <= ledScaled((uint8_t) v, (uint8_t) (s + 1)));
        }
    }
    assert(ledScaled(255, 127) == 127 && ledScaled(200, 127) == 100 && ledScaled(255, 128) == 128);
    // the dimmed values of all reds are exactly the values 0 to scale (ledBudgetScale counts on it)
    for (int s = 0; s <= 255; s++) {
        bool seen[256] = {false};
        for (int v = 0; v <= 255; v++) seen[ledScaled((uint8_t) v, (uint8_t) s)] = true;
        for (int x = 0; x <= 255; x++) assert(seen[x] == (x <= s));
    }

    // 20 mA per channel at 255
    assert(LED_LOAD_OF_MA(20) == 255);
    assert(LED_LOAD_OF_MA(1000) == 12750);
    assert(LED_LOAD_OF_MA(60000) == 765000);
    tests++;
}

static void testAllowance() {
    // no limit at all, or nothing lit: anything goes
    assert(ledAllowance(250, 0, 0) == 765);
    assert(ledAllowance(0, 1, 1) == 765);
    // 250 LEDs on each side, 6 A for the two sides: 76500 / 500 = 153 per LED
    assert(ledAllowance(250, LED_LOAD_OF_MA(6000), 0) == 153);
    // the same LEDs, 4 A for one side: 51000 / 250 = 204 per LED
    assert(ledAllowance(250, 0, LED_LOAD_OF_MA(4000)) == 204);
    // both: the tighter one
    assert(ledAllowance(250, LED_LOAD_OF_MA(6000), LED_LOAD_OF_MA(4000)) == 153);
    assert(ledAllowance(250, LED_LOAD_OF_MA(6000), LED_LOAD_OF_MA(2000)) == 102);
    // a generous limit is no limit
    assert(ledAllowance(10, LED_LOAD_OF_MA(60000), LED_LOAD_OF_MA(60000)) == 765);
    // every LED of a side lit (420), the smallest limit: nothing may be sent
    assert(ledAllowance(420, LED_LOAD_OF_MA(1), 0) == 0);
    // rounding is always down: lit x allowance never exceeds the limit
    for (uint16_t lit = 1; lit <= 420; lit += 7) {
        for (uint32_t ma = 1; ma <= 60000; ma = ma * 3 + 1) {
            uint32_t limit = LED_LOAD_OF_MA(ma);
            assert((uint32_t) ledAllowance(lit, limit, 0) * 2 * lit <= limit || ledAllowance(lit, limit, 0) == 765);
            assert((uint32_t) ledAllowance(lit, 0, limit) * lit <= limit || ledAllowance(lit, 0, limit) == 765);
        }
    }
    tests++;
}

// What an LED of this colour can send at most when dimmed by `scale`, worked out the slow way:
// for the wave every red value is tried.
// shift: the sign's brightness shift. With one, the carry of every byte is unknown, so all three
// channels are taken as carried (the most they can be).
static int slowShift(int v, int shift) { return shift >= 8 ? 0 : v >> shift; }

static uint16_t slowSent(uint8_t r, uint8_t g, uint8_t b, uint8_t scale, bool waveRed, bool greenCarried,
                         int shift = 0) {
    uint16_t red = ledCarried((uint8_t) slowShift(ledScaled(r, scale), shift));
    if (waveRed) {
        red = 0;
        for (int v = 0; v <= 255; v++) {
            uint8_t sent = ledCarried((uint8_t) slowShift(ledScaled((uint8_t) v, scale), shift));
            if (sent > red) red = sent;
        }
    }
    uint8_t green = (uint8_t) slowShift(ledScaled(g, scale), shift);
    return red + ((greenCarried || shift) ? ledCarried(green) : green) +
           ledCarried((uint8_t) slowShift(ledScaled(b, scale), shift));
}

static void checkScale(uint16_t allowed, uint8_t r, uint8_t g, uint8_t b, bool waveRed, bool greenCarried,
                       int shift = 0) {
    // For a picture that passes through the dimmings below its own (a ramp):
    uint8_t scale = ledBudgetScale(allowed, r, g, b, waveRed, greenCarried, (uint8_t) shift, true);
    // it fits, and so does every dimming below it ...
    for (int s = 0; s <= scale; s++) {
        assert(slowSent(r, g, b, (uint8_t) s, waveRed, greenCarried, shift) <= allowed);
    }
    // ... and the next brighter one does not
    if (scale < 255) assert(slowSent(r, g, b, (uint8_t) (scale + 1), waveRed, greenCarried, shift) > allowed);

    // For a picture that is only ever shown at its own dimming:
    uint8_t alone = ledBudgetScale(allowed, r, g, b, waveRed, greenCarried, (uint8_t) shift, false);
    // it fits, and no brighter dimming does
    assert(slowSent(r, g, b, alone, waveRed, greenCarried, shift) <= allowed);
    for (int s = alone + 1; s <= 255; s++) {
        assert(slowSent(r, g, b, (uint8_t) s, waveRed, greenCarried, shift) > allowed);
    }
    assert(alone >= scale); // never dimmer than the ramped answer
}

static void testBudgetScale() {
    // by hand: one-by-one text in full green (0, 255, 0), 153 allowed: green goes out as it is
    assert(ledBudgetScale(153, 0, 255, 0, false, false, 0, true) == 153);
    // within the limit as it is: not touched
    assert(ledBudgetScale(255, 0, 255, 0, false, false, 0, true) == 255);
    assert(ledBudgetScale(765, 255, 255, 255, false, false, 0, true) == 255);
    assert(ledBudgetScale(765, 0, 255, 255, true, true, 0, true) == 255);
    // nothing allowed: dark
    assert(ledBudgetScale(0, 255, 255, 255, false, false, 0, true) == 0);
    // the chonky heading that started this (wave, green 255, blue 25): undimmed it can send
    // 255 + 255 + 29 per LED
    assert(slowSent(0, 255, 25, 255, true, true) == 539);
    // ... but on the way there (a ramp) it can send a little more: the blue of 25, dimmed, goes
    // out as up to 31. It is left alone only when the most any dimming of it sends is allowed.
    {
        uint16_t most = 0;
        for (int s = 0; s <= 255; s++) {
            uint16_t sent = slowSent(0, 255, 25, (uint8_t) s, true, true);
            if (sent > most) most = sent;
        }
        assert(most > 539 && most <= 541);
        assert(ledBudgetScale(most, 0, 255, 25, true, true, 0, true) == 255);
        assert(ledBudgetScale(most - 1, 0, 255, 25, true, true, 0, true) < 255);
    }
    // pure blue 128 is sent as 192: it needs dimming under a limit its plain value would pass
    assert(ledBudgetScale(128, 0, 0, 128, false, false, 0, true) < 255);
    // What is sent does not fall evenly with the dimming: a red of 12 goes out as 14, but dimmed
    // to 11 as 15. With 14 allowed the full colour would fit, and a ramp up to it would not: the
    // frame at 11 is over. So the answer stays below the first dimming that is too bright.
    assert(slowSent(12, 0, 0, 255, false, false) == 14);
    {
        uint8_t scale = ledBudgetScale(14, 12, 0, 0, false, false, 0, true);
        assert(scale < 255);
        for (int s = 0; s <= scale; s++) assert(slowSent(12, 0, 0, (uint8_t) s, false, false) <= 14);
    }
    // the case the review found: a chonky text with green and blue 192. Every frame of a ramp of
    // any length stays within what the text is allowed
    for (uint16_t allowed = 0; allowed <= 765; allowed += 5) {
        uint8_t scale = ledBudgetScale(allowed, 0, 192, 192, true, true, 0, true);
        for (int frames = 2; frames <= 255; frames += 11) {
            for (int frame = 0; frame < frames; frame++) {
                uint8_t at = ledRampScale(scale, (uint8_t) frame, (uint8_t) frames);
                assert(slowSent(0, 192, 192, at, true, true) <= allowed);
            }
        }
    }

    // A text without a ramp is only ever shown at its own dimming, so the dimmings below it do not
    // count: a red of 12 (sent as 14) with 14 allowed fits as it is and is left alone, although
    // dimmed to 11 it would go out as 15. (The gate's case was a one-by-one text dimmed for
    // brightnesses it never shows.) With a ramp it has to stay below that 11.
    assert(ledBudgetScale(14, 12, 0, 0, false, false, 0, false) == 255);
    assert(ledBudgetScale(14, 12, 0, 0, false, false, 0, true) < 255);

    // against the slow way: every limit that matters, both styles, many colours
    srand(20261118);
    for (int round = 0; round < 400; round++) {
        uint8_t r = (uint8_t) (rand() % 256), g = (uint8_t) (rand() % 256), b = (uint8_t) (rand() % 256);
        if (round % 5 == 0) g = 255;
        if (round % 7 == 0) b = 0;
        for (uint16_t allowed = 0; allowed <= 765; allowed += (allowed < 40 ? 1 : 37)) {
            checkScale(allowed, r, g, b, false, false); // one by one
            checkScale(allowed, 0, g, b, true, true);   // chonky
        }
    }
    // the wave: whatever red comes up in a column, dimmed, it is never sent brighter than counted
    for (int scale = 0; scale <= 255; scale++) {
        uint16_t counted = slowSent(0, 0, 0, (uint8_t) scale, true, true);
        for (int v = 0; v <= 255; v++) assert(ledCarried(ledScaled((uint8_t) v, (uint8_t) scale)) <= counted);
    }
    tests++;
}

// The sign's brightness shift ('B' utility message): every value is shifted right before it goes out.
static void testShift() {
    assert(ledShifted(200, 0) == 200 && ledShifted(200, 1) == 100 && ledShifted(200, 3) == 25);
    // 8 is the most the sketch ever stores, and it is dark
    assert(ledShifted(255, 7) == 1 && ledShifted(255, 8) == 0);

    // A text that the shift already brings under the limit is not dimmed again. Full green, one by
    // one, 153 allowed: without a shift it has to come down to 153 ...
    assert(ledBudgetScale(153, 0, 255, 0, false, false, 0, true) == 153);
    // ... at shift 1 it goes out as 127 and is left alone (it was dimmed to 153 and then halved)
    assert(ledBudgetScale(153, 0, 255, 0, false, false, 1, true) == 255);
    // the chonky heading (wave, green 255, blue 25) at shift 1: at most 127 + 127 + 14 per LED (12 goes out as 14)
    assert(slowSent(0, 255, 25, 255, true, true, 1) == 127 + 127 + 14);
    assert(ledBudgetScale(300, 0, 255, 25, true, true, 1, true) == 255);
    assert(ledBudgetScale(300, 0, 255, 25, true, true, 0, true) < 255);
    // shifted to nothing: nothing is sent, nothing to dim
    assert(ledBudgetScale(0, 255, 255, 255, false, false, 8, true) == 255);

    // against the slow way, every shift
    srand(7);
    for (int shift = 0; shift <= 8; shift++) {
        for (int round = 0; round < 60; round++) {
            uint8_t r = (uint8_t) (rand() % 256), g = (uint8_t) (rand() % 256), b = (uint8_t) (rand() % 256);
            for (uint16_t allowed = 0; allowed <= 765; allowed += (allowed < 40 ? 1 : 41)) {
                checkScale(allowed, r, g, b, false, false, shift);
                checkScale(allowed, 0, g, b, true, true, shift);
            }
        }
    }
    tests++;
}

// A ramp is a stretch of TIME: which part of it a frame is in comes from when the frame is drawn.
static void testRampInTime() {
    // 10 ms parts (2500 ticks), a ramp of 6 parts
    assert(ledRampFrame(0, 2500, 6) == 0 && ledRampFrame(2499, 2500, 6) == 0 && ledRampFrame(2500, 2500, 6) == 1);
    assert(ledRampFrame(14999, 2500, 6) == 5 && ledRampFrame(15000, 2500, 6) == 6);
    assert(ledRampFrame(0xFFFFFFFFUL, 2500, 6) == 6 && ledRampFrame(1000000, 2500, 255) == 255);
    // Frames that come every 5 ms (a nudge with no hold) take the ramp's whole time to come on:
    // 60 ms, not the 30 ms that six frames last. The gate's case.
    int full = -1;
    for (int k = 0; k < 40 && full < 0; k++) {
        if (ledRampScale(200, ledRampFrame((uint32_t) k * 1250, 2500, 6), 6) == 200) full = k;
    }
    assert(full == 10); // the frame at 50 ms is the first in the ramp's last part
    // and frames that come every 20 ms skip parts but are never brighter than their time allows
    for (int k = 0; k < 10; k++) {
        uint8_t part = ledRampFrame((uint32_t) k * 5000, 2500, 6);
        assert(part == (k * 2 >= 6 ? 6 : k * 2));
    }
    tests++;
}

static void testRamp() {
    // no ramp
    assert(ledRampScale(200, 0, 0) == 200 && ledRampScale(200, 0, 1) == 200 && ledRampScale(200, 9, 0) == 200);
    // ten frames: a tenth, two tenths, ... the whole from the tenth frame on
    for (int frame = 0; frame < 10; frame++) assert(ledRampScale(250, (uint8_t) frame, 10) == 25 * (frame + 1));
    assert(ledRampScale(250, 10, 10) == 250 && ledRampScale(250, 255, 10) == 250);
    // any length: it only rises, each step is at most a frame's share (and one for the rounding),
    // and it gets there with the last frame
    for (int frames = 2; frames <= 255; frames++) {
        for (int scale = 0; scale <= 255; scale += 5) {
            int before = 0;
            for (int frame = 0; frame < frames; frame++) {
                int now = ledRampScale((uint8_t) scale, (uint8_t) frame, (uint8_t) frames);
                assert(now >= before && now - before <= scale / frames + 1);
                before = now;
            }
            assert(before == scale);
        }
    }
    tests++;
}

int main() {
    testCarriedAndScaled();
    testAllowance();
    testBudgetScale();
    testShift();
    testRamp();
    testRampInTime();
    printf("led_budget_test: all %d tests passed\n", tests);
    return 0;
}
