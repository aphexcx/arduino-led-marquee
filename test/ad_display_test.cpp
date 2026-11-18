// Host test for AdDisplay.h: the real code of the two ad styles ('O' letters one by one, 'C' chonky
// with the colour wave) running against a fake sign and a fake Timer1. No Arduino, no framework.
//
//   clang++ -std=c++11 -Wall -Wextra -o /tmp/ad_display_test test/ad_display_test.cpp \
//       && /tmp/ad_display_test
//
// Add --show to also print a few frames as ASCII art.
//
// What is real: AdDisplay.h, TextWindow.h, the fonts and the gamma table.
// What is fake: the LED strip (it records every column of every frame), and time: a microsecond
// counter that the fakes advance (37 us per LED column, 501 us per latch, 2 us per look at the
// timer), from which the 16-bit timer value is derived, wrapping every 262 ms as Timer1 does.
// What the pictures are compared with is a model written here from the font tables, not the
// column arithmetic under test.
// Not covered here: AVR integer widths (int is 32 bits on the host), real pixel timing, the colour
// quirk of sendBitx8. emulator/tests/ads_test.py runs the real firmware image for those.
//
// The supply's settings are off, as in the sketch. To run everything with them on, add for example
//   -DLED_CURRENT_LIMIT_MA=3000 -DAD_RAMP_MS=100     or     -DLED_CURRENT_LIMIT_SIDE_MA=1200
// (test/run.sh does): every frame's load is then added up and held to the limits, and the colours
// are compared with a dimming worked out here the slow way.

#undef NDEBUG

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------
// The parts of the sketch that AdDisplay.h uses
// ---------------------------------------------------------------------------------------------
#define uint uint8_t // the sketch's uint is 8 bits (the Mac's own `uint` is 32)

#define PROGMEM
#define pgm_read_byte(address) (*(address))
#define gamma ledGammaTable // math.h already has a gamma()

#define COLUMNS_PER_PANEL 60
#define INTERCHAR_SPACE 1

static uint BRIGHTNESS_SHIFT = 0; // the sketch's brightness shift ('B' utility message): none here

#include "../Color.h"
#include "../Gamma.h"
#include "../Fonts.h"

// The sketch's settings (keep in step with the top of LED_Marquee.ino)
#define CHARS_ONEBYONE_DELAY 50
#define AD_SCROLL_HOLD_MS 600
#define AD_SCROLL_STEP_MS 35
#define AD_WAVE_FRAME_MS 10
#ifndef LED_CURRENT_LIMIT_MA
#define LED_CURRENT_LIMIT_MA 0
#endif
#ifndef LED_CURRENT_LIMIT_SIDE_MA
#define LED_CURRENT_LIMIT_SIDE_MA 0
#endif
#ifndef AD_RAMP_MS
#define AD_RAMP_MS 0
#endif

// The font tables are what they should be: 96 characters each, the standard heart 2 columns wider
static_assert(sizeof(FontStd5x7) == 95 * 5 + 7, "standard font: 0x20 to 0x7E and the heart");
static_assert(sizeof(FontChonk) == 96 * 8, "chonky font: 0x20 to 0x7E and the heart");

// Every read of font data is checked: nothing may be read beside the tables
static uint8_t readFont(const uint8_t* address) {
    bool inStd = address >= FontStd5x7 && address < FontStd5x7 + sizeof(FontStd5x7);
    bool inChonk = address >= FontChonk && address < FontChonk + sizeof(FontChonk);
    assert(inStd || inChonk);
    return *address;
}

#define pgm_read_byte_near(address) readFont(address)

// ---------------------------------------------------------------------------------------------
// Fake time and fake Timer1
// ---------------------------------------------------------------------------------------------
static uint64_t nowUs = 0;
static uint64_t timerReads = 0;
static uint64_t lastTimerReadUs = 0;
static uint64_t longestBetweenReadsUs = 0;

static uint16_t timerCount() {
    nowUs += 2; // a loop that waits on the timer makes time pass
    if (nowUs - lastTimerReadUs > longestBetweenReadsUs) longestBetweenReadsUs = nowUs - lastTimerReadUs;
    lastTimerReadUs = nowUs;
    timerReads++;
    return (uint16_t) (nowUs / 4); // 250 counts a millisecond, round every 262.144 ms
}

#define AD_TIMER_COUNT() timerCount()
#define AD_NO_TIMER_SETUP

static double ms(uint64_t us) { return us / 1000.0; }

// What the emulator measures for the ads' frames: 4.4 ms for the 120 columns
static const uint64_t COLUMN_US = 37;
static const uint64_t FRAME_US = 120 * COLUMN_US;

// ---------------------------------------------------------------------------------------------
// Fake sign
// ---------------------------------------------------------------------------------------------
struct Led {
    uint8_t row, r, g, b;
    int carry; // what AdDisplay.h asked the carry flag to be before this column: 1, 0, or -1 (not said)

    bool dark() const { return row == 0; }
};

struct Frame {
    uint64_t sentUs;        // when its last column went out
    bool afterClear;        // a clear() came between the previous frame and this one
    std::vector<Led> cols;  // every column sent for it
};

