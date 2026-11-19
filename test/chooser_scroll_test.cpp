// Host unit test for ChooserScroll.h (the chooser preview's scroll geometry).
// No Arduino, no framework: plain asserts, compiled with the Mac's clang++.
//
//   clang++ -std=c++11 -Wall -Wextra -o /tmp/chooser_scroll_test test/chooser_scroll_test.cpp \
//       && /tmp/chooser_scroll_test
//
// It prints "chooser_scroll_test: all N tests passed" and exits 0, or stops at the first failed assert.

#undef NDEBUG

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../ChooserScroll.h"

static const uint8_t GAP = 18; // three spaces, the sign's default CHOOSER_GAP_COLS

// ---------------------------------------------------------------------------------------------
// An independent, deliberately naive model of the scrolling strip: one entry per LED column.
// The strip is the text, then `gap` blank columns, then the text again, forever.
// ---------------------------------------------------------------------------------------------
struct Cell {
    bool lit;      // true: column `col` of the glyph of character `index`; false: a blank column
    int index;
    int col;
};

static bool operator==(const Cell& a, const Cell& b) {
    if (a.lit != b.lit) return false;
    return !a.lit || (a.index == b.index && a.col == b.col);
}

static int modelGlyphCols(char ch) {
    if (ch == 0x0B) return 0; // VT: only the blank column after it
    if (ch == 0x7F) return 7; // the heart is wider than the other glyphs
    return 5;
}

// One period of the strip: the text with one blank column between characters, then the gap.
static std::vector<Cell> modelPeriod(const std::string& text, int gap) {
    std::vector<Cell> strip;
    for (size_t i = 0; i < text.size(); i++) {
        for (int c = 0; c < modelGlyphCols(text[i]); c++) strip.push_back({true, (int) i, c});
        if (i + 1 < text.size()) strip.push_back({false, 0, 0});
    }
    for (int g = 0; g < gap; g++) strip.push_back({false, 0, 0});
    return strip;
}

// What the firmware would put in the `viewCols` columns of the viewport at scroll position `pos`.
static std::vector<Cell> viewport(const char* text, uint8_t gap, uint16_t pos, int viewCols) {
    std::vector<Cell> out;
    ChooserCursor cur = chooserSeek(text, gap, pos);
    for (int v = 0; v < viewCols; v++) {
        if (chooserCursorLit(cur)) {
            out.push_back({true, cur.index, cur.col});
        } else {
            out.push_back({false, 0, 0});
        }
        chooserCursorNext(text, gap, cur);
    }
    return out;
}

static std::vector<Cell> modelViewport(const std::string& text, int gap, int pos, int viewCols) {
    std::vector<Cell> period = modelPeriod(text, gap);
    std::vector<Cell> out;
    for (int v = 0; v < viewCols; v++) {
        if (text.empty()) {
            out.push_back({false, 0, 0});
        } else {
            out.push_back(period[(pos + v) % period.size()]);
        }
    }
    return out;
}

static std::string repeat(const char* unit, size_t length) {
    std::string s;
    while (s.size() < length) s += unit;
    return s.substr(0, length);
}

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

static void stringWidthCountsFiveColumnsPerGlyphPlusOneBetween() {
    assert(chooserStringCols("") == 0);
    assert(chooserStringCols("A") == 5);
    assert(chooserStringCols("AB") == 11);
    assert(chooserStringCols("3/9") == 17);
    assert(chooserStringCols("1234567") == 41);
}

static void stringWidthFollowsSendCharForHeartAndVerticalTab() {
    assert(chooserGlyphCols('A') == 5);
    assert(chooserGlyphCols('\x7F') == 7);
    assert(chooserGlyphCols('\x0B') == 0);
    assert(chooserStringCols("\x7F") == 7);
    assert(chooserStringCols("A\x7F" "B") == 5 + 1 + 7 + 1 + 5);
    assert(chooserStringCols("NEW\x0BMSG") == 17 + 1 + 1 + 17);
}

