//
// The chooser display (MSGTYPE_CHOOSER): "<i>/<n>" in dim red, one blank column, then a preview of
// message i. A preview that fits beside the count blinks. A longer one scrolls column by column
// (see ChooserScroll.h for the geometry).
//
// Contract VERSION 3: the app pushes, the sign does not poll.
//
// The sign cannot listen to the app and move a text at the same moment (interrupts are off while a
// frame goes out, and a conversation takes as long as the app and the link take). So while a text
// moves the sign says nothing at all. Instead:
//
//  - The app raises its hand. When what the chooser should show changes, it holds the serial line
//    LOW for 50 ms (a break) and then, 20 ms later and unasked, sends the message. The sign looks
//    at the LEVEL of its receive pin between frames and all through every wait. That costs
//    microseconds. LOW means attention: stop where we are, wait for the line to be HIGH again,
//    listen, read one message (chooserAttention).
//  - Underneath is a read of the whole message with '~', but only where a wait cannot be seen:
//    with a scrolling text standing still on its start, each time it has come round; and for a
//    text that fits, every CHOOSER_REFRESH_MS at the brightest frame of its blink (chooserSync).
//    So a push that was lost, or an app that does not push at all, is put right at the next
//    stand-still, and a text is never out of date for longer than once round.
//  - Whatever was read and is not a whole message is not shown. The sign then asks the normal way
//    (chooserFallback); if that brings nothing either, showAsChooser() returns and loop() asks.
//
// The sign sends no '?' any more. An object without a "type" is never a message, here or in loop().
//
// Only panel 1 (the 60 columns nearest the data pins) is ever sent: the LEDs keep their colour
// until told otherwise, so panel 2 is blanked once, when a message is stored, and not touched again.
//
// This file is part of the sketch: LED_Marquee.ino includes it after everything it uses (the
// CHOOSER_* settings at the top of the sketch, chooserLineLow, sendColumnRGB, clear, show,
// startSerial, stopSerial, softSerial, json, fontStd, GAMMA). test/chooser_display_test.cpp includes
// it after fakes of the same names, to run it on the host with a virtual clock and a scripted app.
//

#ifndef LED_MARQUEE_CHOOSERDISPLAY_H
#define LED_MARQUEE_CHOOSERDISPLAY_H

#include "ChooserScroll.h"

// A blink of a preview that fits is this many frames, from the brightest to black
#define CHOOSER_BLINK_FRAMES 58

// Our own copy of what is on screen. The global `json` is overwritten by every read.
struct ChooserState {
    char text[CHOOSER_MAX_CHARS + 1];
    char count[CHOOSER_COUNT_MAX_CHARS + 1];
    ChooserLayout layout;
    uint16_t pos;              // scroll position in columns; 0 = the start of the text at the left edge
    bool onScreen;             // the picture on the LEDs is still ours (nothing else was drawn since)
    bool holding;              // a scrolling text is standing still on its start
    bool syncDue;              // it has come round: this stand-still reads the whole message
    uint blinkCount;           // a text that fits: which frame of its blink is on the LEDs (the last one is 0, black)
    uint failedReads;          // reads in a row that brought nothing usable
    unsigned long holdStartUs; // when the stand-still began (micros)
    unsigned long fullReadMs;  // when the whole message was last read (millis)
};

static ChooserState chooser;

// loop() calls this before it draws anything that is not the chooser.
static void chooserLeft() {
    chooser.onScreen = false;
}

// What a read brought
#define CHOOSER_REPLY_SAME 0    // the chooser message we are showing: carry on from where we are
#define CHOOSER_REPLY_CHANGED 1 // a different chooser message: stored and drawn at its start
#define CHOOSER_REPLY_OTHER 2   // some other message is in `json`: leave the chooser display
#define CHOOSER_REPLY_FAILED 3  // nothing usable: nothing in time, not JSON, or an object without a type

// How many objects without a type loop()'s normal read steps over while it waits for a message
#define CHOOSER_MAX_SKIPPED 3

