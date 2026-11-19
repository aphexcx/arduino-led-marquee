// Host unit test for TextWindow.h (where the columns of a text are, and the clock without an interrupt).
// No Arduino, no framework: plain asserts, compiled with the Mac's clang++.
//
//   clang++ -std=c++11 -Wall -Wextra -o /tmp/text_window_test test/text_window_test.cpp \
//       && /tmp/text_window_test
//
// It prints "text_window_test: all N tests passed" and exits 0, or stops at the first failed assert.

#undef NDEBUG

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../TextWindow.h"

// The sign's two fonts: the standard one (5 columns, heart 7) and the chonky one (8, heart 8)
static const TextMetrics STD = {5, 7};
static const TextMetrics CHONK = {8, 8};

// ---------------------------------------------------------------------------------------------
// An independent, deliberately naive model: one entry per LED column of the whole text.
// ---------------------------------------------------------------------------------------------
struct Cell {
    bool lit;  // true: column `col` of the picture of character `index`; false: a blank column
    int index;
    int col;
};

static bool operator==(const Cell& a, const Cell& b) {
    if (a.lit != b.lit) return false;
    return !a.lit || (a.index == b.index && a.col == b.col);
}

static int modelGlyphCols(const TextMetrics& m, char ch) {
    if (ch == 0x0B) return 0;           // VT: only the blank column after it
    if (ch == 0x7F) return m.heartCols; // the heart
    return m.glyphCols;
}

// The text as columns: every character's lit columns, then one blank column.
static std::vector<Cell> modelStrip(const TextMetrics& m, const std::string& text) {
    std::vector<Cell> strip;
    for (size_t i = 0; i < text.size(); i++) {
        for (int c = 0; c < modelGlyphCols(m, text[i]); c++) strip.push_back({true, (int) i, c});
        strip.push_back({false, 0, 0});
    }
    return strip;
}

static Cell modelAt(const std::vector<Cell>& strip, size_t pos) {
    return pos < strip.size() ? strip[pos] : Cell{false, 0, 0}; // blank for ever after the end
}

static Cell cellOf(const TextCursor& c) {
    return textCursorLit(c) ? Cell{true, (int) c.index, (int) c.col} : Cell{false, 0, 0};
}

static int tests = 0;

static const char* const SAMPLES[] = {
        "", "A", "AB", "HELLO", "HELLO WORLD", "\x7F", "I \x7F U", "\x7F\x7F\x7F", "\x0B", "A\013B",
        "\x0B\x0B", "NEW\x0BMSG\x0B", "LOVE YOU\x7F", " ", "   ", "\x01\xFF\x80 odd bytes \x1F",
};

static void testWidths() {
    // by hand
    assert(textCols(STD, "") == 0);
    assert(textCols(STD, "A") == 5);
    assert(textCols(STD, "AB") == 11);
    assert(textCols(STD, "TEN CHARS!") == 59);          // 10 characters: the widest that fits a panel
    assert(textCols(STD, "HELLO WORLD") == 65);         // 11: too wide
    assert(textCols(STD, "\x7F") == 7);
    assert(textCols(STD, "LOVE YOU\x7F") == 8 * 6 + 7); // a heart is 2 columns wider than a letter
    assert(textCols(STD, "A\x0B" "B") == 5 + 1 + 0 + 1 + 5);
    assert(textCols(STD, "\x0B") == 0);
    assert(textCols(CHONK, "A") == 8);
    assert(textCols(CHONK, "1234!!") == 53);
    assert(textCols(CHONK, "ENERGY!") == 62);
    assert(textCols(CHONK, "\x7F") == 8);
    assert(textCols(CHONK, "I \x7F U") == 5 * 9 - 1);
    assert(textGlyphCols(STD, '\x0B') == 0 && textGlyphCols(CHONK, '\x0B') == 0);
    assert(textGlyphCols(STD, '\x01') == 5 && textGlyphCols(CHONK, '\xFF') == 8); // drawn as a space

    assert(textInFont(' ') && textInFont('~') && textInFont('\x7F'));
    assert(!textInFont('\x1F') && !textInFont('\x0B') && !textInFont('\0') && !textInFont('\x80') &&
           !textInFont('\xFF'));

    // against the model
    const TextMetrics fonts[] = {STD, CHONK};
    for (const TextMetrics& m : fonts) {
        for (const char* s : SAMPLES) {
            size_t model = modelStrip(m, s).size();
            assert(textCols(m, s) == (model ? model - 1 : 0));
        }
    }
    // the longest message the sign can receive, in the widest font, fits the 16-bit counters
    std::string longest(600, '\x7F');
    assert(textCols(CHONK, longest.c_str()) == 600 * 9 - 1);
    assert(textCols(STD, longest.c_str()) == 600 * 8 - 1);
    tests++;
}

