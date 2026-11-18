//
// The chooser display (MSGTYPE_CHOOSER): "<i>/<n>" in dim red, one blank column, then a preview of
// message i. A preview that fits beside the count blinks. A longer one scrolls column by column
// (see ChooserScroll.h for the geometry).
//
// Contract VERSION 3.1: the app pushes, the sign does not poll.
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
//    text that fits, every CHOOSER_REFRESH_MS in the dark pause between two blinks (chooserSync).
//    So a push that was lost, or an app that does not push at all, is put right at the next
//    stand-still, and a text is never out of date for longer than once round.
//  - Whatever was read and is not a whole message is not shown. The sign then asks the normal way
//    (chooserFallback); if that brings nothing either, showAsChooser() returns and loop() asks.
//  - That is also how the chooser ends. When the app leaves chooser mode it raises its hand and
//    sends nothing: the sign waits CHOOSER_ATTN_FIRST_BYTE_MS, asks, and is given the next message.
//  - A break that falls while the sign is already listening for the answer to a '~' (its own, or
//    loop()'s) is received as a 0x00 byte. That byte is stepped over and the read goes on: what
//    the app sends next is the answer (chooserAwaitAnswer).
//  - No '~' is sent, by the chooser or by loop(), into something the app is sending or is about
//    to send: the line must be quiet first, and after a LOW the app is given the time its message
//    needs to begin (chooserRequest).
//  - A frame is whole or it is not a frame. A message opens with a brace and a double quote
//    (chooserOpensMessage). What arrives without having been asked for is taken only as a
//    complete object with a type. Anything less, and an "answer" that does not open like a
//    message or cannot be read to its end, is the rest of something the sign did not hear the
//    start of: it is let by, and then the sign asks again (chooserExchange). It is an error on
//    the sign only when three requests in a row end that way.
//  - A request that has gone out stands. Whether something of the app's began while it went
//    out is known, not guessed (the receive pin's change flag at the end of the sign's own
//    byte: chooserSendByte, in the sketch). If so its start was missed: the sign listens again
//    once the line has paused, and reads the answer to the request it sent. That something
//    crossed the request is never by itself a reason to ask again; a second request would take
//    a second message from the app's list (chooserRequest).
//  - The first LOW the sign sees may be a bit of a message, not a break: the sign's own app
//    raises its hand with a run of 0x00 bytes, and a break can be too short to see. A run of
//    zeros is waited out like a break. A message that is already going by is let by, all of it,
//    and then the sign asks, once (attentionRead).
//
// Two of the steps here do not know about the chooser and can be used by any display loop that
// wants to hear the app while it draws: chooserLineLow() (one look at the pin, a few microseconds,
// between two frames) and attentionRead() (the line was seen LOW: wait for it to be HIGH, listen,
// read one message into the document it is given; true if one came). What the message means is
// the caller's business. (A loop whose text lives in `json`, as the marquee's does, has to give
// it another document, or its text is gone.)
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
    bool holding;              // a scrolling text is standing still on its start
    bool syncDue;              // it has come round: this stand-still reads the whole message
    uint blinkCount;           // a text that fits: which frame of its blink is on the LEDs (the last one is 0, black)
    uint failedReads;          // reads in a row that brought nothing usable
    unsigned long holdStartUs; // when the stand-still (or the dark pause of the blink) began (micros)
    unsigned long fullReadMs;  // when the whole message was last read (millis)
};