static void sixtyFourCharacterTextWidthFitsSixteenBits() {
    std::string text = repeat("ABCDEFGHIJ", 64);
    assert(chooserStringCols(text.c_str()) == 64 * 6 - 1); // 383: does not fit the sketch's 8-bit uint
    std::string hearts(64, '\x7F');
    assert(chooserStringCols(hearts.c_str()) == 64 * 8 - 1); // 511, the widest text there can be
}

static void countOfThreeToSevenCharactersLeavesTheRestOfThePanelAsViewport() {
    const char* counts[] = {"3/9", "3/12", "12/34", "12/345", "123/456"};
    const int expectCountCols[] = {17, 23, 29, 35, 41};
    const int expectViewCols[] = {42, 36, 30, 24, 18};
    for (int i = 0; i < 5; i++) {
        ChooserLayout l = chooserLayout(counts[i], "HELLO", GAP);
        assert(l.countCols == expectCountCols[i]);
        assert(l.viewCols == expectViewCols[i]);
        // the count, one blank column and the viewport fill panel 1 exactly: nothing past column 59
        assert(l.countCols + 1 + l.viewCols == CHOOSER_PANEL_COLS);
    }
}

static void countTooWideForThePanelIsClippedAndLeavesNoViewport() {
    ChooserLayout nine = chooserLayout("123456789", "HELLO", GAP); // 53 columns
    assert(nine.countCols == 53 && nine.viewCols == 6);
    ChooserLayout ten = chooserLayout("1234567890", "HELLO", GAP); // 59 columns + the blank = 60
    assert(ten.countCols == 59 && ten.viewCols == 0);
    ChooserLayout twelve = chooserLayout("123456789012", "HELLO", GAP); // 71 columns: clipped
    assert(twelve.countCols == CHOOSER_PANEL_COLS && twelve.viewCols == 0);
    assert(!chooserScrolls(ten));
    assert(!chooserScrolls(twelve));
}

static void emptyCountGivesTheViewportEverythingButTheBlankColumn() {
    ChooserLayout l = chooserLayout("", "HELLO", GAP);
    assert(l.countCols == 0);
    assert(l.viewCols == 59);
}

static void textThatFitsDoesNotScroll() {
    // "3/9" leaves 42 columns; seven characters are 41
    ChooserLayout l = chooserLayout("3/9", "1234567", GAP);
    assert(l.textCols == 41 && l.viewCols == 42);
    assert(!chooserScrolls(l));
    // a short text beside a wider count
    ChooserLayout shortText = chooserLayout("12/34", "ABCD", GAP);
    assert(shortText.textCols == 23 && shortText.viewCols == 30);
    assert(!chooserScrolls(shortText));
    // exactly as wide as the viewport still fits (the vertical tab is a single blank column)
    ChooserLayout flush = chooserLayout("3/9", "\x0B" "1234567", GAP); // 1 + 41 = 42 columns
    assert(flush.textCols == 42 && flush.viewCols == 42);
    assert(!chooserScrolls(flush));
}

static void textOneColumnTooWideScrolls() {
    ChooserLayout l = chooserLayout("3/9", "\x0B\x0B" "1234567", GAP); // 2 + 41 = 43 columns in 42
    assert(l.textCols == 43 && l.viewCols == 42);
    assert(chooserScrolls(l));
    // the usual case: eight characters (47 columns) beside a three-character count
    assert(chooserScrolls(chooserLayout("3/9", "12345678", GAP)));
}

static void emptyTextDoesNotScrollAndDrawsOnlyBlankColumns() {
    ChooserLayout l = chooserLayout("3/9", "", GAP);
    assert(l.textCols == 0);
    assert(!chooserScrolls(l));
    for (uint16_t pos = 0; pos < 600; pos += 7) {
        std::vector<Cell> view = viewport("", GAP, pos, 60);
        for (size_t v = 0; v < view.size(); v++) assert(!view[v].lit);
    }
}