static void testCentre() {
    uint8_t start, end;
    // the two examples of the sketch's comment
    textCentre(6, 60, &start, &end);
    assert(start == 27 && end == 27);
    textCentre(5, 60, &start, &end);
    assert(start == 27 && end == 28);
    // 10 standard characters: one spare column, it goes after the text
    textCentre(59, 60, &start, &end);
    assert(start == 0 && end == 1);
    textCentre(60, 60, &start, &end);
    assert(start == 0 && end == 0);
    // wider than the panel (the old code wrapped an 8-bit number here and padded with 125 columns)
    textCentre(65, 60, &start, &end);
    assert(start == 0 && end == 0);
    textCentre(61, 60, &start, &end);
    assert(start == 0 && end == 0);
    textCentre(5399, 60, &start, &end);
    assert(start == 0 && end == 0);
    textCentre(0, 60, &start, &end);
    assert(start == 30 && end == 30);
    // every width: the text and its padding fill the panel exactly, the padding differs by at most one
    for (uint16_t cols = 0; cols <= 60; cols++) {
        textCentre(cols, 60, &start, &end);
        assert(start + cols + end == 60);
        assert(end == start || end == start + 1);
    }
    // what the strings come to, with the real widths
    textCentre(textCols(STD, "HELLO"), 60, &start, &end);
    assert(start == 15 && end == 16);               // 29 columns, as before
    textCentre(textCols(STD, "LOVE YOU\x7F"), 60, &start, &end);
    assert(start == 2 && end == 3);                 // 55 columns (the old count said 53: 3 and 4)
    textCentre(textCols(STD, "NEW\x0BMSG\x0B"), 60, &start, &end);
    assert(textCols(STD, "NEW\x0BMSG\x0B") == 37);  // 6 letters and 2 tabs (the old count said 35)
    assert(start == 11 && end == 12);
    tests++;
}

static void testOffsetToShow() {
    assert(textOffsetToShow(0, 60) == 0);
    assert(textOffsetToShow(59, 60) == 0);
    assert(textOffsetToShow(60, 60) == 0);
    assert(textOffsetToShow(61, 60) == 1);
    assert(textOffsetToShow(65, 60) == 5);
    assert(textOffsetToShow(5399, 60) == 5339);
    tests++;
}

static void checkCursor(const TextMetrics& m, const std::string& text) {
    const char* s = text.c_str();
    std::vector<Cell> strip = modelStrip(m, text);
    size_t beyond = strip.size() + 70; // more than a panel past the end
    // seeking to any column gives that column
    for (size_t pos = 0; pos < beyond; pos++) {
        TextCursor c = textSeek(m, s, (uint16_t) pos);
        assert(cellOf(c) == modelAt(strip, pos));
    }
    // stepping from any column gives the columns after it, one by one
    for (size_t from = 0; from < beyond; from += (from < 20 ? 1 : 7)) {
        TextCursor c = textSeek(m, s, (uint16_t) from);
        for (size_t pos = from; pos < beyond; pos++) {
            assert(cellOf(c) == modelAt(strip, pos));
            assert(c.index <= text.size()); // never past the terminating NUL
            textCursorNext(m, s, c);
        }
    }
}

static void testCursor() {
    const TextMetrics fonts[] = {STD, CHONK};
    for (const TextMetrics& m : fonts) {
        for (const char* s : SAMPLES) checkCursor(m, s);
    }
    // by hand: "A", heart, VT, "B" in the standard font
    //   columns 0-4 A, 5 blank, 6-12 heart, 13 blank, 14 blank (the tab), 15-19 B, 20 blank, then nothing
    const char* s = "A\x7F\x0B" "B";
    TextCursor c = textSeek(STD, s, 0);
    assert(c.index == 0 && c.col == 0 && textCursorLit(c));
    c = textSeek(STD, s, 5);
    assert(!textCursorLit(c));
    c = textSeek(STD, s, 12);
    assert(c.index == 1 && c.col == 6 && textCursorLit(c));
    c = textSeek(STD, s, 14);
    assert(c.index == 2 && !textCursorLit(c));
    c = textSeek(STD, s, 15);
    assert(c.index == 3 && c.col == 0 && textCursorLit(c));
    c = textSeek(STD, s, 20);
    assert(!textCursorLit(c));
    c = textSeek(STD, s, 21);
    assert(c.index == 4 && !textCursorLit(c)); // on the NUL
    c = textSeek(STD, s, 60000);
    assert(c.index == 4 && !textCursorLit(c));
    tests++;
}