static bool interruptsOff = false;
static size_t colsThisWindow = 0;
static size_t longestWindowCols = 0;
static std::vector<Led> sending;
static std::vector<Frame> frames;
static bool clearedSinceLastFrame = false;
static int clears = 0;
static int nextCarry = -1;

static void cli() {
    assert(!interruptsOff);
    interruptsOff = true;
    colsThisWindow = 0;
}

static void sei() {
    assert(interruptsOff);
    interruptsOff = false;
    if (colsThisWindow > longestWindowCols) longestWindowCols = colsThisWindow;
}

#define AD_GREEN_STARTS_WITH_CARRY(set) (nextCarry = (set) ? 1 : 0)

static void sendColumnRGB(uint row, uint r, uint g, uint b) {
    assert(interruptsOff); // pixel timing needs interrupts off
    nowUs += COLUMN_US;
    colsThisWindow++;
    sending.push_back(Led{row, r, g, b, nextCarry});
    nextCarry = -1;
}

static void show() {
    assert(!interruptsOff);
    frames.push_back(Frame{nowUs, clearedSinceLastFrame, sending});
    nowUs += 501;
    sending.clear();
    clearedSinceLastFrame = false;
}

static void clear() {
    assert(!interruptsOff && sending.empty());
    nowUs += FRAME_US + 501;
    clearedSinceLastFrame = true;
    clears++;
}

// ---------------------------------------------------------------------------------------------
// The code under test
// ---------------------------------------------------------------------------------------------
#include "../AdDisplay.h"

// ---------------------------------------------------------------------------------------------
// The model: a text as one row byte per column, straight from the font tables
// ---------------------------------------------------------------------------------------------
struct ModelFont {
    const uint8_t* data;
    int cols;      // bytes per character in the table, and lit columns of an ordinary character
    int heartCols; // lit columns of the heart
};

static const ModelFont MODEL_STD = {FontStd5x7, 5, 7};
static const ModelFont MODEL_CHONK = {FontChonk, 8, 8};

// The columns of the first `shown` characters: each character's columns, then one blank column.
static std::vector<uint8_t> modelColumns(const ModelFont& f, const std::string& text, size_t shown = std::string::npos) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < text.size() && i < shown; i++) {
        uint8_t ch = (uint8_t) text[i];
        if (ch == 0x0B) {
            // vertical tab: nothing but the blank column
        } else if (ch < 0x20 || ch > 0x7F) {
            for (int c = 0; c < f.cols; c++) out.push_back(0); // not in the font: a space
        } else {
            int n = ch == 0x7F ? f.heartCols : f.cols;
            for (int c = 0; c < n; c++) out.push_back(f.data[(ch - 0x20) * f.cols + c]);
        }
        out.push_back(0);
    }
    return out;
}

// What one side shows: `pad` blank columns, then the text from column `offset` on.
static std::vector<uint8_t> modelSide(const std::vector<uint8_t>& text, size_t offset, size_t pad) {
    std::vector<uint8_t> side(60, 0);
    for (size_t c = pad; c < 60; c++) {
        size_t at = offset + c - pad;
        if (at < text.size()) side[c] = text[at];
    }
    return side;
}

// Number of columns up to the last lit one
static size_t modelInk(const std::vector<uint8_t>& text) {
    size_t ink = 0;
    for (size_t c = 0; c < text.size(); c++) {
        if (text[c]) ink = c + 1;
    }
    return ink;
}

static std::vector<uint8_t> rowsOfSide(const Frame& f, int side) {
    std::vector<uint8_t> out;
    for (size_t c = 0; c < 60; c++) out.push_back(f.cols[side * 60 + c].row);
    return out;
}

static void printSide(const std::vector<uint8_t>& side) {
    for (int bit = 7; bit >= 1; bit--) {
        printf("    ");
        for (size_t c = 0; c < side.size(); c++) putchar(side[c] & (1 << bit) ? '#' : '.');
        putchar('\n');
    }
}

// ---------------------------------------------------------------------------------------------
// The model of the supply's settings: written again here, the slow way, not taken from LedBudget.h
// ---------------------------------------------------------------------------------------------
static int modelDim(int v, int scale) { return v * (scale + 1) / 256; }

static int modelCarried(int v) { return v | (v >> 1); }

static int modelBits(uint8_t v) {
    int n = 0;
    for (int bit = 0; bit < 8; bit++) n += (v >> bit) & 1;
    return n;
}

// What a frame asks of the supply on one side: every lit LED's three values as they go out (red
// and blue carried; green carried when the code asked for the carry before that column)
static long sideLoad(const Frame& f, int side) {
    long load = 0;
    for (size_t c = 0; c < 60; c++) {
        const Led& led = f.cols[side * 60 + c];
        if (!led.row) continue;
        int green = led.carry == 1 ? modelCarried(led.g) : led.g;
        load += (long) modelBits(led.row) * (modelCarried(led.r) + green + modelCarried(led.b));
    }
    return load;
}