static void staticTextStartsAtTheLeftEdgeColumnByColumn() {
    // position 0: "AB" = A0..A4, blank, B0..B4, then nothing but the gap
    std::vector<Cell> view = viewport("AB", GAP, 0, 14);
    for (int c = 0; c < 5; c++) assert(view[c] == (Cell{true, 0, c}));
    assert(!view[5].lit);
    for (int c = 0; c < 5; c++) assert(view[6 + c] == (Cell{true, 1, c}));
    assert(!view[11].lit && !view[12].lit && !view[13].lit);
}

static void scrollPositionSkipsColumnsOfTheFirstCharacter() {
    // position 3: A3, A4, blank, B0...
    std::vector<Cell> view = viewport("AB", GAP, 3, 4);
    assert(view[0] == (Cell{true, 0, 3}));
    assert(view[1] == (Cell{true, 0, 4}));
    assert(!view[2].lit);
    assert(view[3] == (Cell{true, 1, 0}));
    // position 5 is the blank column between the two characters
    assert(!viewport("AB", GAP, 5, 1)[0].lit);
    assert(viewport("AB", GAP, 6, 1)[0] == (Cell{true, 1, 0}));
}

static void gapOfEighteenBlankColumnsFollowsTheLastCharacter() {
    // "AB" is 11 columns: strip columns 11..28 are the gap, 29 is the second copy's first column
    std::vector<Cell> view = viewport("AB", GAP, 10, 20);
    assert(view[0] == (Cell{true, 1, 4})); // last column of B
    for (int g = 1; g <= 18; g++) assert(!view[g].lit);
    assert(view[19] == (Cell{true, 0, 0})); // the text again from its start
}

static void secondCopyFollowsTheGapFromItsFirstCharacter() {
    const char* text = "12345678"; // 47 columns
    ChooserLayout l = chooserLayout("3/9", text, GAP);
    // scrolled so the last character's last column is at the left edge
    std::vector<Cell> view = viewport(text, GAP, 46, l.viewCols);
    assert(view[0] == (Cell{true, 7, 4}));
    for (int g = 1; g <= 18; g++) assert(!view[g].lit);
    assert(view[19] == (Cell{true, 0, 0}));
    assert(view[24] == (Cell{false, 0, 0}));
    assert(view[25] == (Cell{true, 1, 0}));
    assert(view[41] == (Cell{true, 3, 4})); // the right edge of the 42-column viewport
}

static void positionResetsWhenTheSecondCopyReachesTheLeftEdge() {
    const char* text = "12345678"; // 47 columns
    ChooserLayout l = chooserLayout("3/9", text, GAP);
    assert(l.cycleCols == 47 + 18);
    assert(chooserNextPos(l, 0) == 1);
    assert(chooserNextPos(l, 63) == 64);
    assert(chooserNextPos(l, 64) == 0); // 65 would show the second copy at the left edge: that is position 0
    // ... and the picture at the reset point is the picture at position 0, so the loop is seamless
    assert(viewport(text, GAP, l.cycleCols, l.viewCols) == viewport(text, GAP, 0, l.viewCols));
    // one step before the reset the viewport starts with the last gap column
    std::vector<Cell> last = viewport(text, GAP, 64, 2);
    assert(!last[0].lit);
    assert(last[1] == (Cell{true, 0, 0}));
}

static void sixtyFourCharacterTextScrollsThroughEveryPosition() {
    std::string text = repeat("THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG. ", 64);
    assert(text.size() == 64);
    ChooserLayout l = chooserLayout("12/34", text.c_str(), GAP);
    assert(l.textCols == 383 && l.cycleCols == 401 && l.viewCols == 30);
    assert(chooserScrolls(l));
    uint16_t pos = 0;
    int steps = 0;
    do {
        assert(viewport(text.c_str(), GAP, pos, l.viewCols) == modelViewport(text, GAP, pos, l.viewCols));
        pos = chooserNextPos(l, pos);
        steps++;
    } while (pos != 0);
    assert(steps == 401);
    // the last character really is reachable (index 63 does not overflow anything)
    assert(viewport(text.c_str(), GAP, 382, 1)[0] == (Cell{true, 63, 4}));
}