#if CHOOSER_DEBUG_PANEL_B
// The Uno cannot print (its USB serial pin drives an LED row), so the build with this flag shows
// what it measured at the last attention on panel 2, as four numbers:
//   green  ms from the line seen LOW to the first byte of the message ('{')
//   blue   ms from the line seen LOW to the end of the message
//   white  how many times the line was seen LOW (two digits, counts round)
//   red    how many of those ended in asking with '~' because no whole message followed
// They are sent with the first frame after they change, which is the only frame longer than
// panel 1; that frame is always one of a text standing still.
struct ChooserDebug {
    unsigned int firstByteMs, endMs;
    uint attentions, fallbacks;
    bool stale; // panel 2 does not show these values yet
};

static ChooserDebug chooserDebug;

// Writes value as `digits` decimal digits (the highest value that fits if it is larger).
static void chooserDigits(char* out, unsigned int value, uint digits) {
    unsigned int limit = digits == 3 ? 999 : 99;
    if (value > limit) {
        value = limit;
    }
    out[digits] = '\0';
    while (digits--) {
        out[digits] = '0' + value % 10;
        value /= 10;
    }
}
#endif

// Sends n columns of the string, starting at the cursor. Blank columns are sent as black.
// Call with interrupts off.
static void chooserSendCols(const char* s, ChooserCursor cur, uint gapCols, uint n, uint r, uint g, uint b) {
    while (n--) {
        uint row = 0;
        if (chooserCursorLit(cur)) {
            uint ch = s[cur.index] - ' ';
            if (ch <= HEART - ' ') { // a byte outside the font is drawn as a space
                row = pgm_read_byte_near(fontStd.data + ch * FONTSTD_WIDTH + cur.col);
            }
        }
        sendColumnRGB(row, r, g, b);
        chooserCursorNext(s, gapCols, cur);
    }
}

// Draws the count, the blank column and the text at the current scroll position.
// Never more than 60 columns: panel 2 is not touched.
static void chooserDrawFrame(uint brightness) {
    const ChooserLayout& l = chooser.layout;
    // A scrolling text always fills the viewport. One that fits is sent just as wide as it is
    // (the columns to its right are already dark).
    uint textCols = l.viewCols;
    if (l.textCols < l.viewCols) {
        textCols = l.textCols;
    }

    // Finding the starting column walks the strings: do it before the pixel timing starts
    ChooserCursor countCur = chooserSeek(chooser.count, CHOOSER_PANEL_COLS, 0);
    ChooserCursor textCur = chooserSeek(chooser.text, CHOOSER_GAP_COLS, chooser.pos);
#if CHOOSER_DEBUG_PANEL_B
    char first[4], end[4], attentions[3], fallbacks[3];
    bool numbers = chooserDebug.stale;
    chooserDebug.stale = false;
    chooserDigits(first, chooserDebug.firstByteMs, 3);
    chooserDigits(end, chooserDebug.endMs, 3);
    chooserDigits(attentions, chooserDebug.attentions % 100, 2);
    chooserDigits(fallbacks, chooserDebug.fallbacks % 100, 2);
    ChooserCursor digitCur = chooserSeek(first, CHOOSER_PANEL_COLS, 0); // every number starts the same way
#endif

    cli();
    chooserSendCols(chooser.count, countCur, CHOOSER_PANEL_COLS, l.countCols, 0x80, 0, 0);
    if (l.countCols < CHOOSER_PANEL_COLS) {
        sendColumnRGB(0x00, 0, 0, 0);
    }
    chooserSendCols(chooser.text, textCur, CHOOSER_GAP_COLS, textCols, brightness, brightness, brightness);
#if CHOOSER_DEBUG_PANEL_B
    if (numbers) {
        // the rest of panel 1 as it is (dark), then panel 2: 17 + 1 + 17 + 1 + 11 + 1 + 11 columns
        uint sent = l.countCols + textCols;
        if (l.countCols < CHOOSER_PANEL_COLS) {
            sent++;
        }
        for (; sent < CHOOSER_PANEL_COLS; sent++) {
            sendColumnRGB(0x00, 0, 0, 0);
        }
        chooserSendCols(first, digitCur, CHOOSER_PANEL_COLS, 18, 0, 0x40, 0);
        chooserSendCols(end, digitCur, CHOOSER_PANEL_COLS, 18, 0, 0, 0x40);
        chooserSendCols(attentions, digitCur, CHOOSER_PANEL_COLS, 12, 0x20, 0x20, 0x20);
        chooserSendCols(fallbacks, digitCur, CHOOSER_PANEL_COLS, 11, 0x40, 0, 0);
    }
#endif
    sei();

    show();
}

