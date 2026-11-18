//
// The two message types the app's ads use:
//   'O' (MSGTYPE_ONE_BY_ONE)   letters appear one by one in the standard font, then the text stays
//   'C' (MSGTYPE_CHONKY_SLIDE) the chonky font with a colour wave running through it
//
// What they share is here:
//  - `dly` is real milliseconds. Time is taken from Timer1 (adNow), which keeps counting while a
//    frame is sent with interrupts off; millis() does not.
//  - Both sides of the sign always show the same 60 columns, and a frame is always 120 columns:
//    interrupts are never off for longer than that, however long the text is.
//  - A text wider than a panel scrolls to its end by itself.
//
// This file is part of the sketch: LED_Marquee.ino includes it after everything it uses (the AD_*
// settings and CHARS_ONEBYONE_DELAY at the top of the sketch, COLUMNS_PER_PANEL, Color, GAMMA, the
// fonts, sendColumnRGB, clear, show). test/ad_display_test.cpp includes it after fakes of the same
// names, to run it on the host against a fake sign and a fake Timer1.
//

#ifndef LED_MARQUEE_ADDISPLAY_H
#define LED_MARQUEE_ADDISPLAY_H

#include "TextWindow.h"
#include "LedBudget.h"

// Sending a frame and latching it takes 4.9 ms, so nothing can be paced faster than that
static_assert(AD_WAVE_FRAME_MS >= 6 && AD_WAVE_FRAME_MS <= 100, "AD_WAVE_FRAME_MS must be 6 to 100");
static_assert(AD_SCROLL_STEP_MS >= 6 && AD_SCROLL_STEP_MS <= 250, "AD_SCROLL_STEP_MS must be 6 to 250");
static_assert(AD_SCROLL_HOLD_MS <= 10000, "AD_SCROLL_HOLD_MS must be 0 to 10000");
static_assert(CHARS_ONEBYONE_DELAY >= 6 && CHARS_ONEBYONE_DELAY <= 250, "CHARS_ONEBYONE_DELAY must be 6 to 250");

// Milliseconds as counts of the clock
#define AD_TICKS(ms) ((uint32_t) (ms) * TICK_CLOCK_PER_MS)

// While a chonky text scrolls, the colour wave is drawn this many times per scrolled column, at
// even distances (4 at the default 35 ms a column: a frame every 8.75 ms)
#define AD_SCROLL_SUBFRAMES ((AD_SCROLL_STEP_MS + AD_WAVE_FRAME_MS / 2) / AD_WAVE_FRAME_MS)
static_assert(AD_SCROLL_SUBFRAMES >= 1, "AD_SCROLL_STEP_MS must be at least half of AD_WAVE_FRAME_MS");
#define AD_SCROLL_SUBFRAME_TICKS (AD_TICKS(AD_SCROLL_STEP_MS) / AD_SCROLL_SUBFRAMES)

// Longer than sending one frame and latching it takes (120 columns are 4.4 ms here, show() 0.5 ms).
// A frame is not started when the hold would end before it is out.
#define AD_FRAME_TICKS AD_TICKS(6)

// Each of the frames of a scrolled column needs the time a frame takes. A step of 9 ms with a wave
// frame of 6 ms would ask for two frames of 4.5 ms: they cannot be sent that fast, and the scroll
// would fall behind its clock. Such a pair of settings is refused here rather than adjusted.
static_assert(AD_SCROLL_SUBFRAME_TICKS >= AD_FRAME_TICKS,
              "AD_SCROLL_STEP_MS and AD_WAVE_FRAME_MS: the wave frames of one scrolled column would be less "
              "than 6 ms apart; make AD_SCROLL_STEP_MS a multiple of AD_WAVE_FRAME_MS or larger");

// "All the characters" for adDrawFrame's `shown`
#define AD_SHOW_ALL 0xFFFF

