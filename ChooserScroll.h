//
// Scroll geometry for the chooser preview (MSGTYPE_CHOOSER).
//
// Pure functions: no hardware access, no Arduino headers, so the same file is compiled into the
// sketch and into the host unit test (test/chooser_scroll_test.cpp, see the comment at its top).
//
// The picture on panel 1 (60 columns):
//
//     | count (dim red) | 1 blank column | viewport: the message text ............ |
//     0                                                                          59
//
// When the text is wider than the viewport it scrolls left one column at a time through a "strip":
// the text, then a gap of blank columns, then the text again. When the second copy's first column
// reaches the left edge of the viewport the position goes back to 0, so the loop is seamless.
//
// Integer widths: the sketch runs on an AVR where int is 16 bits and the sketch's `uint` is 8 bits.
// A 64 character text is up to 511 columns wide, so widths and positions are uint16_t here.
//

#ifndef LED_MARQUEE_CHOOSERSCROLL_H
#define LED_MARQUEE_CHOOSERSCROLL_H

#include <stdint.h>

// The longest text and count the sign keeps (the app cuts the text at the same length).
#define CHOOSER_MAX_CHARS 64
#define CHOOSER_COUNT_MAX_CHARS 12

// Copies of the sketch's panel and font numbers. The sketch checks them with static_assert.
#define CHOOSER_PANEL_COLS 60
#define CHOOSER_GLYPH_COLS 5
#define CHOOSER_HEART_COLS 7
#define CHOOSER_CHAR_VT 0x0B
#define CHOOSER_CHAR_HEART 0x7F

// Lit columns of one character, the same rule as sendChar(): the vertical tab has none (it is only
// the blank column that follows every character) and the heart is wider than the rest.
static inline uint8_t chooserGlyphCols(char ch) {
    if (ch == CHOOSER_CHAR_VT) {
        return 0;
    }
    if (ch == CHOOSER_CHAR_HEART) {
        return CHOOSER_HEART_COLS;
    }
    return CHOOSER_GLYPH_COLS;
}

// Width of a string in columns: every glyph plus one blank column between characters (none after
// the last one). 0 for an empty string.
static inline uint16_t chooserStringCols(const char* s) {
    uint16_t cols = 0;
    while (*s) {
        cols += chooserGlyphCols(*s) + 1;
        s++;
    }
    return cols ? cols - 1 : 0;
}

struct ChooserLayout {
    uint8_t countCols;  // columns of the count that are drawn (a count wider than the panel is clipped)
    uint8_t viewCols;   // width of the text viewport: what is left after the count and one blank column
    uint16_t textCols;  // width of the whole text
    uint16_t cycleCols; // text + gap: the scroll position at which the second copy reaches the left edge
};

static inline ChooserLayout chooserLayout(const char* count, const char* text, uint8_t gapCols) {
    ChooserLayout l;
    uint16_t countCols = chooserStringCols(count);
    l.countCols = countCols > CHOOSER_PANEL_COLS ? CHOOSER_PANEL_COLS : (uint8_t) countCols;
    l.viewCols = l.countCols >= CHOOSER_PANEL_COLS - 1 ? 0 : (uint8_t) (CHOOSER_PANEL_COLS - 1 - l.countCols);
    l.textCols = chooserStringCols(text);
    l.cycleCols = l.textCols + (gapCols ? gapCols : 1);
    return l;
}

// True when the text does not fit beside the count and has to scroll.
static inline bool chooserScrolls(const ChooserLayout& l) {
    return l.viewCols > 0 && l.textCols > l.viewCols;
}

// The scroll position one column further left, or 0 when the second copy has reached the left edge.
static inline uint16_t chooserNextPos(const ChooserLayout& l, uint16_t pos) {
    pos++;
    return pos >= l.cycleCols ? 0 : pos;
}

// A place in the strip, moved one column at a time while a frame is sent.
// Each character owns a "cell": its glyph columns, then blank columns (one between characters,
// the whole gap after the last character). After the last cell comes the first one again.
struct ChooserCursor {
    uint8_t index;     // which character of the string
    uint8_t col;       // which column of its cell
    uint8_t glyphCols; // columns 0..glyphCols-1 of the cell are the glyph, the rest are blank
    uint8_t cellCols;  // total columns in this cell
};

// Sets the cursor to the first column of the cell of character c.index (wrapping past the end).
static inline void chooserCursorLoad(const char* s, uint8_t gapCols, ChooserCursor& c) {
    c.col = 0;
    if (!s[0]) {
        // an empty string is blank columns forever
        c.index = 0;
        c.glyphCols = 0;
        c.cellCols = 255;
        return;
    }
    if (!s[c.index]) {
        c.index = 0; // past the last character: the second copy
    }
    c.glyphCols = chooserGlyphCols(s[c.index]);
    if (s[c.index + 1]) {
        c.cellCols = c.glyphCols + 1;
    } else {
        c.cellCols = c.glyphCols + (gapCols ? gapCols : 1); // gap 0 would let an all-blank text never advance
    }
}

// The cursor for strip column `pos` (the column shown at the left edge of the viewport at scroll
// position `pos`). Walks the string, so call it before interrupts are turned off for a frame.
static inline ChooserCursor chooserSeek(const char* s, uint8_t gapCols, uint16_t pos) {
    ChooserCursor c;
    c.index = 0;
    chooserCursorLoad(s, gapCols, c);
    while (pos >= c.cellCols) {
        pos -= c.cellCols;
        c.index++;
        chooserCursorLoad(s, gapCols, c);
    }
    c.col = (uint8_t) pos;
    return c;
}

// True when the cursor is on a glyph column: draw column `c.col` of the glyph of `s[c.index]`.
// False for a blank column.
static inline bool chooserCursorLit(const ChooserCursor& c) {
    return c.col < c.glyphCols;
}

// Moves the cursor one column to the right. Cheap enough to call between two LED columns.
static inline void chooserCursorNext(const char* s, uint8_t gapCols, ChooserCursor& c) {
    c.col++;
    if (c.col >= c.cellCols) {
        c.index++;
        chooserCursorLoad(s, gapCols, c);
    }
}

// Copies at most maxChars characters of src and always terminates dst (dst holds maxChars + 1 bytes).
// A missing string (NULL) is stored as an empty one.
static inline void chooserCopy(char* dst, const char* src, uint8_t maxChars) {
    if (src) {
        while (maxChars && *src) {
            *dst++ = *src++;
            maxChars--;
        }
    }
    *dst = '\0';
}

// True when chooserCopy(dst, incoming, maxChars) would store exactly what `stored` already holds.
static inline bool chooserSame(const char* stored, const char* incoming, uint8_t maxChars) {
    if (!incoming) {
        incoming = "";
    }
    while (maxChars && *incoming) {
        if (*stored != *incoming) {
            return false;
        }
        stored++;
        incoming++;
        maxChars--;
    }
    return *stored == '\0';
}

#endif //LED_MARQUEE_CHOOSERSCROLL_H