static void cursorMatchesTheModelForEveryLengthPositionAndGap() {
    const int gaps[] = {1, 6, 18, 60};
    for (int g = 0; g < 4; g++) {
        for (size_t len = 0; len <= 64; len++) {
            std::string text = repeat("Ab\x7F" "c d\x0B" "e!", len);
            int period = (int) modelPeriod(text, gaps[g]).size();
            // past the reset point too: seeking must wrap instead of running off the string
            for (int pos = 0; pos < period + 70; pos += (len > 20 ? 5 : 1)) {
                assert(viewport(text.c_str(), gaps[g], pos, 60) == modelViewport(text, gaps[g], pos, 60));
            }
        }
    }
}

static void gapOfZeroIsTreatedAsOneSoTheLoopAlwaysAdvances() {
    ChooserLayout l = chooserLayout("3/9", "12345678", 0);
    assert(l.cycleCols == 47 + 1);
    assert(viewport("AB", 0, 0, 30) == modelViewport("AB", 1, 0, 30));
    assert(viewport("\x0B", 0, 0, 30) == modelViewport("\x0B", 1, 0, 30)); // a text with no lit column at all
}

static void copyCutsAtTheLimitAndAlwaysTerminates() {
    char buf[CHOOSER_MAX_CHARS + 1];
    memset(buf, 'x', sizeof(buf));
    chooserCopy(buf, "HELLO", CHOOSER_MAX_CHARS);
    assert(strcmp(buf, "HELLO") == 0);

    std::string longText = repeat("0123456789", 80);
    memset(buf, 'x', sizeof(buf));
    chooserCopy(buf, longText.c_str(), CHOOSER_MAX_CHARS);
    assert(strlen(buf) == 64);
    assert(longText.compare(0, 64, buf) == 0);

    memset(buf, 'x', sizeof(buf));
    chooserCopy(buf, NULL, CHOOSER_MAX_CHARS); // a chooser message with no "str" at all
    assert(buf[0] == '\0');

    char count[CHOOSER_COUNT_MAX_CHARS + 1];
    chooserCopy(count, "123456789012345", CHOOSER_COUNT_MAX_CHARS);
    assert(strcmp(count, "123456789012") == 0);
}

static void sameComparesWhatWouldBeStored() {
    char buf[CHOOSER_MAX_CHARS + 1];
    chooserCopy(buf, "HELLO WORLD", CHOOSER_MAX_CHARS);
    assert(chooserSame(buf, "HELLO WORLD", CHOOSER_MAX_CHARS));
    assert(!chooserSame(buf, "HELLO WORLD!", CHOOSER_MAX_CHARS));
    assert(!chooserSame(buf, "HELLO WORL", CHOOSER_MAX_CHARS));
    assert(!chooserSame(buf, "", CHOOSER_MAX_CHARS));
    assert(!chooserSame(buf, NULL, CHOOSER_MAX_CHARS));

    // a text longer than the limit equals its own stored (cut) copy: no restart on every poll
    std::string longText = repeat("0123456789", 80);
    chooserCopy(buf, longText.c_str(), CHOOSER_MAX_CHARS);
    assert(chooserSame(buf, longText.c_str(), CHOOSER_MAX_CHARS));
    std::string other = longText;
    other[63] = '!';
    assert(!chooserSame(buf, other.c_str(), CHOOSER_MAX_CHARS));
    other = longText;
    other[64] = '!'; // differs only past the cut: the same on the sign
    assert(chooserSame(buf, other.c_str(), CHOOSER_MAX_CHARS));

    chooserCopy(buf, NULL, CHOOSER_MAX_CHARS);
    assert(chooserSame(buf, "", CHOOSER_MAX_CHARS));
    assert(chooserSame(buf, NULL, CHOOSER_MAX_CHARS));
}

