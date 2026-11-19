//
// Where the columns of a text are, for any of the sign's fonts, and a clock that needs no interrupt.
//
// Pure functions: no hardware access, no Arduino headers, so the same file is compiled into the
// sketch and into the host unit test (test/text_window_test.cpp).
//
// A text is a row of "cells", one per character: the character's lit columns, then one blank
// column. The vertical tab (0x0B) has no lit columns, so it is one blank column. The heart (0x7F)
// can be wider than the other characters; how wide is a property of the font (TextMetrics).
// After the last character the row is blank for ever.
//
// Integer widths: the sketch runs on an AVR where int is 16 bits and the sketch's `uint` is 8 bits.
// A message can be about 570 characters, up to 9 columns each, so string lengths and column
// positions are uint16_t here.
//
// ChooserScroll.h has the same arithmetic for the chooser preview, with the standard font's widths
// written in and a strip that goes round. The two could share this file one day.
//

#ifndef LED_MARQUEE_TEXTWINDOW_H
#define LED_MARQUEE_TEXTWINDOW_H

#include <stdint.h>

#define TEXT_CHAR_VT 0x0B
#define TEXT_CHAR_HEART 0x7F
// The fonts hold the characters 0x20 to 0x7F
#define TEXT_CHAR_FIRST 0x20
#define TEXT_CHAR_LAST 0x7F

// What the column arithmetic needs to know about a font
struct TextMetrics {
    uint8_t glyphCols; // lit columns of an ordinary character
    uint8_t heartCols; // lit columns of the heart
};

// True when the fonts have a picture for this byte. Any other byte is drawn as a space.
static inline bool textInFont(char ch) {
    return (uint8_t) ch >= TEXT_CHAR_FIRST && (uint8_t) ch <= TEXT_CHAR_LAST;
}

// Lit columns of one character. The blank column that follows every character is not counted.
static inline uint8_t textGlyphCols(const TextMetrics& m, char ch) {
    if (ch == TEXT_CHAR_VT) {
        return 0;
    }
    if (ch == TEXT_CHAR_HEART) {
        return m.heartCols;
    }
    return m.glyphCols;
}

// Width of a string in columns: every character's lit columns plus one blank column between
// characters (none after the last one). 0 for an empty string.
static inline uint16_t textCols(const TextMetrics& m, const char* s) {
    uint16_t cols = 0;
    while (*s) {
        cols += textGlyphCols(m, *s) + 1;
        s++;
    }
    return cols ? cols - 1 : 0;
}

// The blank columns before and after a text of `cols` columns that put it in the middle of a panel.
// When they cannot be equal the extra column goes after the text. A text as wide as the panel or
// wider gets none.
//   60 - 6 columns: 27 before, 27 after        60 - 5 columns: 27 before, 28 after
static inline void textCentre(uint16_t cols, uint8_t panelCols, uint8_t* startPad, uint8_t* endPad) {
    uint8_t spare = cols >= panelCols ? 0 : (uint8_t) (panelCols - cols);
    *startPad = spare / 2;
    *endPad = spare - spare / 2;
}

// How far a text has to have moved left for its column `endCol` (counted from 1) to be the last
// column of the panel. 0 while that column is inside the panel anyway.
static inline uint16_t textOffsetToShow(uint16_t endCol, uint8_t panelCols) {
    return endCol > panelCols ? endCol - panelCols : 0;
}

// A place in the row of cells, moved one column at a time while a frame is sent.
struct TextCursor {
    uint16_t index;    // which character of the string (the terminating NUL when past the end)
    uint8_t col;       // which column of its cell
    uint8_t glyphCols; // columns 0..glyphCols-1 of the cell are lit columns, column glyphCols is the blank one
};

// Sets the cursor to the first column of the cell of character c.index.
static inline void textCursorLoad(const TextMetrics& m, const char* s, TextCursor& c) {
    c.col = 0;
    c.glyphCols = s[c.index] ? textGlyphCols(m, s[c.index]) : 0;
}

// The cursor for column `pos` of the text (0 = its first column). Walks the string, so call it
// before interrupts are turned off for a frame.
static inline TextCursor textSeek(const TextMetrics& m, const char* s, uint16_t pos) {
    TextCursor c;
    c.index = 0;
    textCursorLoad(m, s, c);
    while (s[c.index] && pos > c.glyphCols) {
        pos -= c.glyphCols + 1;
        c.index++;
        textCursorLoad(m, s, c);
    }
    c.col = s[c.index] ? (uint8_t) pos : 0;
    return c;
}

// True when the cursor is on a lit column: draw column `c.col` of the picture of `s[c.index]`.
// False for a blank column (between characters, a vertical tab, after the end).
static inline bool textCursorLit(const TextCursor& c) {
    return c.col < c.glyphCols;
}

// Moves the cursor one column to the right. Cheap enough to call between two LED columns.
static inline void textCursorNext(const TextMetrics& m, const char* s, TextCursor& c) {
    if (!s[c.index]) {
        return; // past the end: blank for ever
    }
    if (c.col < c.glyphCols) {
        c.col++;
    } else {
        c.index++;
        textCursorLoad(m, s, c);
    }
}

// ---------------------------------------------------------------------------------------------
// Elapsed time from a free-running 16-bit counter, with no interrupt.
//
// The sign sends every frame with interrupts off (about 3.7 ms for 120 columns), and millis()
// loses the timer ticks that fall into such a window after the first. A hardware counter keeps
// counting whatever the interrupt flag says. Reading it and adding up the differences gives the
// real time, as long as it is read at least once per trip round (65536 counts): a difference is
// taken modulo 65536, so one wrap between two reads is counted right and two would lose a round.
//
// The sketch uses Timer1 at 250 counts a millisecond (one count is 4 us, a round is 262 ms).
// ---------------------------------------------------------------------------------------------
#define TICK_CLOCK_PER_MS 250

struct TickClock {
    uint16_t last;  // the counter at the last read
    uint32_t ticks; // counts since the start; 2^32 counts are 4 hours 46 minutes
};

static inline void tickClockStart(TickClock& c, uint16_t counter) {
    c.last = counter;
    c.ticks = 0;
}

// Takes in the counter's value now and returns the counts since tickClockStart().
static inline uint32_t tickClockRead(TickClock& c, uint16_t counter) {
    c.ticks += (uint16_t) (counter - c.last);
    c.last = counter;
    return c.ticks;
}

#endif //LED_MARQUEE_TEXTWINDOW_H