static const long LIMIT_BOTH = (long) LED_CURRENT_LIMIT_MA * 255 / 20;
static const long LIMIT_SIDE = (long) LED_CURRENT_LIMIT_SIDE_MA * 255 / 20;
static const int RAMP_FRAMES = AD_RAMP_MS / AD_WAVE_FRAME_MS;
static long dimmedRuns = 0, undimmedRuns = 0;

// How far a text in this colour must be dimmed (255 = not at all): the most LEDs any 60 columns
// of it light, what each may send under the limits, and the brightest dimming that sends no more.
// wave: a chonky text, whose red can be any value and whose green goes out carried.
static int modelScale(const std::vector<uint8_t>& cols, int r, int g, int b, bool wave) {
    long most = 0;
    for (size_t from = 0; from < cols.size(); from++) {
        long lit = 0;
        for (size_t c = from; c < cols.size() && c < from + 60; c++) lit += modelBits(cols[c]);
        if (lit > most) most = lit;
    }
    long allowed = 765;
    if (most > 0 && LIMIT_BOTH > 0 && LIMIT_BOTH / (2 * most) < allowed) allowed = LIMIT_BOTH / (2 * most);
    if (most > 0 && LIMIT_SIDE > 0 && LIMIT_SIDE / most < allowed) allowed = LIMIT_SIDE / most;
    int best = 0;
    for (int scale = 0; scale <= 255; scale++) {
        int red = modelCarried(modelDim(r, scale));
        if (wave) {
            red = 0;
            for (int v = 0; v <= 255; v++) {
                if (modelCarried(modelDim(v, scale)) > red) red = modelCarried(modelDim(v, scale));
            }
        }
        int green = wave ? modelCarried(modelDim(g, scale)) : modelDim(g, scale);
        if (red + green + modelCarried(modelDim(b, scale)) > allowed) {
            // A chonky text with a ramp passes through all the dimmings below its own: it has to
            // stay below the first that is too bright. Any other text is only shown at its own
            // dimming: the brightest that fits is the one.
            if (wave && RAMP_FRAMES > 1) break;
            continue;
        }
        best = scale;
    }
    (best < 255 ? dimmedRuns : undimmedRuns)++;
    return best;
}

// The dimming of a frame in part number `frame` of the ramp over which a chonky text comes on
static int modelRamp(int scale, size_t frame) {
    if (RAMP_FRAMES < 2 || frame + 1 >= (size_t) RAMP_FRAMES) return scale;
    return scale * (int) (frame + 1) / RAMP_FRAMES;
}

// The ramp is a stretch of time, cut into parts as long as a frame of the wave: the part a frame
// is in comes from when it is drawn, counted from the message's first frame.
static size_t modelRampPart(uint64_t sinceStartUs) { return (size_t) (sinceStartUs / (AD_WAVE_FRAME_MS * 1000)); }

// ---------------------------------------------------------------------------------------------
// Running a message
// ---------------------------------------------------------------------------------------------
struct Run {
    uint64_t startUs, endUs;
    std::vector<Frame> frames;
    int clears;
};

static Run runWith(void (*show)(const char*, unsigned int), const char* text, unsigned int dly) {
    // start anywhere in the timer's round, so that waits cross its wrap at different places
    nowUs += 7919 + (nowUs % 100003);
    frames.clear();
    clears = 0;
    longestWindowCols = 0;
    lastTimerReadUs = nowUs;
    longestBetweenReadsUs = 0;
    Run r;
    r.startUs = nowUs;
    show(text, dly);
    r.endUs = nowUs;
    r.frames = frames;
    r.clears = clears;
    assert(!interruptsOff && sending.empty());
    // Whatever the text and however long the wait, of every message:
    // interrupts are never off for longer than one frame of 120 columns
    assert(longestWindowCols <= 120);
    // the timer was looked at far more often than once per round (262 ms), so no round was lost
    assert(longestBetweenReadsUs < 10000);
    for (size_t i = 0; i < r.frames.size(); i++) {
        const Frame& f = r.frames[i];
        // every frame is exactly 120 columns ...
        assert(f.cols.size() == 120);
        // ... and both sides are the same, LED for LED
        for (size_t c = 0; c < 60; c++) {
            const Led& a = f.cols[c];
            const Led& b = f.cols[60 + c];
            assert(a.row == b.row);
            if (a.row) assert(a.r == b.r && a.g == b.g && a.b == b.b && a.carry == b.carry);
        }
        // ... and no frame asks more of the supply than the limits allow, one side or both
        if (LIMIT_SIDE > 0) assert(sideLoad(f, 0) <= LIMIT_SIDE && sideLoad(f, 1) <= LIMIT_SIDE);
        if (LIMIT_BOTH > 0) assert(sideLoad(f, 0) + sideLoad(f, 1) <= LIMIT_BOTH);
    }
    return r;
}

static const Color ORANGE = {255, 120, 0};
static const Color BLUE = {0, 161, 242};
// The colours the messages are sent in: these two, except in testSupplyRandom
static Color colourO = ORANGE;
static Color colourC = BLUE;