// ---------------------------------------------------------------------------------------------
// The supply. Two settings, both off (0) unless the sketch says otherwise; with both off none of
// the code below them is compiled and the ads are sent exactly as without it.
//
// LED_CURRENT_LIMIT_MA: the most current the LEDs of one frame of an ad may draw, both sides
// together, by the model in LedBudget.h. LED_CURRENT_LIMIT_SIDE_MA: the most for one side. (Both
// sides show the same picture, so one side is half the frame; which of the two limits bites
// depends on how the sign is fed.) Before a message's first frame the fullest 60 columns of its
// text are counted, and if that many LEDs in the message's colour would be above a limit, the
// colour is dimmed for the whole message (one brightness from its first frame to its last) by
// just as much as it takes. The limits are on what the lit LEDs add: the current that flows with
// the sign dark (about 0.84 A) is not counted. The brightness shift of the 'B' utility message is
// taken into account: a text that the shift already brings under a limit is not dimmed again.
//
// AD_RAMP_MS: a chonky text does not come on in one frame but rises to its brightness over this
// long, a step every frame. (One-by-one texts come on a letter at a time anyway.) The time is cut
// into parts as long as a frame of the wave (a rest is dropped: 105 ms are 10 parts of 10 ms), and
// the text is at its brightness in the last part: 90 ms after its first frame for a ramp of 100.
// ---------------------------------------------------------------------------------------------
#ifndef LED_CURRENT_LIMIT_MA
#define LED_CURRENT_LIMIT_MA 0
#endif
#ifndef LED_CURRENT_LIMIT_SIDE_MA
#define LED_CURRENT_LIMIT_SIDE_MA 0
#endif
#ifndef AD_RAMP_MS
#define AD_RAMP_MS 0
#endif
static_assert(LED_CURRENT_LIMIT_MA >= 0 && LED_CURRENT_LIMIT_MA <= 60000, "LED_CURRENT_LIMIT_MA must be 0 (off) to 60000");
static_assert(LED_CURRENT_LIMIT_SIDE_MA >= 0 && LED_CURRENT_LIMIT_SIDE_MA <= 60000,
              "LED_CURRENT_LIMIT_SIDE_MA must be 0 (off) to 60000");
static_assert(AD_RAMP_MS >= 0 && AD_RAMP_MS / AD_WAVE_FRAME_MS <= 255,
              "AD_RAMP_MS must be 0 (off) to 255 frames of the wave");
static_assert(AD_RAMP_MS == 0 || AD_RAMP_MS >= 2 * AD_WAVE_FRAME_MS,
              "AD_RAMP_MS must be 0 (off) or at least two frames of the wave: a shorter ramp would do nothing");

#ifndef LED_LIMITS
#define LED_LIMITS (LED_CURRENT_LIMIT_MA > 0 || LED_CURRENT_LIMIT_SIDE_MA > 0)
#endif
#define AD_LIMITS LED_LIMITS
#define AD_RAMP_FRAMES (AD_RAMP_MS / AD_WAVE_FRAME_MS)
// true when the values of an ad can be other than the message's
#define AD_DIMS (AD_LIMITS || AD_RAMP_FRAMES > 1)

// ---------------------------------------------------------------------------------------------
// The clock: Timer1, free running, 16 MHz / 64 = 250 counts a millisecond. No interrupt. The
// compare-output bits are 0, so pins 9 and 10 (10 is SoftwareSerial's RX) are left alone.
// Nothing else in the sketch uses Timer1 (no Servo, no analogWrite on pins 9 and 10).
//
// adNow() must be called at least every 262 ms (see TickClock in TextWindow.h). Everything in this
// file waits by calling it in a loop, and the longest stretch without a call is one frame (5 ms).
// ---------------------------------------------------------------------------------------------
#ifndef AD_TIMER_COUNT
#define AD_TIMER_COUNT() TCNT1
#endif

static TickClock adClock;

