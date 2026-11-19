// Host unit test for BrightnessShift.h (the number of the 'B' utility message).
// No Arduino, no framework: plain asserts, compiled with the Mac's clang++.
//
//   clang++ -std=c++11 -Wall -Wextra -o /tmp/brightness_shift_test test/brightness_shift_test.cpp \
//       && /tmp/brightness_shift_test
//
// It prints "brightness_shift_test: all N tests passed" and exits 0, or stops at the first failed assert.

#undef NDEBUG

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "../BrightnessShift.h"

static int tests = 0;

// What the sketch did before: atoi() into the processor's 16-bit int, stored in its 8-bit `uint`.
// Here only to show which texts this test is about: the ones where the two differ.
static int before(const char* text) {
    return (uint8_t) (int16_t) strtoll(text, nullptr, 10);
}

static void testTheTexts() {
    struct Case {
        const char* text;
        int shift;
    };
    const Case cases[] = {
            {"0", 0}, {"1", 1}, {"7", 7}, {"8", 8},
            // more than 8 is 8, dark, whatever it is more by
            {"9", 8}, {"10", 8}, {"80", 8}, {"89", 8}, {"90", 8}, {"200", 8}, {"255", 8}, {"256", 8}, {"264", 8},
            {"32767", 8}, {"32768", 8}, {"32776", 8}, {"65535", 8}, {"65536", 8}, {"65544", 8}, {"4294967304", 8},
            {"99999999999999999999999999999999999999", 8}, {"0000000000000000000008", 8}, {"00000000000000009", 8},
            {"000", 0}, {"0000000000000000000003", 3},
            // below 0 is 0
            {"-1", 0}, {"-8", 0}, {"-32768", 0}, {"-32769", 0}, {"-4294967304", 0}, {"-0", 0},
            // read as atoi() reads: blanks and a sign before, anything else ends it, no digits is 0
            {"8x", 8}, {" 8", 8}, {"\t\n 8", 8}, {"+8", 8}, {"+3", 3}, {"3 4", 3}, {"3.9", 3}, {"x", 0}, {"x8", 0},
            {"", 0}, {"+", 0}, {"-", 0}, {"+-8", 0}, {"8-", 8}, {" ", 0},
    };
    for (const Case& c : cases) {
        assert(brightnessShiftFromText(c.text, BRIGHTNESS_SHIFT_MAX) == c.shift);
    }
    // the texts that went wrong before: stored as they came, the low byte of a 16-bit number
    assert(before("200") == 200 && before("256") == 0 && before("65536") == 0 && before("32768") == 0);
    assert(before("-1") == 255 && before("8") == 8 && before("264") == 8);
    tests++;
}

static void testEveryNumber() {
    // every number from 0 to 70000, and around the places where 16 and 32 bits wrap
    char text[32];
    for (long v = 0; v <= 70000; v++) {
        snprintf(text, sizeof text, "%ld", v);
        assert(brightnessShiftFromText(text, 8) == (v > 8 ? 8 : v));
        snprintf(text, sizeof text, "-%ld", v);
        assert(brightnessShiftFromText(text, 8) == 0);
    }
    for (long long v = 4294967296LL - 20; v <= 4294967296LL + 300; v++) {
        snprintf(text, sizeof text, "%lld", v);
        assert(brightnessShiftFromText(text, 8) == 8);
    }
    tests++;
}

static void testOtherMaxima() {
    // nothing depends on the maximum being 8 (the count is kept in 16 bits: with a maximum of 25 it
    // reaches 259, with 255 it reaches 2559)
    assert(brightnessShiftFromText("25", 25) == 25 && brightnessShiftFromText("26", 25) == 25 &&
           brightnessShiftFromText("259", 25) == 25);
    assert(brightnessShiftFromText("255", 255) == 255 && brightnessShiftFromText("256", 255) == 255 &&
           brightnessShiftFromText("2559", 255) == 255 && brightnessShiftFromText("99999", 255) == 255 &&
           brightnessShiftFromText("254", 255) == 254);
    assert(brightnessShiftFromText("5", 0) == 0 && brightnessShiftFromText("0", 0) == 0);
    tests++;
}

int main() {
    testTheTexts();
    testEveryNumber();
    testOtherMaxima();
    printf("brightness_shift_test: all %d tests passed\n", tests);
    return 0;
}