static void checksumIsCrc16CcittFalse() {
    // the standard check value: "123456789" -> 0x29B1. Here the text, one zero byte and the count
    // go in, so feed the check string as the text of a plain CRC step by step.
    uint16_t crc = 0xFFFF;
    for (const char* p = "123456789"; *p; p++) crc = chooserCrcByte(crc, (uint8_t) *p);
    assert(crc == 0x29B1);
}

static void checksumMatchesTheContractVectors() {
    // the app lane has the same seven vectors in its unit test: this is how the two halves agree
    assert(chooserCrc("HELLO W", "3/12") == 13345);
    const char* longest = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG AND KEEPS ON RUNNING";
    assert(strlen(longest) == 64);
    assert(chooserCrc(longest, "7/12") == 36894);
    assert(chooserCrc("<empty>", "1/0") == 7805);
    assert(chooserCrc("", "1/1") == 18175);
    assert(chooserCrc("A", "1/1") == 26634);
    assert(chooserCrc("A", "11/1") == 47554); // the zero byte keeps "A1" + "1/1" apart from "A" + "11/1"
    assert(chooserCrc("I \x7F U", "12/345") == 46267);
}

static void checksumTakesBytesAboveAsciiAsUnsigned() {
    // char is signed on the AVR and on the Mac: 0xC3 must go in as 195, not as -61
    uint16_t expect = 0xFFFF;
    expect = chooserCrcByte(expect, 0xC3);
    expect = chooserCrcByte(expect, 0xA9);
    expect = chooserCrcByte(expect, 0);
    expect = chooserCrcByte(expect, '1');
    assert(chooserCrc("\xC3\xA9", "1") == expect);
}

int main() {
    int tests = 0;
#define RUN(test) do { test(); tests++; } while (0)
    RUN(stringWidthCountsFiveColumnsPerGlyphPlusOneBetween);
    RUN(stringWidthFollowsSendCharForHeartAndVerticalTab);
    RUN(sixtyFourCharacterTextWidthFitsSixteenBits);
    RUN(countOfThreeToSevenCharactersLeavesTheRestOfThePanelAsViewport);
    RUN(countTooWideForThePanelIsClippedAndLeavesNoViewport);
    RUN(emptyCountGivesTheViewportEverythingButTheBlankColumn);
    RUN(textThatFitsDoesNotScroll);
    RUN(textOneColumnTooWideScrolls);
    RUN(emptyTextDoesNotScrollAndDrawsOnlyBlankColumns);
    RUN(staticTextStartsAtTheLeftEdgeColumnByColumn);
    RUN(scrollPositionSkipsColumnsOfTheFirstCharacter);
    RUN(gapOfEighteenBlankColumnsFollowsTheLastCharacter);
    RUN(secondCopyFollowsTheGapFromItsFirstCharacter);
    RUN(positionResetsWhenTheSecondCopyReachesTheLeftEdge);
    RUN(sixtyFourCharacterTextScrollsThroughEveryPosition);
    RUN(cursorMatchesTheModelForEveryLengthPositionAndGap);
    RUN(gapOfZeroIsTreatedAsOneSoTheLoopAlwaysAdvances);
    RUN(copyCutsAtTheLimitAndAlwaysTerminates);
    RUN(sameComparesWhatWouldBeStored);
    RUN(checksumIsCrc16CcittFalse);
    RUN(checksumMatchesTheContractVectors);
    RUN(checksumTakesBytesAboveAsciiAsUnsigned);
    printf("chooser_scroll_test: all %d tests passed\n", tests);
    return 0;
}