#ifndef AD_NO_TIMER_SETUP
// Call once from setup(). (Arduino's own start-up leaves Timer1 counting to 255 and back for PWM.)
static void adClockSetup() {
    static_assert(F_CPU / 64 == TICK_CLOCK_PER_MS * 1000UL, "Timer1 at F_CPU/64 must give TICK_CLOCK_PER_MS");
    TIMSK1 = 0;                      // no Timer1 interrupts
    TCCR1A = 0;                      // normal mode: count 0 to 65535 and round; OC1A and OC1B disconnected
    TCCR1B = _BV(CS11) | _BV(CS10);  // clock / 64
}
#endif

// Starts counting time from zero.
static void adClockStart() {
    tickClockStart(adClock, AD_TIMER_COUNT());
}

// Counts (4 us each) since adClockStart().
static uint32_t adNow() {
    return tickClockRead(adClock, AD_TIMER_COUNT());
}

static void adWaitUntil(uint32_t ticks) {
    while (adNow() < ticks) {
    }
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

// The colours the ads have on the sign depend on a quirk of sendBitx8 (PixelBitBanging.h): it walks
// its bit mask with `ror`, which shifts the carry flag in, so a colour byte that starts with the
// carry set goes out as v | v >> 1. Red and blue always start that way (sendBitx8 returns with the
// carry set, and they follow another byte). Green is the first byte of a column and started with
// whatever the code before it had left in the flag: clear in the one-by-one style (green went out
// as it is), set in the chonky style (green went out as g | g >> 1). So that neither style changes
// colour, the flag is put the way it was just before each column. emulator/tests/ads_test.py holds
// the result to it LED for LED. (With a brightness shift other than 0 the flag is whatever the
// shift leaves, here as everywhere else in the sketch.)
#ifndef AD_GREEN_STARTS_WITH_CARRY
#define AD_GREEN_STARTS_WITH_CARRY(set) do { if (set) { asm volatile("sec"); } else { asm volatile("clc"); } } while (0)
#endif

#if AD_DIMS
// How far the supply's limit dims the chonky text being shown (255 = not at all); how long the
// message's earlier positions lasted, in ticks of the clock (the text comes on once per message,
// over AD_RAMP_MS of real time, whatever happens on the way: a nudge to the next position neither
// cuts the ramp short nor, when its frames come faster than the wave's, hurries it); and what the
// two make of the frame being sent
static uint adTextScale = 255;
static uint32_t adRampBase = 0;
static uint adWaveScale = 255;
#endif

// Sends one side of the sign: `pad` blank columns, then the text from the cursor on, up to the
// panel's edge. Characters from number `shown` on are not drawn yet (blank). With `wave`, red goes
// up by 10 from one column to the next, as the chonky style always did. Blank columns are black.
// A byte outside the font is drawn as a space. Call with interrupts off.
static void adSendSide(const Font& font, const TextMetrics& m, const char* s, TextCursor cur, uint16_t shown,
                       uint pad, bool wave, uint r, uint g, uint b) {
    uint n = COLUMNS_PER_PANEL - pad;
    while (pad--) {
        sendColumnRGB(0, 0, 0, 0);
    }
    while (n--) {
        uint row = 0;
        if (cur.index < shown && textCursorLit(cur)) {
            uint ch = (uint) s[cur.index] - TEXT_CHAR_FIRST;
            if (ch <= TEXT_CHAR_LAST - TEXT_CHAR_FIRST) {
                row = pgm_read_byte_near(font.data + ch * font.width + cur.col);
            }
        }
#if AD_DIMS
        // The wave's red is dimmed here, column by column; the caller has dimmed everything else.
        // This has to come before the carry flag is set for the column: the arithmetic changes it.
        const uint red = wave ? ledScaled(r, adWaveScale) : r;
        AD_GREEN_STARTS_WITH_CARRY(wave);
        sendColumnRGB(row, red, g, b);
#else
        AD_GREEN_STARTS_WITH_CARRY(wave);
        sendColumnRGB(row, r, g, b);
#endif
        if (wave) {
            r += 10;
        }
        textCursorNext(m, s, cur);
    }
}

// Sends a whole frame, the same picture on both sides, and latches it: always 120 columns.
// `left` is the text column at the left edge (after the padding). Returns the time the last column
// went out, which is when the picture appears.
static uint32_t adDrawFrame(const Font& font, const TextMetrics& m, const char* s, const TextCursor& left,
                            uint16_t shown, uint pad, bool wave, uint r, uint g, uint b) {
    cli();
    adSendSide(font, m, s, left, shown, pad, wave, r, g, b);
    adSendSide(font, m, s, left, shown, pad, wave, r, g, b);
    sei();
    uint32_t sent = adNow();
    show();
    return sent;
}

// Columns from the start of a text to its last lit column. A text ends where its last LED is: the
// blank columns at the end of its last characters (a full stop is 2 lit columns of 8) do not make
// it wider than the panel. 0 for a text with nothing lit.
static uint16_t adInkCols(const Font& font, const TextMetrics& m, const char* s) {
    uint16_t pos = 0;
    uint16_t ink = 0;
    for (; *s; s++) {
        uint cols = textGlyphCols(m, *s);
        if (textInFont(*s)) {
            const uint* glyph = font.data + (uint) ((uint) *s - TEXT_CHAR_FIRST) * font.width;
            for (uint c = cols; c; c--) {
                if (pgm_read_byte_near(glyph + c - 1)) {
                    ink = pos + c;
                    break;
                }
            }
        }
        pos += cols + 1;
    }
    return ink;
}

#if AD_LIMITS
static uint adBits(uint v) {
    uint n = 0;
    for (; v; v &= v - 1) {
        n++;
    }
    return n;
}

// The lit LEDs of the cursor's column of the text
static uint adLitInColumn(const Font& font, const char* s, const TextCursor& cur) {
    if (!textCursorLit(cur) || !textInFont(s[cur.index])) {
        return 0;
    }
    return adBits(pgm_read_byte_near(font.data + (uint) ((uint) s[cur.index] - TEXT_CHAR_FIRST) * font.width + cur.col));
}

// The most LEDs one side of the sign can have lit while it shows this text: the fullest 60
// columns of the text (wherever it scrolls to, it never shows more). Walks the whole text once.
static uint16_t adMostLit(const Font& font, const TextMetrics& m, const char* s) {
    TextCursor lead = textSeek(m, s, 0); // the column that comes into the window
    TextCursor trail = lead;             // the column that leaves it
    uint16_t cols = 0;
    uint16_t lit = 0;
    uint16_t most = 0;
    while (s[lead.index]) {
        lit += adLitInColumn(font, s, lead);
        textCursorNext(m, s, lead);
        if (cols >= COLUMNS_PER_PANEL) {
            lit -= adLitInColumn(font, s, trail);
            textCursorNext(m, s, trail);
        } else {
            cols++;
        }
        if (lit > most) {
            most = lit;
        }
    }
    return most;
}

// How far this text in this colour has to be dimmed to stay within the supply's limits (255 = not
// at all). With `wave` it is a chonky text: red is the wave, green goes out carried.
static uint adBudgetScale(const Font& font, const TextMetrics& m, const char* s, uint r, uint g, uint b, bool wave) {
    const uint16_t allowed = ledAllowance(adMostLit(font, m, s), LED_LOAD_OF_MA(LED_CURRENT_LIMIT_MA),
                                          LED_LOAD_OF_MA(LED_CURRENT_LIMIT_SIDE_MA));
    // BRIGHTNESS_SHIFT is the sketch's own variable, the one sendColumnRGB() shifts by.
    // Only a chonky text with a ramp is ever shown dimmer than its own dimming.
    return ledBudgetScale(allowed, r, g, b, wave, wave, BRIGHTNESS_SHIFT, wave && AD_RAMP_FRAMES > 1);
}
#endif

// ---------------------------------------------------------------------------------------------
// 'O': the letters appear one at a time (CHARS_ONEBYONE_DELAY ms each), centred when the text fits.
// When the next letter does not fit any more, the text moves left a column at a time
// (AD_SCROLL_STEP_MS each) and the letter appears, whole, with the last column of the move: the
// newest letter is always at the right edge. The finished text then stays for delayMs: from the
// frame that shows the last letter to the return, delayMs of real time.
// ---------------------------------------------------------------------------------------------
void showCharsOneByOneOnBothPanels(const char* str, Color textColor, unsigned int delayMs) {
    const TextMetrics m = fontMetrics(fontStd);
    uint r = GAMMA(textColor.r);
    uint g = GAMMA(textColor.g);
    uint b = GAMMA(textColor.b);
#if AD_LIMITS
    {
        // the finished text (or the fullest stretch of a long one) must be within the supply's limits
        const uint scale = adBudgetScale(fontStd, m, str, r, g, b, false);
        r = ledScaled(r, scale);
        g = ledScaled(g, scale);
        b = ledScaled(b, scale);
    }
#endif
    uint startPad, endPad; // no padding when the text is wider than a panel
    textCentre(textCols(m, str), COLUMNS_PER_PANEL, &startPad, &endPad);

    clear();
    adClockStart();

    TextCursor left = textSeek(m, str, 0); // the text column at the left edge of the panel ...
    uint16_t offset = 0;                   // ... and its number
    uint16_t endCol = 0;                   // the columns of the letters shown so far, with the blank after each
    uint32_t due = 0;                      // when the next frame is to be drawn
    uint32_t complete = 0;                 // when the picture with the last letter appeared

    for (uint16_t p = 0; str[p]; p++) {
        endCol += textGlyphCols(m, str[p]) + 1;
        const uint16_t target = textOffsetToShow(endCol - 1, COLUMNS_PER_PANEL);
        uint stepMs = CHARS_ONEBYONE_DELAY;
        while (offset < target) {
            // make room; the new letter comes with the last column of the move
            offset++;
            textCursorNext(m, str, left);
            stepMs = AD_SCROLL_STEP_MS;
            if (offset < target) {
                adWaitUntil(due);
                adDrawFrame(fontStd, m, str, left, p, startPad, false, r, g, b);
                due += AD_TICKS(AD_SCROLL_STEP_MS);
            }
        }
        adWaitUntil(due);
        complete = adDrawFrame(fontStd, m, str, left, p + 1, startPad, false, r, g, b);
        due += AD_TICKS(stepMs);
    }
    adWaitUntil(complete + AD_TICKS(delayMs));
}

// ---------------------------------------------------------------------------------------------
// 'C': the chonky font with the colour wave. Red rises by 10 per column across the panel and the
// whole pattern moves one column per frame, a frame every AD_WAVE_FRAME_MS; green and blue are the
// message's.
// ---------------------------------------------------------------------------------------------

// Draws wave frames with the text's column `left` at the left edge. A text that reaches further
// than the panel by `scrollCols` columns stands still for AD_SCROLL_HOLD_MS, moves left one column
// every AD_SCROLL_STEP_MS until its end is at the right edge, and then stays for delayMs (the wave
// keeps running all the time). Otherwise it just stays for delayMs. From the first frame that
// shows the end of the text to the return is delayMs of real time; with delayMs 0 that frame is
// the last thing drawn.
// With `another`, the caller draws a different picture straight after this one (scrollCols is 0
// then). The return then comes delayMs after this call began, so that the next picture, which
// takes as long to send as this one did, appears delayMs after this one appeared.
// (With the supply's settings on: the text is dimmed by adTextScale and comes up to that over
// AD_RAMP_MS, counted from adRampBase. The caller sets both before a message's first call.)
static void adShowWave(const TextMetrics& m, const char* s, TextCursor left, uint16_t scrollCols,
                       unsigned int delayMs, uint g, uint b, bool another) {
    uint phase = (uint) delayMs; // where the wave starts, as it always has: the low byte of dly
    uint32_t due = 0;            // when the next frame is to be drawn
    uint32_t stepBase = 0;       // while scrolling: when the column we are on was moved to
    uint32_t endAt = 0;          // when the hold at the end is over
    uint sub = 0;                // while scrolling: which of the frames of this column comes next
    bool scrolling = false;
    bool atEnd = false;
#if AD_DIMS
    const uint fullG = g;
    const uint fullB = b;
#endif

    adClockStart();
    for (;;) {
        if (atEnd) {
            uint32_t start = adNow();
            if (start < due) {
                start = due;
            }
            if (start + AD_FRAME_TICKS > endAt) {
                adWaitUntil(endAt);
                return;
            }
        }
        adWaitUntil(due);
        if (scrolling && sub == 0) {
            textCursorNext(m, s, left);
            scrollCols--;
        }
#if AD_DIMS
        // this frame's brightness, worked out before the frame, not in it
        adWaveScale = adTextScale;
#if AD_RAMP_FRAMES > 1
        adWaveScale = ledRampScale(adTextScale, ledRampFrame(adRampBase + adNow(), AD_TICKS(AD_WAVE_FRAME_MS),
                                                             AD_RAMP_FRAMES), AD_RAMP_FRAMES);
#endif
        g = ledScaled(fullG, adWaveScale);
        b = ledScaled(fullB, adWaveScale);
#endif
        uint32_t sent = adDrawFrame(fontChonk, m, s, left, AD_SHOW_ALL, 0, true, phase, g, b);
        phase -= 10;
        if (!atEnd && scrollCols == 0) {
            if (!delayMs) {
                return;
            }
            atEnd = true;
            scrolling = false;
            endAt = (another ? 0 : sent) + AD_TICKS(delayMs);
        }
        if (scrolling) {
            sub++;
            if (sub == AD_SCROLL_SUBFRAMES) {
                sub = 0;
                stepBase += AD_TICKS(AD_SCROLL_STEP_MS);
            }
            due = stepBase + sub * AD_SCROLL_SUBFRAME_TICKS;
        } else {
            due += AD_TICKS(AD_WAVE_FRAME_MS);
            if (!atEnd && due >= AD_TICKS(AD_SCROLL_HOLD_MS)) {
                scrolling = true;
                sub = 0;
                due = AD_TICKS(AD_SCROLL_HOLD_MS);
                stepBase = due;
            }
        }
    }
}

// nudge (the message's "scroll"): move the text left by its first character, a column at a time,
// each column shown for delayMs (or for as long as sending a frame takes, 5 ms, if delayMs is less).
// The last position has the first character's last lit column gone and the blank column after it
// at the left edge: 9 positions for an ordinary character, 1 for a vertical tab or an empty text. This is what the app's letter-by-letter streaming asks for; it is not
// what makes a long text scroll (that happens by itself, when nudge is false).
void showChonkySlideStyleOnBothPanels(const char* str, unsigned int delayMs, Color colorFrom, bool nudge) {
    // TODO colorFrom, colorTo support (red is the wave)
    const TextMetrics m = fontMetrics(fontChonk);
    TextCursor left = textSeek(m, str, 0);
#if AD_LIMITS
    // green and blue are the message's, red is the wave: every value comes up somewhere along the text
    adTextScale = adBudgetScale(fontChonk, m, str, 0, colorFrom.g, colorFrom.b, true);
#endif
#if AD_DIMS
    adRampBase = 0; // the text comes on once per message
#endif
    if (nudge) {
        const uint last = left.glyphCols; // the first character's own width, not the font's
        for (uint skip = 0; skip <= last; skip++) {
            adShowWave(m, str, left, 0, delayMs, colorFrom.g, colorFrom.b, skip < last);
#if AD_DIMS
            adRampBase += adNow(); // how long this position lasted: the ramp goes on from there
#endif
            textCursorNext(m, str, left);
        }
    } else {
        adShowWave(m, str, left, textOffsetToShow(adInkCols(fontChonk, m, str), COLUMNS_PER_PANEL), delayMs,
                   colorFrom.g, colorFrom.b, false);
    }
}

#endif //LED_MARQUEE_ADDISPLAY_H