static void showO(const char* text, unsigned int dly) { showCharsOneByOneOnBothPanels(text, colourO, dly); }

static void showC(const char* text, unsigned int dly) { showChonkySlideStyleOnBothPanels(text, dly, colourC, false); }

static void showCNudge(const char* text, unsigned int dly) {
    showChonkySlideStyleOnBothPanels(text, dly, colourC, true);
}

// a and b are the same time to within `tolUs`
static bool near(uint64_t a, uint64_t b, uint64_t tolUs) {
    return (a > b ? a - b : b - a) <= tolUs;
}

static int tests = 0;
static bool showArt = false;

static const unsigned int DELAYS[] = {0, 1, 7, 333, 999, 1000, 1001, 2000, 3000, 4000, 30000, 65535};

// ---------------------------------------------------------------------------------------------
// 'O', letters one by one
// ---------------------------------------------------------------------------------------------

// The frames the description in AdDisplay.h asks for: (letters shown, text column at the left edge)
struct Step {
    size_t shown;
    size_t offset;
    bool scrolled; // this frame came AD_SCROLL_STEP_MS after the one before, not CHARS_ONEBYONE_DELAY
};

static std::vector<Step> modelOneByOne(const std::string& text) {
    std::vector<Step> steps;
    size_t offset = 0;
    bool afterMove = false; // the frame before this one came with a move of the text
    for (size_t p = 0; p < text.size(); p++) {
        size_t end = modelColumns(MODEL_STD, text, p + 1).size() - 1; // columns up to this letter's last one
        size_t target = end > 60 ? end - 60 : 0;
        bool moves = offset < target;
        // the text makes room a column at a time, still without the new letter ...
        while (offset + 1 < target) {
            offset++;
            steps.push_back(Step{p, offset, afterMove});
            afterMove = true;
        }
        // ... and the letter comes with the last column of the move (or at once, if it fits)
        if (offset < target) offset++;
        steps.push_back(Step{p + 1, offset, afterMove});
        afterMove = moves;
    }
    return steps;
}

static void checkOneByOne(const std::string& text, unsigned int dly) {
    Run r = runWith(showO, text.c_str(), dly);
    std::vector<uint8_t> whole = modelColumns(MODEL_STD, text);
    size_t width = whole.empty() ? 0 : whole.size() - 1;
    size_t pad = width < 60 ? (60 - width) / 2 : 0;
    std::vector<Step> steps = modelOneByOne(text);
    // one brightness for the whole message: the finished text must be within the limits
    const int dim = modelScale(whole, GAMMA(255), GAMMA(120), GAMMA(0), false);

    assert(r.clears == 1);
    assert(r.frames.size() == steps.size());
    assert(r.frames.size() >= text.size());
    if (!r.frames.empty()) assert(r.frames[0].afterClear);
    for (size_t i = 0; i < steps.size(); i++) {
        const Frame& f = r.frames[i];
        // the picture: the letters shown so far, at the place the text has moved to
        std::vector<uint8_t> expected = modelSide(modelColumns(MODEL_STD, text, steps[i].shown), steps[i].offset, pad);
        assert(rowsOfSide(f, 0) == expected);
        // its colour: the message's, through the gamma table, on every lit column; green starts
        // without the carry, as this style always sent it
        for (size_t c = 0; c < 120; c++) {
            if (f.cols[c].row) {
                assert(f.cols[c].r == modelDim(GAMMA(255), dim) && f.cols[c].g == modelDim(GAMMA(120), dim) &&
                       f.cols[c].b == modelDim(GAMMA(0), dim));
                assert(f.cols[c].carry == 0);
            }
        }
        // its moment: a letter every 50 ms, a scrolled column every 35 ms
        if (i > 0) {
            uint64_t gap = f.sentUs - r.frames[i - 1].sentUs;
            assert(near(gap, steps[i].scrolled ? AD_SCROLL_STEP_MS * 1000 : CHARS_ONEBYONE_DELAY * 1000, 40));
            // the text never jumps: it stays or moves one column
            assert(steps[i].offset - steps[i - 1].offset <= 1);
            assert(steps[i].shown - steps[i - 1].shown <= 1);
        }
        // a letter that has just appeared is whole and, once the text is moving, at the right edge
        if (i == 0 || steps[i].shown > steps[i - 1].shown) {
            size_t end = modelColumns(MODEL_STD, text, steps[i].shown).size() - 1;
            assert(end <= steps[i].offset + 60 - pad);
            if (steps[i].offset > 0) assert(end == steps[i].offset + 60);
        }
    }
    if (!steps.empty()) {
        assert(steps.back().shown == text.size());
        if (width <= 60) assert(steps.back().offset == 0);
        else assert(steps.back().offset == width - 60); // the end of the text is at the right edge
    }
    // the hold: from the picture with the last letter to the return, dly of real time
    // (with dly 0 it returns as soon as that picture is latched, which takes show()'s 0.5 ms)
    uint64_t complete = r.frames.empty() ? r.startUs + FRAME_US + 501 : r.frames.back().sentUs;
    uint64_t hold = r.frames.empty() || dly ? (uint64_t) dly * 1000 : 501;
    if (!near(r.endUs - complete, hold, 40)) {
        printf("'O' \"%s\" dly %u: held %.3f ms\n", text.c_str(), dly, ms(r.endUs - complete));
    }
    assert(near(r.endUs - complete, hold, 40));
}