// Brightness of frame `count` of the blink: 145 down to 0, as it always was
static uint chooserBlinkLevel(uint count) {
    return GAMMA(((unsigned int) count * 256) / 100);
}

// The whole message has just been read and compared with what is shown.
static void chooserFullReadDone() {
    chooser.fullReadMs = millis();
    chooser.syncDue = false;
}

// Puts the stored message on the LEDs from its start: a scrolling text standing still on its
// first characters, a text that fits at the brightest frame of its blink.
static void chooserRestart() {
    chooser.pos = 0;
    chooser.holding = true;
    chooser.blinkCount = CHOOSER_BLINK_FRAMES - 1;
    chooserDrawFrame(chooserScrolls(chooser.layout) ? GAMMA(CHOOSER_TEXT_BRIGHTNESS)
                                                    : chooserBlinkLevel(CHOOSER_BLINK_FRAMES - 1));
    chooser.holdStartUs = micros();
}

// Takes a new chooser message: keeps a copy, blanks the strip (this is the one time panel 2 is
// sent) and shows the message from its start.
static void chooserStore(const char* text, const char* count) {
    chooserCopy(chooser.text, text, CHOOSER_MAX_CHARS);
    chooserCopy(chooser.count, count, CHOOSER_COUNT_MAX_CHARS);
    chooser.layout = chooserLayout(chooser.count, chooser.text, CHOOSER_GAP_COLS);
    clear();
#if CHOOSER_DEBUG_PANEL_B
    chooserDebug.stale = true; // clear() wiped the numbers too
#endif
    chooserRestart();
}

// Waits until `us` microseconds have passed since `sinceUs`, watching the receive line all the
// time. Returns true at once if the line is LOW (attention). Looks at the line at least once.
static bool chooserWait(unsigned long sinceUs, unsigned long us) {
    do {
        if (chooserLineLow()) {
            return true;
        }
    } while (micros() - sinceUs < us);
    return false;
}

// Waits until the line has been HIGH for CHOOSER_LINE_QUIET_MS without a gap: a break is over, a
// run of bytes has ended (one byte is 0.7 ms and has a HIGH stop bit in it, so a single look at
// the pin would not do). Gives up after maxMs.
static void chooserAwaitQuietLine(unsigned int maxMs) {
    unsigned long startMs = millis();
    unsigned long highSinceMs = startMs;
    for (;;) {
        unsigned long nowMs = millis();
        if (chooserLineLow()) {
            highSinceMs = nowMs;
        } else if (nowMs - highSinceMs >= CHOOSER_LINE_QUIET_MS) {
            return;
        }
        if (nowMs - startMs >= maxMs) {
            return;
        }
    }
}

// Waits up to maxMs for the '{' that opens a message to be the next byte in the receive buffer.
// Anything in front of it is thrown away: what a break was received as, the tail of bytes that
// were already under way when we began to listen.
static bool chooserAwaitObject(unsigned int maxMs) {
    unsigned long startMs = millis();
    do {
        int c = softSerial.peek();
        if (c == '{') {
            return true;
        }
        if (c >= 0) {
            softSerial.read();
        }
    } while (millis() - startMs < maxMs);
    return false;
}

// What is in `json` after a read, and what it does to the display. A different chooser message
// is stored and drawn here.
static uint chooserTake(bool readOk) {
    if (!readOk) {
        return CHOOSER_REPLY_FAILED;
    }
    const char* type = json["type"];
    if (!type) {
        // An object without a type is not a message, whatever else is in it. It is not believed.
        return CHOOSER_REPLY_FAILED;
    }
    chooser.failedReads = 0;
    if (type[0] != MSGTYPE_CHOOSER) {
        return CHOOSER_REPLY_OTHER;
    }
    chooserFullReadDone();
    const char* text = json["str"];
    const char* count = json["flashy"];
    if (chooserSame(chooser.text, text, CHOOSER_MAX_CHARS) &&
        chooserSame(chooser.count, count, CHOOSER_COUNT_MAX_CHARS)) {
        return CHOOSER_REPLY_SAME;
    }
    chooserStore(text, count);
    return CHOOSER_REPLY_CHANGED;
}