static ChooserState chooser;

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
// (leaving chooser mode is one of those, by design: attention and no message)
// They are sent with the first frame after they change, which is the only frame longer than
// panel 1 (119 columns, about 2 ms more). After a pushed message that is a frame of a text
// standing still; after a fallback that brought the same message again it is the next scrolled one.
//
// Nobody may be standing at the sign, so the same build also tells the app, which logs what it
// reads. The next time a '~' is due anyway, one line of text goes out in front of it:
//
//     #SSS fFFF eEEE aAA bBB lLLL~
//
//   SSS  a count of these lines, 000 to 999 and round again (a missing number is a lost line)
//   FFF  ms from the line seen LOW to the first byte of the message (as on panel 2, green)
//   EEE  ms from the line seen LOW to the end of the message (blue)
//   AA   attentions so far, BB fallbacks so far (white, red)
//   LLL  ms the line was still LOW after the sign first saw it LOW, rounded up (001: it was HIGH
//        again at once, so it was a byte, not a break); 000: it did not go HIGH within
//        CHOOSER_ATTN_HIGH_WAIT_MS
// 27 characters, none of them a request byte, and then the '~' that was due: the app answers it
// as it answers any request. The line carries the numbers of the latest attention; attentions
// that needed no request in between are counted in AA.
struct ChooserDebug {
    unsigned int firstByteMs, endMs, lowMs, lines;
    uint attentions, fallbacks; // 0 to 99, then from 0 again
    bool stale;     // panel 2 does not show these values yet
    bool lineStale; // the app has not been sent them yet
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

// One more on a counter that is shown as two digits
static void chooserCount(uint& counter) {
    counter = counter >= 99 ? 0 : counter + 1;
    chooserDebug.stale = true;
    chooserDebug.lineStale = true;
}

// Sends the numbers to the app as one line of text, if it has not had them. The caller sends
// the '~' that ends it. Call only while listening has just begun and a '~' is due.
// Returns true if the line from the app changed while this one went out (see chooserSendByte).
// The line is sent whole all the same, and the caller sends its '~' behind it and then deals
// with what crossed, as for a '~' alone.
static bool chooserDebugLine() {
    if (!chooserDebug.lineStale) {
        return false;
    }
    char line[28];
    memcpy_P(line, PSTR("#000 f000 e000 a00 b00 l000"), sizeof(line));
    chooserDigits(line + 1, chooserDebug.lines, 3);
    chooserDigits(line + 6, chooserDebug.firstByteMs, 3);
    chooserDigits(line + 11, chooserDebug.endMs, 3);
    chooserDigits(line + 16, chooserDebug.attentions, 2);
    chooserDigits(line + 20, chooserDebug.fallbacks, 2);
    chooserDigits(line + 24, chooserDebug.lowMs, 3);
    line[4] = line[9] = line[14] = line[18] = line[22] = ' '; // (chooserDigits ends each number with a 0 byte)
    line[27] = '\0';
    bool crossed = false;
    for (const char* c = line; *c; c++) {
        if (chooserSendByte(*c)) {
            crossed = true;
        }
    }
    chooserDebug.lineStale = false;
    chooserDebug.lines = chooserDebug.lines >= 999 ? 0 : chooserDebug.lines + 1;
    return crossed;
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
    ChooserCursor digitCur = countCur;
    if (numbers) { // (not on every frame: the divisions would slow the blink)
        chooserDebug.stale = false;
        chooserDigits(first, chooserDebug.firstByteMs, 3);
        chooserDigits(end, chooserDebug.endMs, 3);
        chooserDigits(attentions, chooserDebug.attentions, 2);
        chooserDigits(fallbacks, chooserDebug.fallbacks, 2);
        digitCur = chooserSeek(first, CHOOSER_PANEL_COLS, 0); // every number starts the same way
    }
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
// first characters, a text that fits at the first and brightest frame of its blink.
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

// AFTER A LOW THE SIGN OWES THE APP THE TIME ITS MESSAGE NEEDS TO BEGIN. A LOW on the line is the
// app raising its hand (or already sending): what follows is a message nobody asked for, up to
// CHOOSER_ATTN_FIRST_BYTE_MS later. Until that time has passed in silence the sign sends nothing,
// on any path, because sending makes it deaf (SoftwareSerial transmits with interrupts off) and
// the first byte of that message would be lost. So every look at the line goes through
// lineSeenLow(), which remembers when it last was LOW, and chooserRequest() honours it.
static unsigned long lineLowAtMs; // when the line was last seen LOW (millis)
static bool lineWasLow;           // ... if it ever was

static bool lineSeenLow() {
    if (chooserLineLow()) {
        lineLowAtMs = millis();
        lineWasLow = true;
        return true;
    }
    return false;
}

// Waits until `us` microseconds have passed since `sinceUs`, watching the receive line all the
// time. Returns true at once if the line is LOW (attention). Looks at the line at least once.
static bool chooserWait(unsigned long sinceUs, unsigned long us) {
    do {
        if (lineSeenLow()) {
            return true;
        }
    } while (micros() - sinceUs < us);
    return false;
}

#define LINE_QUIET 0 // HIGH for quietMs without a gap: nothing is on its way
#define LINE_BREAK 1 // (only with stopAtBreak) the app is holding it LOW
#define LINE_BUSY 2  // gave up after maxMs: it is still carrying bytes
#define LINE_HELD 3  // gave up: LOW without a gap for maxLowMs

// Waits until the line has been HIGH for quietMs without a gap: a break is over, a run of
// bytes has ended (one byte is 0.7 ms and has a HIGH stop bit in it, so a single look at the
// pin would not do). Gives up after maxMs (LINE_BUSY: bytes are still arriving), or when the
// line has been LOW without a gap for maxLowMs (LINE_HELD: nothing is driving it, or it is held
// for good).
// With stopAtBreak it also returns, at once, when the line stays LOW for CHOOSER_BREAK_MIN_US:
// that is the app raising its hand, and it is left for the caller's next look at the line.
static uint chooserAwaitQuietLine(unsigned int maxMs, unsigned int maxLowMs, uint quietMs, bool stopAtBreak) {
    unsigned long startUs = micros();
    unsigned long highSinceUs = startUs;
    unsigned long lowSinceUs = startUs;
    bool wasLow = false;
    for (;;) {
        unsigned long nowUs = micros();
        if (lineSeenLow()) {
            if (!wasLow) {
                wasLow = true;
                lowSinceUs = nowUs;
            }
            if (stopAtBreak && nowUs - lowSinceUs >= CHOOSER_BREAK_MIN_US) {
                return LINE_BREAK;
            }
            if (nowUs - lowSinceUs >= maxLowMs * 1000UL) {
                return LINE_HELD;
            }
            highSinceUs = nowUs;
        } else {
            wasLow = false;
            if (nowUs - highSinceUs >= quietMs * 1000UL) {
                return LINE_QUIET;
            }
        }
        if (nowUs - startUs >= maxMs * 1000UL) {
            return LINE_BUSY;
        }
    }
}

// What may stand in front of a message without being part of it: the 0x00 a break is received
// as (and each of the zero bytes the app sends instead of a break), JSON white space (an app
// that ends what it writes with a newline), and 0xFF. That last one is what the receiver makes
// of a dip of the line too short to be a byte (it finds a start bit and then nothing but HIGH);
// no message contains it (JSON is ASCII or UTF-8, which has no 0xFF).
static bool chooserIsPadding(int c) {
    return c == 0 || c == 0xFF || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Takes the byte of padding at the front of the receive buffer. A 0x00 is the line having been
// LOW: the app raising its hand, received by the receiver instead of seen by a look at the pin.
// It counts as a LOW seen all the same (see lineSeenLow), or the sign would ask into the
// message that is about to follow it.
static void chooserTakePadding(int c) {
    softSerial.read();
    if (c == 0) {
        lineLowAtMs = millis();
        lineWasLow = true;
    }
}

// Waits up to maxMs for a pushed message to begin: the next byte in the receive buffer that is
// not padding. True if that byte is the '{' that opens it (it is left in the buffer). Anything
// else is not a message, whatever comes behind it (an object inside an array, the tail of bytes
// that were under way when we began to listen): false at once, and the caller asks.
static bool chooserAwaitObject(unsigned int maxMs) {
    unsigned long startMs = millis();
    do {
        int c = softSerial.peek();
        if (c == '{') {
            return true;
        }
        if (c >= 0) {
            if (!chooserIsPadding(c)) {
                return false;
            }
            chooserTakePadding(c);
        }
    } while (millis() - startMs < maxMs);
    return false;
}

// What ArduinoJson reads a message from: the serial line, with the opening brace in front again
// that chooserOpensMessage() took from it.
struct ChooserReader {
    bool brace; // the '{' has not been handed over yet

    int read() {
        if (brace) {
            brace = false;
            return '{';
        }
        char c;
        return softSerial.readBytes(&c, 1) ? (unsigned char) c : -1; // (waits, up to the timeout)
    }

    size_t readBytes(char* buffer, size_t length) {
        size_t n = 0;
        if (brace && length) {
            brace = false;
            buffer[n++] = '{';
        }
        return n + softSerial.readBytes(buffer + n, length - n);
    }
};

// A MESSAGE OPENS WITH A BRACE AND A DOUBLE QUOTE: `{"` (white space may stand between them;
// `{}` is let through as the object without a type that it is). The '{' is at the front of the
// receive buffer. Takes it and looks at what follows, waiting up to byteMs for it.
// False: this is not where a message begins. It is a '{' from the inside of one, the rest of
// something the sign did not hear the start of, and the caller lets it go by.
// The quote is what tells the two apart. ArduinoJson also reads keys in single quotes and
// without any, so the rest of a pushed message whose text is  x{type:'D',str:'...'}  would be a
// message of its own if a brace were enough. But inside a JSON string a double quote is always
// written \", so a brace inside a text is followed by a double quote only when the text ends
// there, and what follows that (`{","flashy":...`, `{"}`) is never an object.
// (This holds for messages whose values are texts and numbers, as all of the app's are. A value
// that is itself an object would open like a message.)
static bool chooserOpensMessage(unsigned int byteMs) {
    softSerial.read(); // the '{'
    unsigned long startMs = millis();
    do {
        int c = softSerial.peek();
        if (c == '"' || c == '}') {
            return true;
        }
        if (c >= 0) {
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                return false;
            }
            softSerial.read();
        }
    } while (millis() - startMs < byteMs);
    return false; // a brace and then nothing
}

// Reads the message that begins at the front of the receive buffer (the caller has seen its '{'
// there) into `into`, allowing byteMs for every byte after the first.
// InvalidInput also when it does not open like a message (chooserOpensMessage).
static DeserializationError chooserReadFrame(JsonDocument& into, unsigned int byteMs) {
    DeserializationError err = DeserializationError::InvalidInput;
    softSerial.setTimeout(byteMs);
    if (chooserOpensMessage(byteMs)) {
        ChooserReader reader = {true};
        err = deserializeJson(into, reader);
    }
    softSerial.setTimeout(1000); // the Stream default
    return err;
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

// Waits for the first byte of the answer to a '~', as long as a read waits for any byte.
// Padding in front of it is thrown away (chooserIsPadding: a 0x00, white space). That is what a break is received as when it falls while
// we are listening: the app raised its hand just as we asked, and what it sends next (if
// anything) is the answer. Without this the 0x00 would end the read as "no answer" while the
// answer is still on its way.
// A 0x00 that comes late in the wait is given the time a pushed message needs to follow it (the
// rest of the break, the guard), so the wait can outlast ANSWER_FIRST_BYTE_MS by that much, once.
static bool chooserAwaitAnswer() {
    unsigned long startMs = millis();
    unsigned long waitMs = ANSWER_FIRST_BYTE_MS;
    do {
        int c = softSerial.peek();
        if (c >= 0 && !chooserIsPadding(c)) {
            return true;
        }
        if (c >= 0) {
            chooserTakePadding(c);
        }
        if (c == 0) {
            unsigned long neededMs = (millis() - startMs) + ANSWER_AFTER_BREAK_MS;
            if (neededMs > ANSWER_FIRST_BYTE_MS + ANSWER_AFTER_BREAK_MS) {
                neededMs = ANSWER_FIRST_BYTE_MS + ANSWER_AFTER_BREAK_MS;
            }
            if (neededMs > waitMs) {
                waitMs = neededMs;
            }
        }
    } while (millis() - startMs < waitMs);
    return false;
}

// Reads one JSON object of an answer into `json`.
static DeserializationError chooserReadObject() {
    if (!chooserAwaitAnswer()) {
        return DeserializationError::EmptyInput;
    }
    if (softSerial.peek() != '{') {
        return DeserializationError::InvalidInput; // every message opens with a brace
    }
    return chooserReadFrame(json, ANSWER_FIRST_BYTE_MS);
}

// The read of a normal '~' request, loop()'s and ours (the '~' has just been sent).
// An object without a type is not a message. It is stepped over and the read goes on in the same
// window, because the real answer may be on its way behind it. (Asking again at once would start
// listening in the middle of that answer and read its tail as garbage.)
static DeserializationError chooserReadMessage() {
    DeserializationError err = chooserReadObject();
    for (uint skipped = 0;
         skipped < CHOOSER_MAX_SKIPPED && err == DeserializationError::Ok && !json["type"].is<const char*>();
         skipped++) {
        err = chooserReadObject();
    }
    return err;
}

#define REQUEST_SENT 0    // the request went out and the sign is listening for its answer
#define REQUEST_UNASKED 1 // nothing was sent: bytes nobody asked for are arriving; the sign is listening
#define REQUEST_DEAD 2    // the sign is not listening: the line never became quiet

// How often one exchange may ask (again after letting go by what was not a message)
#define REQUEST_MAX_TRIES 3

// Gets the request for the next message out, if the line allows it. loop()'s and the chooser's.
//
// Three rules keep the sign from talking into what the app is sending, and from asking twice:
//  1. The line must be quiet first, for REQUEST_QUIET_MS, however long the app goes on (up to
//     REQUEST_BUSY_WAIT_MS; a line LOW without a gap for REQUEST_QUIET_WAIT_MS is dead). So a
//     request never follows a message of the app's by less than that, seen or not.
//  2. If the line was seen LOW less than CHOOSER_ATTN_FIRST_BYTE_MS ago (by any look, anywhere),
//     the app may be about to send a message by itself. The sign listens for the rest of that
//     time and sends nothing. If a byte comes, no request is sent: REQUEST_UNASKED.
//  3. A REQUEST THAT HAS GONE OUT STANDS. It is never sent a second time because of what was on
//     the line while it went out. The request byte goes out through chooserSendByte(), which
//     says whether the line from the app changed meanwhile. If it did, something of the app's
//     began while the sign could not receive: the app raising its hand (a break, or the run of
//     0x00 bytes it sends instead of one), the first byte of a message it pushed, or noise. It
//     cannot be the answer: the app had not received the request yet. Whatever it is, its
//     start was missed, so the receiver is not trusted with it (it would take a byte from its
//     middle): the sign stops listening until the line has been HIGH for REQUEST_GAP_MS, longer
//     than it ever is between two bytes sent back to back, and then listens for the answer as
//     after any request. What the app sends behind a break or a run of zeros is then read whole:
//     the message it pushed, or, if it has left chooser mode, the answer to this very request.
//     Asking again instead would take a second message from the app's list for the one shown.
//     The one thing that is lost is a pushed message whose own first byte crossed the request
//     (no break in front of it that the sign could see, and its first bit inside the 0.7 ms of
//     the request): the request then gets no answer, the read ends as unanswered, and the sign
//     is that much later, with nothing taken and no error. (If that message pauses further on,
//     its second part arrives where the answer would, is let by like any rest of a message,
//     and the sign does ask again: chooserExchange. The app is in chooser mode then.)
//     And one limit: for that REQUEST_GAP_MS the sign hears nothing. The app answers a request
//     2 to 9 ms after it. If NOISE crossed the request and the answer began within the
//     millisecond after it, the answer goes by and the request counts as unanswered.
static uint chooserRequest() {
    if (chooserAwaitQuietLine(REQUEST_BUSY_WAIT_MS, REQUEST_QUIET_WAIT_MS, REQUEST_QUIET_MS, false) != LINE_QUIET) {
        return REQUEST_DEAD;
    }
    startSerial(); // we stopped listening after the last read, so this starts with an empty buffer
    while (lineWasLow && millis() - lineLowAtMs < CHOOSER_ATTN_FIRST_BYTE_MS) {
        if (softSerial.peek() >= 0) {
            return REQUEST_UNASKED;
        }
    }
    bool crossed = false;
#if CHOOSER_DEBUG_PANEL_B
    crossed = chooserDebugLine();
#endif
    if (chooserSendByte('~')) {
        crossed = true;
    }
    if (crossed) {
        stopSerial();
        if (chooserAwaitQuietLine(REQUEST_BUSY_WAIT_MS, REQUEST_QUIET_WAIT_MS, REQUEST_GAP_MS, false) != LINE_QUIET) {
            return REQUEST_DEAD; // (the request did go out; nothing can be heard on this line)
        }
        startSerial();
    }
    return REQUEST_SENT;
}

// The line is carrying the rest of something the sign did not hear the start of. Stops
// listening and lets it go by: until the line has been quiet for longer than the app may pause
// inside a message.
static void chooserLetRestPass() {
    stopSerial();
    chooserAwaitQuietLine(REQUEST_BUSY_WAIT_MS, REQUEST_QUIET_WAIT_MS, CHOOSER_ATTN_BYTE_MS + 10, false);
}

// One exchange with the app: gets its next message into `json`. loop()'s read and the chooser's.
// Asks, and reads the answer; or, if the app is already sending by itself, reads that.
//   Ok          a message is in `json` (an object; whether it has a type is the caller's check
//               for an answer, and checked here for what came unasked)
//   EmptyInput  no answer, or the line is dead
//   another     the last of REQUEST_MAX_TRIES requests in a row was answered with something that
//               opened like a message and could not be read
// A FRAME IS WHOLE OR IT IS NOT A FRAME. What does not open like a message (`{"`), what opens
// like one and cannot be read to its end, and what arrives unasked and is not a complete object
// with a type, is first of all the rest of something whose start the sign did not hear (the app
// may pause inside a message for longer than the line needs to look quiet). So it is no error
// yet: it is let by, until the line has been quiet for longer than such a pause, and the sign
// asks again. That costs no message: if the bytes were an answer that was damaged on the line,
// its message was lost with them; if they were the rest of a pushed message, the app is in
// chooser mode, where a request takes nothing. Only when the last try ends the same way is it
// reported. The caller stops the listening.
static DeserializationError chooserExchange() {
    DeserializationError unreadable = DeserializationError::EmptyInput;
    for (uint attempt = 0; attempt < REQUEST_MAX_TRIES; attempt++) {
        unreadable = DeserializationError::EmptyInput;
        uint request = chooserRequest();
        if (request == REQUEST_DEAD) {
            return DeserializationError::EmptyInput;
        }
        if (request == REQUEST_UNASKED) {
            if (chooserAwaitObject(CHOOSER_ATTN_FIRST_BYTE_MS) &&
                chooserReadFrame(json, CHOOSER_ATTN_BYTE_MS) == DeserializationError::Ok &&
                json["type"].is<const char*>()) {
                return DeserializationError::Ok;
            }
            chooserLetRestPass();
            continue;
        }
        DeserializationError err = chooserReadMessage();
        if (err == DeserializationError::Ok || err == DeserializationError::EmptyInput) {
            return err;
        }
        unreadable = err;
        chooserLetRestPass();
    }
    return unreadable;
}

// A read has just ended, at the closing brace. Whatever the app writes behind a message (a
// newline, say) is not the app raising its hand: let it go by before the line is watched again.
// A break is not let by: it is still there for the next look at the line.
static void chooserLetTrailerPass() {
    chooserAwaitQuietLine(CHOOSER_TRAILER_WAIT_MS, CHOOSER_TRAILER_WAIT_MS, CHOOSER_LINE_QUIET_MS, true);
}

// Asks for the whole message the normal way and reads the answer into `json`, with the normal
// read's patience (a second per byte). No error flash and no error text on the wire.
// Only ever called with the text standing still or already stopped.
static bool chooserAsk() {
    DeserializationError err = chooserExchange();
    stopSerial();
    chooserLetTrailerPass();
    return err == DeserializationError::Ok;
}

// True when this is the moment to read the whole message again: where a wait cannot be seen.
static bool chooserSyncIsDue() {
    if (chooserScrolls(chooser.layout)) {
        // standing still on its start, having come round since the last read
        return chooser.holding && chooser.syncDue;
    }
    // a text that fits: in the dark after a blink, when the last read is long enough ago
    return chooser.blinkCount == 0 && millis() - chooser.fullReadMs >= CHOOSER_REFRESH_MS;
}

// The read underneath the pushes: the whole message, compared with what is shown.
static uint chooserSync() {
    chooser.syncDue = false; // one try per stand-still
    return chooserTake(chooserAsk());
}

// Nothing usable was read: ask the normal way (chooserExchange() first lets whatever is still
// coming in go by).
static uint chooserFallback() {
#if CHOOSER_DEBUG_PANEL_B
    chooserCount(chooserDebug.fallbacks);
#endif
    return chooserTake(chooserAsk());
}

// The receive line was seen LOW: the app has something to say and will say it unasked.
// Waits for the line to go HIGH and stay HIGH (the break is over), listens, and reads one message
// into `into`. Returns true when a whole JSON object came. When bytes came that were not one, the
// line is quiet again on return (they have gone by). Nothing here knows about the chooser:
// any display loop that looks at chooserLineLow() between its frames can call this, and then
// decide what the message means for what it is showing.
static bool attentionRead(JsonDocument& into) {
#if CHOOSER_DEBUG_PANEL_B
    unsigned long seenMs = millis();
    unsigned long seenUs = micros();
    chooserCount(chooserDebug.attentions);
    chooserDebug.firstByteMs = 0;
    chooserDebug.endMs = 0;
    chooserDebug.lowMs = 0;
    // how much longer the line stays LOW (this look is only in the bench build; if the line does
    // not go HIGH it makes the wait that follows start CHOOSER_ATTN_HIGH_WAIT_MS later)
    while (micros() - seenUs < CHOOSER_ATTN_HIGH_WAIT_MS * 1000UL) {
        if (!lineSeenLow()) {
            chooserDebug.lowMs = (unsigned int) ((micros() - seenUs + 999) / 1000);
            break;
        }
    }
#endif
    if (chooserAwaitQuietLine(CHOOSER_ATTN_HIGH_WAIT_MS, CHOOSER_ATTN_HIGH_WAIT_MS, CHOOSER_LINE_QUIET_MS, false) ==
        LINE_BUSY) {
        // Bytes, and no end to them: this LOW was no break but a bit of a message that is going
        // by (the app's break was too short to be seen, or it gives none). Its start is gone.
        // Let all of it go by; the caller then asks, once.
        chooserLetRestPass();
        return false;
    }
    startSerial(); // begins with an empty buffer: nothing of the break is in it
    bool readOk = false;
    bool began = chooserAwaitObject(CHOOSER_ATTN_FIRST_BYTE_MS);
    bool nothingCame = !began && softSerial.peek() < 0;
    if (began) {
#if CHOOSER_DEBUG_PANEL_B
        chooserDebug.firstByteMs = (unsigned int) (millis() - seenMs);
#endif
        readOk = chooserReadFrame(into, CHOOSER_ATTN_BYTE_MS) == DeserializationError::Ok;
#if CHOOSER_DEBUG_PANEL_B
        chooserDebug.endMs = (unsigned int) (millis() - seenMs);
#endif
    }
    stopSerial();
    if (readOk) {
        chooserLetTrailerPass();
    } else if (!nothingCame) {
        // Bytes came and were not a whole message: the rest of one (the LOW was a bit of it, not
        // a break), or one that was cut short. Let the rest go by before the caller asks, or
        // every bit of it would be the next attention.
        chooserLetRestPass();
    }
    return readOk;
}

// Attention while the chooser is on screen. A chooser message is shown from its start, whether
// or not it is the one already showing. If no message came within CHOOSER_ATTN_FIRST_BYTE_MS the
// caller asks: that is what the app means when it has left chooser mode (it raises its hand and
// sends nothing).
static uint chooserAttention() {
    uint reply = chooserTake(attentionRead(json));
    if (reply == CHOOSER_REPLY_SAME) {
        chooserRestart();
        reply = CHOOSER_REPLY_CHANGED;
    }
    return reply;
}

// Shows a chooser message and stays here for as long as the chooser is on screen. Returns true
// when `json` holds a message of another type for loop() to show, false when the app did not
// answer (two requests in a row unanswered, or the line LOW for good): loop() then does what it
// does after any unanswered request.
//
// Every time round this loop the receive line is looked at first, and every wait in it watches
// the line, so the longest the line goes unwatched is one frame (or, once per message, the
// blanking of the strip and a frame): a few milliseconds. A LOW seen during a wait is acted on
// even if it is gone again by the next look. No byte is sent from here while a text is moving,
// except after an attention that was not followed by a whole message.
bool showAsChooser(const char* blinkyStr, const char* countyStr) {
    chooser.failedReads = 0;
    // loop() has drawn something else since the chooser was last on the LEDs (the message that
    // ended it, or the stars), so the message is always drawn afresh here, from its start.
    chooserStore(blinkyStr, countyStr);
    chooserFullReadDone(); // loop()'s read has just brought this message

    bool attention = false; // the line was seen LOW during the last wait
    for (;;) {
        uint reply = CHOOSER_REPLY_SAME;
        if (attention || lineSeenLow()) {
            attention = false;
            reply = chooserAttention();
        } else if (chooserSyncIsDue()) {
            reply = chooserSync();
        }
        if (reply == CHOOSER_REPLY_FAILED) {
            chooser.failedReads++;
            if (chooser.failedReads >= CHOOSER_MAX_FAILED_READS) {
                return false; // a line that stays LOW, an app that is gone
            }
            if (lineSeenLow()) {
                attention = true; // the app raised its hand while we were reading: that comes first
                continue;         // (and is acted on even if the line is HIGH again by then)
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
            // Fits: the blink. The text fades to black frame after frame, stays dark for a
            // moment (the time a read took in that moment is part of it), and starts again.
            if (chooser.blinkCount) {
                chooser.blinkCount--;
                chooserDrawFrame(chooserBlinkLevel(chooser.blinkCount));
                if (!chooser.blinkCount) {
                    chooser.holdStartUs = micros();
                }
            } else if (chooserWait(chooser.holdStartUs, CHOOSER_BLINK_PAUSE_MS * 1000UL)) {
                attention = true;
            } else {
                chooser.blinkCount = CHOOSER_BLINK_FRAMES - 1;
                chooserDrawFrame(chooserBlinkLevel(chooser.blinkCount));
            }
        } else if (chooser.holding) {
            // Standing still on the start of the text (the frame is already on the LEDs). The
            // time a read took is part of the stand-still, not added to it.
            if (chooserWait(chooser.holdStartUs, CHOOSER_HOLD_MS * 1000UL)) {
                attention = true;
            } else {
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
            attention = chooserWait(stepStartUs, CHOOSER_SCROLL_DELAY_MS * 1000UL);
        }
    }
}

// What loop() does with a reply it has read into `json`. Returns the type character of the message
// to show now, or one of these two:
#define CHOOSER_DISPATCH_NOTHING 0   // not a message (it has no type): draw nothing and ask again
#define CHOOSER_DISPATCH_NO_ANSWER 1 // the chooser was shown and ended because the app stopped answering:
                                     // do what follows an unanswered request (the stars), not another wait
// A chooser message is shown here, for as long as the chooser is on screen; the message that ended
// it is then in `json`.
static char chooserDispatch() {
    const char* type = json["type"];
    if (!type) {
        return CHOOSER_DISPATCH_NOTHING;
    }
    if (type[0] == MSGTYPE_CHOOSER) {
        if (!showAsChooser(json["str"], json["flashy"])) {
            return CHOOSER_DISPATCH_NO_ANSWER;
        }
        type = json["type"]; // chooserTake() only lets a message with a type end the display
    }
    // (a type whose first byte is that code's value is no type the sign knows: the default one)
    return type[0] == CHOOSER_DISPATCH_NO_ANSWER ? MSGTYPE_DEFAULT : type[0];
}

#endif //LED_MARQUEE_CHOOSERDISPLAY_H