static void testOneByOneFits() {
    const char* texts[] = {"HELLO", "TEN CHARS!", "ABCDEFGH", "A", "LOVE YOU\x7F", "NEW\x0BMSG\x0B", "\x7F\x7F", "",
                           "\x0B", "odd\x01\xFE!", "1234567890"};
    for (const char* text : texts) {
        for (unsigned int dly : DELAYS) checkOneByOne(text, dly);
    }
    // by hand: "HELLO" is 29 columns, 15 blank columns before it; one frame per letter
    Run r = runWith(showO, "HELLO", 1000);
    assert(r.frames.size() == 5);
    std::vector<uint8_t> h = rowsOfSide(r.frames[0], 0);
    assert(h[14] == 0 && h[15] == 0xfe && h[19] == 0xfe && h[20] == 0); // H, alone
    // "LOVE YOU" and a heart: 55 columns, 2 before and 3 after, the heart whole on both sides
    r = runWith(showO, "LOVE YOU\x7F", 0);
    std::vector<uint8_t> last = rowsOfSide(r.frames.back(), 1);
    assert(last[1] == 0 && last[2] != 0 && last[49] == 0 && last[50] == 0x70 && last[53] == 0x3e && last[56] == 0x70 &&
           last[57] == 0);
    if (showArt) {
        printf("'O' \"LOVE YOU\" and a heart, the finished text (one side):\n");
        printSide(last);
    }
    tests++;
}

static void testOneByOneLong() {
    std::string forty = "THE QUICK BROWN FOX JUMPS OVER LAZY DOGS";
    assert(forty.size() == 40);
    std::string three;
    for (int i = 0; i < 300; i++) three += (i % 37 == 36) ? '\x7F' : (i % 11 == 10) ? ' ' : (char) ('A' + i % 26);
    std::string longest;
    for (int i = 0; i < 570; i++) longest += (char) ('0' + i % 10);
    const std::string texts[] = {"HELLO WORLD", forty, "I \x7F TRANCE AND ENERGY", "TEN CHARS \x7F", three, longest,
                                 "TAB\x0B\x0B\x0B HERE AND\x0BTHERE\x0B", "odd \x01\x02\xFE\xFF bytes are spaces",
                                 "\x7F\x7F\x7F\x7F\x7F\x7F\x7F\x7F\x7F"};
    for (const std::string& text : texts) {
        checkOneByOne(text, 1000);
        checkOneByOne(text, 999);
    }
    checkOneByOne(forty, 0);
    checkOneByOne(forty, 30000);

    // by hand: 11 letters are 65 columns. Ten fit (59 columns); for the 11th the text moves 5 columns,
    // and the D arrives with the fifth
    Run r = runWith(showO, "HELLO WORLD", 2000);
    assert(r.frames.size() == 10 + 5);
    std::vector<uint8_t> all = modelColumns(MODEL_STD, "HELLO WORLD");
    assert(rowsOfSide(r.frames[9], 0) == modelSide(modelColumns(MODEL_STD, "HELLO WORL"), 0, 0));
    assert(rowsOfSide(r.frames[13], 0) == modelSide(modelColumns(MODEL_STD, "HELLO WORL"), 4, 0));
    assert(rowsOfSide(r.frames[14], 0) == modelSide(all, 5, 0));
    assert(rowsOfSide(r.frames[14], 0)[59] == all[64] && all[64] != 0); // the D's last column is the panel's last
    // 9 letters at 50 ms, then 5 columns at 35 ms, then 2 s
    assert(near(r.frames[9].sentUs - r.frames[0].sentUs, 9 * 50000, 100));
    assert(near(r.frames[14].sentUs - r.frames[9].sentUs, 50000 + 4 * 35000, 100));
    assert(near(r.endUs - r.frames[14].sentUs, 2000000, 40));
    if (showArt) {
        printf("'O' \"HELLO WORLD\", the finished text (one side):\n");
        printSide(rowsOfSide(r.frames[14], 0));
    }
    tests++;
}

// ---------------------------------------------------------------------------------------------
// 'C', chonky with the colour wave
// ---------------------------------------------------------------------------------------------

// Checks frame number `k` of a run: the picture at `offset`, and the wave: red is the low byte of
// dly, less 10 per frame, plus 10 per column; green and blue are the message's; green starts with
// the carry, as this style always sent it.
// `dim` is how far this frame is dimmed (255 = not at all).
static bool waveFrameIs(const Frame& f, const std::vector<uint8_t>& text, size_t offset, unsigned int dly, size_t k,
                        int dim) {
    if (rowsOfSide(f, 0) != modelSide(text, offset, 0)) return false;
    for (size_t c = 0; c < 120; c++) {
        const Led& led = f.cols[c];
        if (!led.row) continue;
        if (led.r != modelDim((uint8_t) (dly - 10 * k + 10 * (c % 60)), dim)) return false;
        if (led.g != modelDim(161, dim) || led.b != modelDim(242, dim) || led.carry != 1) return false;
    }
    return true;
}