// The read of a normal '~' request, loop()'s and ours (the '~' has just been sent).
// An object without a type is not a message. It is stepped over and the read goes on in the same
// window, because the real answer may be on its way behind it. (Asking again at once would start
// listening in the middle of that answer and read its tail as garbage.)
static DeserializationError chooserReadMessage() {
    DeserializationError err = deserializeJson(json, softSerial);
    for (uint skipped = 0;
         skipped < CHOOSER_MAX_SKIPPED && err == DeserializationError::Ok && !json["type"].is<const char*>();
         skipped++) {
        err = deserializeJson(json, softSerial);
    }
    return err;
}

// Asks for the whole message the normal way and reads the answer into `json`, with the normal
// read's patience (a second per byte). No error flash and no error text on the wire.
// Only ever called with the text standing still or already stopped.
static bool chooserAsk() {
    startSerial(); // we stopped listening after the last read, so this starts with an empty buffer
    softSerial.print('~');
    DeserializationError err = chooserReadMessage();
    stopSerial();
    return err == DeserializationError::Ok;
}

// True when this is the moment to read the whole message again: where a wait cannot be seen.
static bool chooserSyncIsDue() {
    if (chooserScrolls(chooser.layout)) {
        // standing still on its start, having come round since the last read
        return chooser.holding && chooser.syncDue;
    }
    // a text that fits: at the brightest frame of a blink, when the last read is long enough ago
    return chooser.blinkCount == CHOOSER_BLINK_FRAMES - 1 && millis() - chooser.fullReadMs >= CHOOSER_REFRESH_MS;
}

// The read underneath the pushes: the whole message, compared with what is shown.
static uint chooserSync() {
    chooser.syncDue = false; // one try per stand-still
    return chooserTake(chooserAsk());
}

// Nothing usable was read: ask the normal way. First let whatever is still coming in go by, or
// the '~' would be answered into the tail of it.
static uint chooserFallback() {
#if CHOOSER_DEBUG_PANEL_B
    chooserDebug.fallbacks++;
    chooserDebug.stale = true;
#endif
    chooserAwaitQuietLine(CHOOSER_FALLBACK_QUIET_WAIT_MS);
    return chooserTake(chooserAsk());
}

// The receive line was seen LOW: the app has something to say and will say it unasked.
// Waits for the line to go HIGH and stay HIGH (the break is over), listens, and reads one message.
// A chooser message is shown from its start, whether or not it is the one already showing.
static uint chooserAttention() {
#if CHOOSER_DEBUG_PANEL_B
    unsigned long seenMs = millis();
    chooserDebug.attentions++;
    chooserDebug.stale = true;
#endif
    chooserAwaitQuietLine(CHOOSER_ATTN_HIGH_WAIT_MS);
    startSerial(); // begins with an empty buffer: nothing of the break is in it
    bool readOk = false;
    if (chooserAwaitObject(CHOOSER_ATTN_FIRST_BYTE_MS)) {
#if CHOOSER_DEBUG_PANEL_B
        chooserDebug.firstByteMs = (unsigned int) (millis() - seenMs);
#endif
        softSerial.setTimeout(CHOOSER_ATTN_BYTE_MS);
        readOk = deserializeJson(json, softSerial) == DeserializationError::Ok;
        softSerial.setTimeout(1000); // the Stream default, for the normal '~' read
#if CHOOSER_DEBUG_PANEL_B
        chooserDebug.endMs = (unsigned int) (millis() - seenMs);
#endif
    }
    stopSerial();
    uint reply = chooserTake(readOk);
    if (reply == CHOOSER_REPLY_SAME) {
        chooserRestart();
        reply = CHOOSER_REPLY_CHANGED;
    }
    return reply;
}

