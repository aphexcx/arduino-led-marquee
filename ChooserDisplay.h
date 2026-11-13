//
// The chooser display (MSGTYPE_CHOOSER): "<i>/<n>" in dim red, one blank column, then a preview of
// message i. A preview that fits beside the count blinks as it always did. A longer one scrolls
// column by column (see ChooserScroll.h for the geometry).
//
// The sign owns the scroll: the app sends the text once and, while the chooser is on screen, is
// asked '?' ("anything changed?") instead of '~'. It answers "{}" when nothing has.
//
// This file is part of the sketch: LED_Marquee.ino includes it after everything it uses (the
// CHOOSER_* settings at the top of the sketch, sendColumnRGB, clear, show, startSerial, stopSerial,
// softSerial, json, fontStd, GAMMA). test/chooser_display_test.cpp includes it after fakes of the
// same names, to run it on the host with a virtual clock and a scripted app.
//

#ifndef LED_MARQUEE_CHOOSERDISPLAY_H
#define LED_MARQUEE_CHOOSERDISPLAY_H

#include "ChooserScroll.h"

// Our own copy of what is on screen. The global `json` is overwritten by every read.
struct ChooserState {
    char text[CHOOSER_MAX_CHARS + 1];
    char count[CHOOSER_COUNT_MAX_CHARS + 1];
    ChooserLayout layout;
    uint16_t pos;             // scroll position in columns; 0 = the start of the text at the left edge
    uint16_t holdLeftMs;      // how much longer the text stands still before it scrolls
    uint failedPolls;         // unanswered polls in a row
};

static ChooserState chooser;

// What the app's answer to a poll means
#define CHOOSER_REPLY_SAME 0    // nothing changed (or nothing usable arrived): carry on from where we are
#define CHOOSER_REPLY_CHANGED 1 // a different chooser message: stored, start again
#define CHOOSER_REPLY_OTHER 2   // some other message is in `json`: leave the chooser display
#define CHOOSER_REPLY_GAVE_UP 3 // the app stopped answering

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
    // A scrolling text always fills the viewport. One that fits is sent just as wide as it is, as
    // before, so its blink keeps its speed (the columns to its right are already dark).
    uint textCols = l.viewCols;
    if (l.textCols < l.viewCols) {
        textCols = l.textCols;
    }

    // Finding the starting column walks the strings: do it before the pixel timing starts
    ChooserCursor countCur = chooserSeek(chooser.count, CHOOSER_PANEL_COLS, 0);
    ChooserCursor textCur = chooserSeek(chooser.text, CHOOSER_GAP_COLS, chooser.pos);

    cli();
    chooserSendCols(chooser.count, countCur, CHOOSER_PANEL_COLS, l.countCols, 0x80, 0, 0);
    if (l.countCols < CHOOSER_PANEL_COLS) {
        sendColumnRGB(0x00, 0, 0, 0);
    }
    chooserSendCols(chooser.text, textCur, CHOOSER_GAP_COLS, textCols, brightness, brightness, brightness);
    sei();

    show();
}

// Takes a new chooser message: keeps a copy and puts the display at the start (the hold).
static void chooserStore(const char* text, const char* count) {
    chooserCopy(chooser.text, text, CHOOSER_MAX_CHARS);
    chooserCopy(chooser.count, count, CHOOSER_COUNT_MAX_CHARS);
    chooser.layout = chooserLayout(chooser.count, chooser.text, CHOOSER_GAP_COLS);
    chooser.pos = 0;
    chooser.holdLeftMs = CHOOSER_HOLD_MS;
    if (chooserScrolls(chooser.layout)) {
        clear();
        chooserDrawFrame(GAMMA(CHOOSER_TEXT_BRIGHTNESS));
    }
}

// One blink of a preview that fits: the text fades to black, exactly as before scrolling existed.
static void chooserBlink() {
    unsigned int count = 58;

    clear();
    while (count > 0) {
        count--;
        chooserDrawFrame(GAMMA(((count % 100) * 256) / 100));
    }
}