static void checkWaveFrame(const Frame& f, const std::vector<uint8_t>& text, size_t offset, unsigned int dly, size_t k,
                           int dim) {
    assert(waveFrameIs(f, text, offset, dly, k, dim));
}

static void checkChonky(const std::string& text, unsigned int dly) {
    Run r = runWith(showC, text.c_str(), dly);
    std::vector<uint8_t> cols = modelColumns(MODEL_CHONK, text);
    size_t ink = modelInk(cols);
    size_t last = ink > 60 ? ink - 60 : 0; // where the text ends up: its last lit column in the panel's last
    // one brightness for the whole message, reached over the ramp
    const int dim = modelScale(cols, 0, 161, 242, true);

    assert(r.clears == 0);
    assert(!r.frames.empty());
    size_t offset = 0;
    size_t firstAtEnd = 0;
    uint64_t movedUs = 0;
    for (size_t k = 0; k < r.frames.size(); k++) {
        const Frame& f = r.frames[k];
        // where is the text in this frame? It may have moved one column, never more, never back
        if (offset < last && rowsOfSide(f, 0) == modelSide(cols, offset + 1, 0) &&
            !(rowsOfSide(f, 0) == modelSide(cols, offset, 0))) {
            offset++;
            if (offset == 1) {
                // the start stood still for AD_SCROLL_HOLD_MS
                assert(near(f.sentUs - r.frames[0].sentUs, AD_SCROLL_HOLD_MS * 1000, 60));
            } else {
                // a column every AD_SCROLL_STEP_MS
                assert(near(f.sentUs - movedUs, AD_SCROLL_STEP_MS * 1000, 60));
            }
            movedUs = f.sentUs;
            if (offset == last) firstAtEnd = k;
        }
        checkWaveFrame(f, cols, offset, dly, k, modelRamp(dim, k));
        // the wave never stops: a frame every 10 ms, or every 8.75 ms while the text moves
        if (k > 0) {
            uint64_t gap = f.sentUs - r.frames[k - 1].sentUs;
            assert(gap <= AD_WAVE_FRAME_MS * 1000 + 60);
            assert(gap >= 8700);
        }
    }
    assert(offset == last); // it got to its end
    // the hold: from the first frame that shows the end of the text to the return, dly of real time
    // (with dly 0 that frame is the last and it returns as soon as it is latched: show()'s 0.5 ms)
    uint64_t hold = dly ? (uint64_t) dly * 1000 : 501;
    if (!near(r.endUs - r.frames[firstAtEnd].sentUs, hold, 40)) {
        printf("'C' \"%s\" dly %u: held %.3f ms\n", text.c_str(), dly, ms(r.endUs - r.frames[firstAtEnd].sentUs));
    }
    assert(near(r.endUs - r.frames[firstAtEnd].sentUs, hold, 40));
    if (dly == 0) assert(firstAtEnd == r.frames.size() - 1);
    if (last == 0) {
        // a text that fits: a frame every 10 ms for as long as it stays (1000 ms: 100 frames, as before)
        size_t expected = dly < 6 ? 1 : (dly - 6) / 10 + 1;
        assert(r.frames.size() == expected || r.frames.size() == expected + 1);
        // 1000 ms: the first frame is out FRAME_US after its start and the hold ends 1000 ms after
        // that; a frame is drawn every AD_WAVE_FRAME_MS as long as 6 ms are left for it. At 10 ms
        // that is frames at 0 to 990 ms: 100, as it always was. (At 12 ms: 0 to 996, 84.)
        if (dly == 1000) assert(r.frames.size() == (1000 * 1000 + FRAME_US - 6000) / (AD_WAVE_FRAME_MS * 1000) + 1);
    }
}

static void testChonkyFits() {
    const char* texts[] = {"CHONK", " ABC", " A..", "1234!!", "SAYS...", "I\x7FU", "", " ", "\x0B", "A\013B",
                           "o\x01\xFE!", "......."};
    for (const char* text : texts) {
        for (unsigned int dly : DELAYS) checkChonky(text, dly);
    }
    // dly 0 or missing: one frame, no hold
    Run r = runWith(showC, "CHONK", 0);
    assert(r.frames.size() == 1);
    // what counts as fitting is the lit columns: 7 characters whose last one is narrow still fit ...
    assert(adInkCols(fontChonk, fontMetrics(fontChonk), "SAYS...") == 56);
    assert(adInkCols(fontChonk, fontMetrics(fontChonk), "1234!!") == 52);
    assert(adInkCols(fontChonk, fontMetrics(fontChonk), "") == 0);
    assert(adInkCols(fontChonk, fontMetrics(fontChonk), "   ") == 0);
    assert(adInkCols(fontChonk, fontMetrics(fontChonk), "A  ") == 7);
    assert(adInkCols(fontChonk, fontMetrics(fontChonk), "\x7F") == 7);
    assert(adInkCols(fontStd, fontMetrics(fontStd), "\x7F") == 7);
    assert(adInkCols(fontStd, fontMetrics(fontStd), "A\x0B!") == 5 + 1 + 1 + 3);
    // ... and 7 full-width ones are one column too wide
    assert(adInkCols(fontChonk, fontMetrics(fontChonk), "ENERGY!") == 61);

    // the chonky heart: 7 lit columns and the blank one, 7 rows, bit 0 never set
    std::vector<uint8_t> heart = modelColumns(MODEL_CHONK, "\x7F");
    const uint8_t want[] = {0x70, 0xf8, 0xfc, 0x7e, 0xfc, 0xf8, 0x70, 0x00, 0x00};
    assert(heart == std::vector<uint8_t>(want, want + 9));
    r = runWith(showC, "I\x7FU", 1000);
    assert(rowsOfSide(r.frames[0], 1) == modelSide(modelColumns(MODEL_CHONK, "I\x7FU"), 0, 0));
    if (showArt) {
        printf("'C' \"I\", a heart, \"U\" (one side):\n");
        printSide(rowsOfSide(r.frames[0], 0));
    }
    tests++;
}