// Shows a chooser message and stays here for as long as the chooser is on screen. Returns true
// when `json` holds a message of another type for loop() to show, false when nothing usable could
// be read (loop() then asks with its own '~'; if that brings the same chooser message, we carry on
// from where we were).
//
// Every time round this loop the receive line is looked at first, and every wait in it watches
// the line, so the longest the line goes unwatched is one frame (or, once per message, the
// blanking of the strip and a frame): a few milliseconds. No byte is sent from here while a text
// is moving, except after an attention that was not followed by a whole message.
bool showAsChooser(const char* blinkyStr, const char* countyStr) {
    chooser.failedReads = 0;
    if (!(chooser.onScreen &&
          chooserSame(chooser.text, blinkyStr, CHOOSER_MAX_CHARS) &&
          chooserSame(chooser.count, countyStr, CHOOSER_COUNT_MAX_CHARS))) {
        chooserStore(blinkyStr, countyStr);
    }
    chooser.onScreen = true;
    chooserFullReadDone(); // loop()'s read has just brought this message, new or the same

    for (;;) {
        uint reply = CHOOSER_REPLY_SAME;
        if (chooserLineLow()) {
            reply = chooserAttention();
        } else if (chooserSyncIsDue()) {
            reply = chooserSync();
        }
        if (reply == CHOOSER_REPLY_FAILED) {
            chooser.failedReads++;
            if (chooser.failedReads >= CHOOSER_MAX_FAILED_READS) {
                return false; // a line that stays LOW, an app that is gone
            }
            if (chooserLineLow()) {
                continue; // the app raised its hand while we were reading: that comes first
            }
            reply = chooserFallback();
            if (reply == CHOOSER_REPLY_FAILED) {
                return false;
            }
        }
        if (reply == CHOOSER_REPLY_OTHER) {
            return true;
        }
        if (reply == CHOOSER_REPLY_CHANGED) {
            continue; // already drawn at its start
        }

        if (!chooserScrolls(chooser.layout)) {
            // Fits: the next frame of the blink, one straight after the other
            chooser.blinkCount = chooser.blinkCount ? chooser.blinkCount - 1 : CHOOSER_BLINK_FRAMES - 1;
            chooserDrawFrame(chooserBlinkLevel(chooser.blinkCount));
        } else if (chooser.holding) {
            // Standing still on the start of the text (the frame is already on the LEDs). The
            // time a read took is part of the stand-still, not added to it.
            if (!chooserWait(chooser.holdStartUs, CHOOSER_HOLD_MS * 1000UL)) {
                chooser.holding = false;
            }
        } else {
            // One column to the left
            unsigned long stepStartUs = micros();
            chooser.pos = chooserNextPos(chooser.layout, chooser.pos);
            chooserDrawFrame(GAMMA(CHOOSER_TEXT_BRIGHTNESS));
            if (chooser.pos == 0) {
                // The second copy has reached the left edge: same picture as the start. Stand
                // still again; this is where reading the whole message costs nothing visible.
                chooser.holding = true;
                chooser.syncDue = true;
                chooser.holdStartUs = micros();
                continue;
            }
            chooserWait(stepStartUs, CHOOSER_SCROLL_DELAY_MS * 1000UL);
        }
    }
}

// What loop() does with a reply it has read into `json`. Returns the type character of the message
// to show now, or 0 when there is nothing to show: draw nothing and ask again.
//  - no type: not a message, whatever else is in it
//  - a chooser message: shown here, for as long as the chooser is on screen; the message that
//    ended it is then in `json` (0 if instead nothing usable could be read any more)
static char chooserDispatch() {
    const char* type = json["type"];
    if (!type) {
        return 0;
    }
    if (type[0] == MSGTYPE_CHOOSER) {
        if (!showAsChooser(json["str"], json["flashy"])) {
            return 0;
        }
        type = json["type"]; // chooserTake() only lets a message with a type end the display
    }
    chooserLeft(); // whatever loop() draws now replaces the chooser picture
    return type[0];
}

#endif //LED_MARQUEE_CHOOSERDISPLAY_H