// Sends one request byte and reads the answer into `json`, waiting only a short time for it.
// No error flash and no error text on the wire: a missed poll must not disturb the scroll.
static bool chooserRequest(char request) {
    startSerial(); // we stopped listening after the last read, so this starts with an empty buffer
    softSerial.setTimeout(CHOOSER_POLL_TIMEOUT_MS);
    softSerial.print(request);
    DeserializationError err = deserializeJson(json, softSerial);
    softSerial.setTimeout(1000); // the Stream default, for the normal '~' read
    stopSerial();
    return err == DeserializationError::Ok;
}

// Asks the app whether anything changed and takes in its answer.
static uint chooserPoll() {
    // '?' is answered with "{}" while the app has nothing new. But if the last answer was lost on
    // the way, the app believes we have its newest message and would keep saying "{}": so after a
    // failed poll ask for the whole message ('~') until one read works again.
    bool ok = chooserRequest(chooser.failedPolls ? '~' : '?');

    const char* type = json["type"];
    if (ok && !type) {
        // "{}" means nothing changed. Any other object without a type is a damaged message.
        ok = json.is<JsonObject>() && json.size() == 0;
    }
    if (!ok) {
        chooser.failedPolls++;
        return chooser.failedPolls >= CHOOSER_MAX_FAILED_POLLS ? CHOOSER_REPLY_GAVE_UP : CHOOSER_REPLY_SAME;
    }
    chooser.failedPolls = 0;

    if (!type) {
        return CHOOSER_REPLY_SAME;
    }
    if (type[0] != MSGTYPE_CHOOSER) {
        return CHOOSER_REPLY_OTHER;
    }
    const char* text = json["str"];
    const char* count = json["flashy"];
    if (chooserSame(chooser.text, text, CHOOSER_MAX_CHARS) &&
        chooserSame(chooser.count, count, CHOOSER_COUNT_MAX_CHARS)) {
        // an app that does not know '?' answers with the whole message every time
        return CHOOSER_REPLY_SAME;
    }
    chooserStore(text, count);
    return CHOOSER_REPLY_CHANGED;
}

// Shows a chooser message and stays here, polling the app between frames, for as long as the
// chooser is on screen. Returns true when `json` holds a message of another type for loop() to
// show, false when the app stopped answering (loop() then asks with a normal '~').
bool showAsChooser(const char* blinkyStr, const char* countyStr) {
    uint colsSincePoll = 0;
    unsigned long lastPollMs = millis();

    chooser.failedPolls = 0;
    chooserStore(blinkyStr, countyStr);

    for (;;) {
        unsigned long sliceStart = millis();
        unsigned int sliceMs = 0; // how long this step lasts, the poll included
        bool poll = true;

        if (!chooserScrolls(chooser.layout)) {
            // Fits: one blink, then ask
            chooserBlink();
        } else if (chooser.holdLeftMs) {
            // Standing still on the start of the text (the frame is already on the LEDs)
            sliceMs = min(chooser.holdLeftMs, CHOOSER_POLL_EVERY_MS);
            chooser.holdLeftMs -= sliceMs;
        } else {
            // One column to the left
            chooser.pos = chooserNextPos(chooser.layout, chooser.pos);
            chooserDrawFrame(GAMMA(CHOOSER_TEXT_BRIGHTNESS));
            if (chooser.pos == 0) {
                // The second copy has reached the left edge: same picture as the start, hold again
                chooser.holdLeftMs = CHOOSER_HOLD_MS;
                continue;
            }
            sliceMs = CHOOSER_SCROLL_DELAY_MS;
            colsSincePoll++;
            // ... and never leave the app unasked for long (the first columns after a hold, a slow scroll)
            poll = colsSincePoll >= CHOOSER_POLL_EVERY_COLS || millis() - lastPollMs >= CHOOSER_POLL_EVERY_MS;
        }

        if (poll) {
            colsSincePoll = 0;
            lastPollMs = millis();
            uint reply = chooserPoll();
            if (reply == CHOOSER_REPLY_OTHER) {
                return true;
            }
            if (reply == CHOOSER_REPLY_GAVE_UP) {
                return false;
            }
            if (reply == CHOOSER_REPLY_CHANGED) {
                continue; // already drawn at its start by chooserStore()
            }
        }

        // The poll's time comes out of this step, so an answer inside one frame delay causes no hitch
        while (millis() - sliceStart < sliceMs) {
        }
    }
}

#endif //LED_MARQUEE_CHOOSERDISPLAY_H
