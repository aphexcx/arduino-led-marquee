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
static uint16_t slowSent(uint8_t r, uint8_t g, uint8_t b, uint8_t scale, bool waveRed, bool greenCarried) {
    uint16_t red = ledCarried(ledScaled(r, scale));
    if (waveRed) {
        red = 0;
        for (int v = 0; v <= 255; v++) {
            uint8_t sent = ledCarried(ledScaled((uint8_t) v, scale));
            if (sent > red) red = sent;
        }
    }
    uint8_t green = ledScaled(g, scale);
    return red + (greenCarried ? ledCarried(green) : green) + ledCarried(ledScaled(b, scale));
}

static void checkScale(uint16_t allowed, uint8_t r, uint8_t g, uint8_t b, bool waveRed, bool greenCarried) {
    uint8_t scale = ledBudgetScale(allowed, r, g, b, waveRed, greenCarried);
    // it fits ...
    assert(slowSent(r, g, b, scale, waveRed, greenCarried) <= allowed);
    // ... and nothing brighter does
    for (int s = scale + 1; s <= 255; s++) {
        assert(slowSent(r, g, b, (uint8_t) s, waveRed, greenCarried) > allowed);
    }
}

static void testBudgetScale() {
    // by hand: one-by-one text in full green (0, 255, 0), 153 allowed: green goes out as it is
    assert(ledBudgetScale(153, 0, 255, 0, false, false) == 153);
    // within the limit as it is: not touched
    assert(ledBudgetScale(255, 0, 255, 0, false, false) == 255);
    assert(ledBudgetScale(765, 255, 255, 255, false, false) == 255);
    assert(ledBudgetScale(765, 0, 255, 255, true, true) == 255);
    // nothing allowed: dark
    assert(ledBudgetScale(0, 255, 255, 255, false, false) == 0);
    // the chonky heading that started this (wave, green 255, blue 25): undimmed it can send
    // 255 + 255 + 29 per LED
    assert(slowSent(0, 255, 25, 255, true, true) == 539);
    assert(ledBudgetScale(539, 0, 255, 25, true, true) == 255);
    assert(ledBudgetScale(538, 0, 255, 25, true, true) < 255);
    // pure blue 128 is sent as 192: it needs dimming under a limit its plain value would pass
    assert(ledBudgetScale(128, 0, 0, 128, false, false) < 255);

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
    testRamp();
    printf("led_budget_test: all %d tests passed\n", tests);
    return 0;
}