static void testChonkyLong() {
    std::string forty = "THE QUICK BROWN FOX JUMPS OVER LAZY DOGS";
    std::string three;
    for (int i = 0; i < 300; i++) three += (i % 37 == 36) ? '\x7F' : (i % 11 == 10) ? ' ' : (char) ('A' + i % 26);
    const std::string texts[] = {"ENERGY!", "DREAMSTATE", "I \x7F DREAMSTATE", forty, three,
                                 "TABS\x0B\x0B\x0B" "AND odd \x01\xFE bytes", "TRAILING SPACES    "};
    for (const std::string& text : texts) {
        checkChonky(text, 1000);
        checkChonky(text, 999);
    }
    checkChonky("ENERGY!", 0);
    checkChonky(forty, 0);
    checkChonky("DREAMSTATE", 30000);

    // by hand: "ENERGY!" is 61 lit columns. 600 ms at the start, one column, then dly.
    Run r = runWith(showC, "ENERGY!", 2000);
    std::vector<uint8_t> cols = modelColumns(MODEL_CHONK, "ENERGY!");
    assert(rowsOfSide(r.frames[0], 0) == modelSide(cols, 0, 0));
    assert(rowsOfSide(r.frames[59], 0) == modelSide(cols, 0, 0));
    assert(rowsOfSide(r.frames[60], 0) == modelSide(cols, 1, 0));
    assert(rowsOfSide(r.frames[60], 0)[59] == 0xc0); // the last column of the "!"
    assert(near(r.frames[60].sentUs - r.frames[0].sentUs, 600000, 60));
    assert(near(r.endUs - r.frames[60].sentUs, 2000000, 40));
    assert(near(r.endUs - r.startUs, 600000 + 2000000 + FRAME_US, 100));

    // 40 characters: 358 columns to the last lit one (the S is 7 wide), so the text moves 298 columns:
    // the first after 600 ms, the other 297 every 35 ms
    r = runWith(showC, forty.c_str(), 1000);
    assert(modelInk(modelColumns(MODEL_CHONK, forty)) == 358);
    assert(near(r.endUs - r.startUs, FRAME_US + 600000 + 297 * 35000 + 1000000, 300));
    if (showArt) {
        printf("'C' 40 characters, the end of the text (one side):\n");
        printSide(rowsOfSide(r.frames.back(), 0));
    }
    tests++;
}

// "scroll": true. The text moves left by its first character, a column at a time, each column for dly.
static void checkNudge(const std::string& text, unsigned int dly) {
    std::vector<uint8_t> cols = modelColumns(MODEL_CHONK, text);
    // the first character's lit columns and its blank one: 9 positions; a vertical tab is only
    // its blank column, and so is an empty text: 1 position
    size_t positions = text.empty() || text[0] == '\x0B' ? 1 : 9;
    Run r = runWith(showCNudge, text.c_str(), dly);
    const int dim = modelScale(cols, 0, 161, 242, true);
    size_t k = 0;
    for (size_t skip = 0; skip < positions; skip++) {
        assert(k < r.frames.size());
        uint64_t first = r.frames[k].sentUs;
        size_t n = 0;
        // this position's frames: up to the first frame that shows the next position (the last
        // position has no next one: every frame that is left must be its own)
        while (k < r.frames.size() &&
               (n == 0 || skip + 1 == positions || rowsOfSide(r.frames[k], 0) != modelSide(cols, skip + 1, 0))) {
            // The wave starts again at every position. The text comes on once per message, over
            // the ramp's TIME: the positions of a nudge neither start it again nor, when their
            // frames come faster than the wave's (a dly of 0: one every 5 ms), hurry it. The frame
            // is drawn FRAME_US before it is out; a frame right on the edge of two parts of the
            // ramp may be in either.
            const uint64_t drawn = r.frames[k].sentUs - FRAME_US - r.startUs;
            assert(waveFrameIs(r.frames[k], cols, skip, dly, n, modelRamp(dim, modelRampPart(drawn > 100 ? drawn - 100 : 0))) ||
                   waveFrameIs(r.frames[k], cols, skip, dly, n, modelRamp(dim, modelRampPart(drawn + 100))));
            k++;
            n++;
        }
        // each picture stays for dly: until the next one appears, or until the return
        uint64_t until = k < r.frames.size() ? r.frames[k].sentUs : r.endUs;
        if (dly >= 10) assert(near(until - first, (uint64_t) dly * 1000, 60));
        // (a dly shorter than a frame takes to send: one picture straight after the other)
        if (dly < 5 && skip + 1 < positions) assert(near(until - first, FRAME_US + 501, 60));
        if (dly == 0) assert(n == 1);
    }
    // nothing after the last position: the text never moves into its second character
    assert(k == r.frames.size());
    // it always ends (the old code never did for an odd dly)
    assert(r.endUs - r.startUs <= positions * ((uint64_t) dly * 1000 + 6000));
}