static void testCursorRandom() {
    srand(20261118);
    const char alphabet[] = "AB \x7F\x7F\x0B\x0B!.\x05\xF0";
    const TextMetrics fonts[] = {STD, CHONK};
    for (int round = 0; round < 300; round++) {
        std::string text;
        int n = rand() % 40;
        for (int i = 0; i < n; i++) text += alphabet[rand() % (sizeof(alphabet) - 1)];
        checkCursor(fonts[round % 2], text);
    }
    // a text as long as the sign can receive: the index and the position need 16 bits
    std::string longText;
    for (int i = 0; i < 600; i++) longText += (i % 50 == 49) ? '\x7F' : (char) ('A' + i % 26);
    std::vector<Cell> strip = modelStrip(CHONK, longText);
    assert(strip.size() == 5400);
    TextCursor c = textSeek(CHONK, longText.c_str(), 5000);
    for (size_t pos = 5000; pos < 5500; pos++) {
        assert(cellOf(c) == modelAt(strip, pos));
        textCursorNext(CHONK, longText.c_str(), c);
    }
    assert(textSeek(CHONK, longText.c_str(), 5390).index == 598);
    tests++;
}

// ---------------------------------------------------------------------------------------------
// The clock, against a simulated 16-bit counter that goes round every 65536 counts
// ---------------------------------------------------------------------------------------------
static void testTickClock() {
    assert(TICK_CLOCK_PER_MS == 250); // 16 MHz / 64

    // The counter starts anywhere and wraps; the clock adds up what really passed.
    const uint16_t starts[] = {0, 1, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF, 12345};
    for (uint16_t start : starts) {
        uint64_t real = start; // the counter if it were wide enough
        TickClock c;
        tickClockStart(c, (uint16_t) real);
        assert(tickClockRead(c, (uint16_t) real) == 0);
        uint64_t began = real;
        srand(start);
        // 30 seconds and more, read at uneven intervals: sometimes at once, sometimes after a
        // whole frame (3.7 ms = 930 counts), sometimes just short of a full round
        while (real - began < 9000000) {
            int kind = rand() % 10;
            uint32_t step = kind < 4 ? (uint32_t) (rand() % 4)
                          : kind < 8 ? 900 + (uint32_t) (rand() % 200)
                          : kind < 9 ? (uint32_t) (rand() % 65536)
                          : 65535;
            real += step;
            assert(tickClockRead(c, (uint16_t) real) == real - began);
        }
        assert(c.ticks >= 30000UL * TICK_CLOCK_PER_MS);
    }

    // A read exactly on the wrap, and reads that straddle it
    TickClock c;
    tickClockStart(c, 0xFFF0);
    assert(tickClockRead(c, 0xFFFF) == 15);
    assert(tickClockRead(c, 0x0000) == 16);
    assert(tickClockRead(c, 0x0010) == 32);
    tickClockStart(c, 0xFFF0);
    assert(tickClockRead(c, 0x0010) == 32);

    // The longest stretch with interrupts off in the ads is one frame of 120 columns (about 3.7 ms);
    // the old one-by-one code had one of 2 seconds. The counter does not care about interrupts, only
    // about being read once per round: up to 65535 counts (262 ms) between reads is exact ...
    tickClockStart(c, 100);
    assert(tickClockRead(c, (uint16_t) (100 + 65535)) == 65535);
    // ... and a whole round between two reads cannot be seen (which is why the ads' waits poll it)
    tickClockStart(c, 100);
    assert(tickClockRead(c, (uint16_t) (100 + 65536)) == 0);

    // Several hours without losing a count
    tickClockStart(c, 0);
    uint64_t real = 0;
    for (int i = 0; i < 200000; i++) {
        real += 60000;
        assert(tickClockRead(c, (uint16_t) real) == (uint32_t) real);
    }
    assert(real > 0xFFFFFFFFULL); // went past 32 bits: it wraps like any counter, after 4 h 46 min
    tests++;
}

int main() {
    testWidths();
    testCentre();
    testOffsetToShow();
    testCursor();
    testCursorRandom();
    testTickClock();
    printf("text_window_test: all %d tests passed\n", tests);
    return 0;
}