static void testChonkyNudge() {
    // "ABCDEF|" is what the app's streaming sends; then a heart, a narrow character, a byte that is
    // not in the font, a vertical tab first, and nothing at all
    const char* texts[] = {"ABCDEF|", "\x7F" "AB", ".AB", "\x01" "AB", "\x0B" "AB|", "\x0B", ""};
    for (const char* text : texts) {
        for (unsigned int dly : DELAYS) {
            if (dly <= 4000) checkNudge(text, dly);
        }
    }
    // shorter than the nudge (its last two positions are both dark, so checkNudge cannot tell them
    // apart): nothing is read past the end of the string, and it ends
    runWith(showCNudge, "A", 20);
    tests++;
}

// Random texts through both styles: the checks of runWith() and of the two check functions hold
static void testRandom() {
    srand(5);
    const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !.,\x7F\x7F\x0B\x02\xF1";
    for (int round = 0; round < 60; round++) {
        std::string text;
        int n = rand() % 30;
        for (int i = 0; i < n; i++) text += alphabet[rand() % (sizeof(alphabet) - 1)];
        unsigned int dly = (unsigned int) (rand() % 1500);
        checkOneByOne(text, dly);
        checkChonky(text, dly);
    }
    tests++;
}

// Random texts in random colours, with short and long holds so that a ramp is cut off at every
// point: runWith() holds EVERY frame that is sent to the supply's limits, the frames of a ramp and
// every position of a nudge included. (With no limit set this only checks its other rules.)
static void testSupplyRandom() {
    srand(11);
    const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !.,$|\x7F\x0B";
    // the two cases the reviews found: a frame part of the way up a ramp drew more than the last
    colourC = Color{0, 96, 96};
    runWith(showC, "$", 263);
    colourC = Color{0, 192, 192};
    runWith(showC, "|", 2710);
    runWith(showCNudge, "ABCDEF|", 10);
    for (int round = 0; round < 250; round++) {
        std::string text;
        int n = 1 + rand() % 12;
        for (int i = 0; i < n; i++) text += alphabet[rand() % (sizeof(alphabet) - 1)];
        // colours with single bits, runs of ones and values next to powers of two come up often:
        // that is where what is sent jumps
        const int edges[] = {0, 1, 11, 12, 15, 16, 31, 32, 63, 64, 96, 127, 128, 170, 191, 192, 254, 255};
        auto value = [&]() { return (uint8_t) (rand() % 3 ? edges[rand() % 18] : rand() % 256); };
        colourO = Color{value(), value(), value()};
        colourC = Color{0, value(), value()};
        unsigned int dly = (unsigned int) (rand() % 4 ? rand() % 140 : rand() % 900);
        runWith(showO, text.c_str(), dly % 60);
        runWith(showC, text.c_str(), dly);
        if (round % 5 == 0) runWith(showCNudge, text.c_str(), dly % 40);
    }
    colourO = ORANGE;
    colourC = BLUE;
    tests++;
}

int main(int argc, char** argv) {
    showArt = argc > 1 && strcmp(argv[1], "--show") == 0;
    testOneByOneFits();
    testOneByOneLong();
    testChonkyFits();
    testChonkyLong();
    testChonkyNudge();
    testRandom();
    testSupplyRandom();
    // with a limit set, the texts above it were dimmed and those under it were not
    const bool limits = LIMIT_BOTH + LIMIT_SIDE > 0;
    if (limits) assert(dimmedRuns > 100 && undimmedRuns > 100);
    else assert(dimmedRuns == 0);
    printf("ad_display_test: all %d tests passed (%llu looks at the timer, %.0f s of sign time)", tests,
           (unsigned long long) timerReads, ms(nowUs) / 1000);
    if (limits || RAMP_FRAMES > 1) {
        printf("; limit %d mA, a side %d mA, ramp %d ms: %ld messages dimmed, %ld not", LED_CURRENT_LIMIT_MA,
               LED_CURRENT_LIMIT_SIDE_MA, AD_RAMP_MS, dimmedRuns, undimmedRuns);
    }
    printf("\n");
    return 0;
}
