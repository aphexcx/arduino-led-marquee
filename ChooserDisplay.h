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
//    the sign only when three requests in a row end that way, or when the line does not fall
//    silent behind such a thing.
//  - A request that has gone out stands. Whether something of the app's began while it went
//    out is known, not guessed, and so is when (the receive pin's change flag, looked at after
//    every bit of the sign's own byte: chooserSendByte, in the sketch). The one byte of the
//    app's that began then is lost; the sign's serial port lets it go by and listens again
//    from its end, so what follows it is received whole, the answer to the request too, however
//    close behind. That something crossed the request is never by itself a reason to ask
//    again; a second request would take a second message from the app's list (chooserRequest).
//  - Once the app has raised its hand, the chooser message that comes next is shown from its
//    start, by whatever way it comes: read where a push is looked for, read in an exchange the
//    sign began for another reason, or as the plain answer to the sign's own request because
//    the pushed one went by unread. Also when it is the message already showing (handSeen).
//  - The first LOW the sign sees may be a bit of a message, not a break: the sign's own app
//    raises its hand with a run of 0x00 bytes, and a break can be too short to see. A run of
//    zeros is waited out like a break. A message that is already going by is let by, all of it,
//    and then the sign asks, once (attentionRead).
//  - ONE RULE FOR THE GAP BETWEEN TWO BYTES OF ONE FRAME. The app may pause between any two
//    bytes of what it writes for one message: for up to the time the read of that frame gives
//    a byte (CHOOSER_ATTN_BYTE_MS when it came unasked, ANSWER_FIRST_BYTE_MS when it was asked
//    for). The same time holds wherever a frame is read or let go by: for every byte that
//    ArduinoJson reads, for each byte of white space in front of the opening brace and behind
//    it, and for the rest of a frame that could not be read (chooserLetRestPass). And for what
//    the app writes behind the closing brace (a newline), with one limit, because the sign is
//    not listening then and only sees the line: bytes that come late are known as that when
//    they are over within CHOOSER_TRAILER_LATE_MS (chooserHandRaised). A longer run is a
//    message going by, as far as the sign can tell, and it asks.
//  - A byte that is nobody's is stepped over. A LOW too short to be the app's is received as
//    one of 0xFF, 0xFE, 0xFC, 0xF8, 0xF0, 0xE0, 0xC0, 0x80, by its length; two of them close
//    together as any byte at all. In front of a message, and between its brace and its first
//    quote, every byte that is not a character of text is stepped over (chooserIsPadding; a
//    0x00 behind the brace is the app raising its hand and ends the frame). Inside a message,
//    between its texts, every byte that is not a character of text is dropped; inside a text,
//    every byte above 0x7F that does not begin or continue a UTF-8 character (ChooserReader):
//    in a text of the app's, which is ASCII, that is every one of the eight, alone or in a row.
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

// How many bytes that are nobody's are dropped from the inside of one message (a line that
// makes them without end must not keep a read going without end)
#define CHOOSER_MAX_NOISE 8

// How often white space in front of a message, or behind its brace, gives the next byte a new
// wait (an app that writes white space without end must not keep the sign waiting without end)
#define CHOOSER_MAX_WHITE_WAITS 8

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

// ONCE THE APP HAS RAISED ITS HAND, THE CHOOSER MESSAGE THAT COMES NEXT IS SHOWN FROM ITS START,
// BY WHATEVER WAY IT COMES. The app raises its hand and sends the chooser message unasked when
// what the chooser should show has changed, or when it has come back to the chooser. That
// message is shown from its start also when it is the one already showing (the contract). It
// does not always arrive where the sign looks for it (attentionRead). The sign may read it in
// an exchange it began for another reason: where a request was due and the app was already
// sending, as the "answer" to a request the push overtook, or behind a request it crossed. Or
// the sign may not read it at all (it went by while the sign waited for the line to be quiet)
// and get the same message as the plain answer to its own request. In all of these the caller
// would call the same text "the same" and go on from the middle of it: an app that left and
// came back to the same selection. So the hand is remembered until a message has been taken:
//   - the line was LOW for CHOOSER_BREAK_MIN_US without a gap, in any wait that watches it
//     (chooserAwaitQuietLine): a break, or one of the zero bytes the app sends for one
//   - a 0x00 was received while the sign listened (chooserTakePadding): the same, received
//   - a message was read where a request was due and none was sent (chooserExchange)
// Not a hand: a LOW that was a bit of bytes going by (a stray frame, a late answer, a newline
// behind the last message), also when such a byte crosses the sign's own request. The sign
// asks then too, and the same message for an answer goes on from where it is.
// Set in those three places (a break or zeros that cross a request are seen by the first two:
// chooserRequest looks at the line for it). Read where a message is taken, and put down there:
// what was seen up to the end of that read belongs to that message; a hand raised behind it
// belongs to the next (chooserAsk, chooserAttention). Put down when the chooser comes on the
// LEDs (showAsChooser).
static bool handSeen;

static bool lineSeenLow() {
    if (chooserLineLow()) {
        lineLowAtMs = millis();
        lineWasLow = true;
        return true;
    }
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
static uint chooserAwaitQuietLine(unsigned int maxMs, unsigned int maxLowMs, unsigned int quietMs,
                                  bool stopAtBreak) {
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
            if (nowUs - lowSinceUs >= CHOOSER_BREAK_MIN_US) {
                handSeen = true;
                if (stopAtBreak) {
                    return LINE_BREAK;
                }
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

static bool chooserIsWhiteSpace(int c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// What may stand in front of a message without being part of it: every byte that is not a
// character of text (0x21 to 0x7E). A message begins with a brace; a character of text in front
// of one is the rest of something else, and the caller lets that go by. Everything else is:
//  - 0x00: what a break is received as, and each of the zero bytes the app sends instead of one
//  - white space: an app that ends what it writes with a newline
//  - what the receiver makes of a LOW pulse on the line that is shorter than a break. It takes
//    the falling edge for a start bit and looks at the line eight times, one bit time apart,
//    from 1.5 bit times after the edge: LOW (0) while the pulse lasts, HIGH (1) after it. So
//    ONE pulse is all ones above as many zeros as it had looks: up to 1.5 bit times (104 us at
//    14400 baud) 0xFF, then one more zero for every bit time, 0xFE 0xFC 0xF8 0xF0 0xE0 0xC0, to
//    0x80 for 7.5 to 8.5 bit times; from there on it is the 0x00 of a break. (Measured in the
//    emulator with the sign's own receiver: one pulse of every length from 2 us to 715 us; see
//    the pull request.) Two pulses inside one byte time can make any other byte with its high
//    bits set, or 0x7F, or a control character.
// Not covered, because no rule can: noise that forges a character of text (three pulses or
// more, placed to the bit), and a pulse so close in front of a message that the receiver is
// still busy with it when the brace begins.
static bool chooserIsPadding(int c) {
    return c >= 0 && (c < 0x21 || c > 0x7E);
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
        handSeen = true;
    }
}

// Waits up to maxMs for a pushed message to begin: the next byte in the receive buffer that is
// not padding. True if that byte is the '{' that opens it (it is left in the buffer). Anything
// else is not a message, whatever comes behind it (an object inside an array, the tail of bytes
// that were under way when we began to listen): false at once, and the caller asks.
// White space in front of the brace is a byte the app wrote for this message: the byte behind
// it has byteMs, as any byte behind another (ONE RULE). Other padding is given no time.
static bool chooserAwaitObject(unsigned int maxMs, unsigned int byteMs) {
    unsigned long startMs = millis();
    uint waits = 0;
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
            if (chooserIsWhiteSpace(c) && waits < CHOOSER_MAX_WHITE_WAITS) {
                waits++;
                startMs = millis();
                maxMs = byteMs;
            }
        }
    } while (millis() - startMs < maxMs);
    return false;
}

// What ArduinoJson reads a message from: the serial line, with the opening brace in front again
// that chooserOpensMessage() took from it, and without the bytes that are nobody's:
//  - between the texts of a message (outside double quotes) JSON is characters of text and
//    white space: every other byte is dropped, a control character and 0x7F too;
//  - inside a text every byte up to 0x7F is the text's (the sign's heart is 0x7F), and a byte
//    above it is kept only as part of a UTF-8 character: a lead byte (0xC2 to 0xF4) with a
//    continuation byte behind it that such a lead can have, and the continuation bytes (0x80
//    to 0xBF) that follow. So a text in UTF-8 arrives whole, and in a text in ASCII, as the
//    app's are, nothing above 0x7F is left: no combination of the eight bytes the receiver
//    makes of one pulse begins a character (0xE0 0x80 and 0xF0 0x80 are not UTF-8).
// At most CHOOSER_MAX_NOISE bytes are dropped from one frame; from then on the frame is read
// as it comes. (Not caught: noise that forges a character of text, or a whole UTF-8 character,
// or a control character inside a text. A text in single quotes, which ArduinoJson also reads
// and the app does not write, is treated as if it were outside a text.)
// Every byte is given the frame's time for a byte, counted from the byte before it.
struct ChooserReader {
    bool brace;        // the '{' has not been handed over yet
    bool starved;      // a byte did not come in its time: that is where the read ended
    bool inText;       // between the double quotes of a text
    bool escaped;      // ... behind a backslash
    uint dropped;      // how many bytes were dropped
    uint continuation; // how many more bytes the UTF-8 character under way has
    int held;          // the byte that was looked at behind a lead byte, -1 if none

    // The next byte from the line, -1 if none came in its time
    int next() {
        if (held >= 0) {
            int c = held;
            held = -1;
            return c;
        }
        char c;
        if (!softSerial.readBytes(&c, 1)) { // (waits, up to the timeout)
            starved = true;
            return -1;
        }
        return (unsigned char) c;
    }

    int read() {
        if (brace) {
            brace = false;
            return '{';
        }
        for (;;) {
            int c = next();
            if (c < 0) {
                return -1;
            }
            bool keep = false;
            if (!inText) {
                keep = chooserIsWhiteSpace(c) || (c >= 0x20 && c <= 0x7E);
            } else if (c < 0x80) {
                keep = true;
                continuation = 0;
            } else if (c < 0xC0) {
                if (continuation) {
                    continuation--;
                    keep = true;
                }
            } else if (c >= 0xC2 && c <= 0xF4) {
                int after = next();
                if (after < 0) {
                    return -1;
                }
                held = after;
                // (UTF-8 has no character that begins E0 80.., F0 80.., ED A0.. or F4 90..)
                int lowest = c == 0xE0 ? 0xA0 : c == 0xF0 ? 0x90 : 0x80;
                int highest = c == 0xED ? 0x9F : c == 0xF4 ? 0x8F : 0xBF;
                if (after >= lowest && after <= highest) {
                    continuation = c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
                    keep = true;
                }
            }
            if (!keep && dropped < CHOOSER_MAX_NOISE) {
                dropped++;
                continue;
            }
            if (!inText) {
                inText = c == '"';
            } else if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inText = false;
            }
            return c;
        }
    }

    size_t readBytes(char* buffer, size_t length) {
        size_t n = 0;
        while (n < length) {
            int c = read();
            if (c < 0) {
                break;
            }
            buffer[n++] = (char) c;
        }
        return n;
    }
};

// A MESSAGE OPENS WITH A BRACE AND A DOUBLE QUOTE: `{"` (white space may stand between them;
// `{}` is let through as the object without a type that it is). The '{' is at the front of the
// receive buffer. Takes it and looks at what follows, waiting up to byteMs for it: again for the
// byte behind each byte of white space, as between any two bytes of a frame (up to
// CHOOSER_MAX_WHITE_WAITS times). A byte that is nobody's may stand there too (chooserIsPadding):
// it is stepped over and given no time. A 0x00 is not: that is the app raising its hand.
// False: this is not where a message begins. It is a '{' from the inside of one, the rest of
// something the sign did not hear the start of, and the caller lets it go by.
// The quote is what tells the two apart. ArduinoJson also reads keys in single quotes and
// without any, so the rest of a pushed message whose text is  x{type:'D',str:'...'}  would be a
// message of its own if a brace were enough. But inside a JSON string a double quote is always
// written \", so a brace inside a text is followed by a double quote only when the text ends
// there, and what follows that (`{","flashy":...`, `{"}`) is never an object.
// (This holds for messages whose values are texts and numbers, as all of the app's are. A value
// that is itself an object would open like a message.)
// How long the rest of the frame that was read last, and could not be read, may still pause:
// the time its read gave a byte. 0 when the read ended because a byte did not come in that time
// (the line has then been silent for as long as the app may pause: nothing more is coming).
static unsigned int frameRestMs;

static bool chooserOpensMessage(unsigned int byteMs) {
    softSerial.read(); // the '{'
    unsigned long startMs = millis();
    uint waits = 0;
    bool since = false; // a byte was taken since startMs was set
    do {
        int c = softSerial.peek();
        if (c == '"' || c == '}') {
            return true;
        }
        if (c >= 0) {
            if (c == 0 || !chooserIsPadding(c)) {
                return false;
            }
            softSerial.read();
            since = true;
            if (chooserIsWhiteSpace(c) && waits < CHOOSER_MAX_WHITE_WAITS) {
                waits++;
                startMs = millis(); // the next byte has the same time again
                since = false;
            }
        }
    } while (millis() - startMs < byteMs);
    if (!since) {
        frameRestMs = 0; // (silent for a byte's time: nothing more is coming)
    }
    return false; // a brace and then nothing
}

// ONE RULE FOR THE GAP BETWEEN TWO BYTES OF ONE FRAME: the time a frame's read gives each byte
// is also the time the app has for whatever it writes behind the frame (a newline), byte after
// byte. Kept here: when the last byte of the last frame read (or of what came behind it) was
// taken, and the time that frame's bytes were given (0: nothing was read, or that time is over).
static unsigned long frameEndMs;
static unsigned int frameByteMs;
static bool frameOpened; // the frame read last opened like a message (chooserOpensMessage)

// Reads the message that begins at the front of the receive buffer (the caller has seen its '{'
// there) into `into`, allowing byteMs for every byte after the first.
// InvalidInput also when it does not open like a message (chooserOpensMessage).
// When the read fails, frameRestMs says how long the rest of the frame may still pause.
static DeserializationError chooserReadFrame(JsonDocument& into, unsigned int byteMs) {
    DeserializationError err = DeserializationError::InvalidInput;
    softSerial.setTimeout(byteMs);
    frameRestMs = byteMs;
    frameOpened = chooserOpensMessage(byteMs);
    if (frameOpened) {
        ChooserReader reader = {true, false, false, false, 0, 0, -1};
        err = deserializeJson(into, reader);
        if (reader.starved) {
            frameRestMs = 0;
        }
    }
    softSerial.setTimeout(1000); // the Stream default
    if (err == DeserializationError::Ok) {
        frameEndMs = millis(); // at its closing brace
        frameByteMs = byteMs;
    }
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

// WHAT STANDS WHERE THE ANSWER IS WAITED FOR AND IS NO ANSWER. Bytes that are no character of
// text are stepped over (chooserIsPadding): white space the app wrote behind its last message,
// the zeros of its hand, what the receiver makes of a pulse. So the rest of a newline that
// crossed the request costs nothing: the answer behind it is read.
// A character of text that is no opening brace, or a brace from which no whole message is
// read, is something else: the rest of a frame nobody asked for (an answer that comes long
// after the sign stopped waiting for it; a pushed message whose break the sign could not see),
// or an answer damaged on the line. THE SIGN DOES NOT TRY TO FIND AN ANSWER BEHIND IT. It stops
// listening, lets the line go by until it has been silent for the time an answer's byte is
// given (a second), and asks again (chooserExchange). The cost, known and taken: an answer
// that the app sends behind such bytes, glued to them or less than a second after them, goes
// by with them, and the second request takes the app's next message. The sign's own app sends
// nothing of the kind: it writes no frame unasked outside chooser mode, and in chooser mode
// its requests take nothing.
// (A version of this code followed such bytes to find where the answer began: three guesses
// about where in a message the first byte heard stood. Four reviews in a row found a case it
// lost. It is not here.)

// Waits for the first byte of the answer to a '~', as long as a read waits for any byte.
// Padding in front of it is thrown away (chooserIsPadding). A 0x00 is what a break is received
// as when it falls while we are listening: the app raised its hand just as we asked, and what it
// sends next (if anything) is the answer. Without this the 0x00 would end the read as "no
// answer" while the answer is still on its way.
// A 0x00 that comes late in the wait is given the time a pushed message needs to follow it (the
// rest of the break, the guard), so the wait can outlast ANSWER_FIRST_BYTE_MS by that much, once.
// White space is a byte the app wrote for its answer: the byte behind it has the time any byte
// of an answer has (ONE RULE), CHOOSER_MAX_WHITE_WAITS times. Other padding is given no time,
// and neither is white space beyond that count: the wait can then end as "no answer" with such
// a byte only just received (nine bytes of white space each most of a second apart, in front of
// an answer, cost that answer).
static bool chooserAwaitAnswer() {
    unsigned long startMs = millis();
    unsigned long waitMs = ANSWER_FIRST_BYTE_MS;
    uint waits = 0;
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
        } else if (chooserIsWhiteSpace(c) && waits < CHOOSER_MAX_WHITE_WAITS) {
            waits++;
            startMs = millis();
            waitMs = ANSWER_FIRST_BYTE_MS;
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
        // Every message opens with a brace. Whatever this is, it stands where an answer was
        // due, and may be one that lost its first byte: it has the time an answer has.
        frameRestMs = ANSWER_FIRST_BYTE_MS;
        return DeserializationError::InvalidInput;
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

// How often a request may find a byte on its way just as it begins to listen, and wait for the
// line to be quiet once more
#define REQUEST_MAX_LOOKS 3

// Gets the request for the next message out, if the line allows it. loop()'s and the chooser's.
//
// Three rules keep the sign from talking into what the app is sending, and from asking twice:
//  1. The line must be quiet first, for REQUEST_QUIET_MS, however long the app goes on (up to
//     REQUEST_BUSY_WAIT_MS; a line LOW without a gap for REQUEST_QUIET_WAIT_MS is dead). So a
//     request never follows a message of the app's by less than that, seen or not.
//     THE LAST PART OF THAT QUIET IS WATCHED WITH THE RECEIVER ON (REQUEST_GAP_MS, longer than
//     a byte). Between the last look at the line and the receiver's coming on there is a moment
//     in which neither sees a start bit (the receiver works out its delays first: a bit time
//     or two). A byte that begins just then (what the app wrote late behind its last message)
//     would be taken from its middle, and so would every byte that follows it back to back:
//     the answer, if the app sends it right behind. So the sign looks once more while it
//     already listens: a LOW, or anything received, and the line was not quiet after all. That
//     counts as a LOW seen (rule 2); the sign stops listening and waits for the quiet again.
//     If that happens REQUEST_MAX_LOOKS times in a row, bytes nobody asked for keep arriving:
//     no request is sent (REQUEST_UNASKED). A request never goes out unwatched.
//     AND THE REQUEST IS NOT SENT WITH A BYTE RECEIVED: the last look of all is made by the
//     serial port itself, with interrupts off, in front of the request's start bit
//     (chooserSendRequest). A byte that was received in the moment before counts as above.
//     So whatever the sign hears behind its request, it has heard nothing of it before.
//  2. If the line was seen LOW less than CHOOSER_ATTN_FIRST_BYTE_MS ago (by any look, anywhere),
//     the app may be about to send a message by itself. The sign listens for the rest of that
//     time and sends nothing. If a byte comes, no request is sent: REQUEST_UNASKED.
//  3. A REQUEST THAT HAS GONE OUT STANDS. It is never sent a second time because of what was on
//     the line while it went out. The request byte goes out through chooserSendByte(), which
//     says whether the line from the app changed meanwhile. If it did, something of the app's
//     began while the sign could not receive: the app raising its hand (a break, or the run of
//     0x00 bytes it sends instead of one), a byte it wrote late behind its last message (a
//     newline), the first byte of a message it pushed, or noise. It cannot be the answer: the
//     app had not received the request yet. That ONE byte is lost. Nothing else is: the sign's
//     serial port knows in which bit of the request it began, so it knows when it is over, and
//     listens again from there, in step (SoftwareSerial::write() in this sketch's copy). So
//     whatever follows that byte is received whole, however close behind it: the rest of what
//     the app was writing, and THE ANSWER TO THIS REQUEST, also when the app's UART sends it
//     back to back with the bytes that crossed. (It used to wait for a pause on the line
//     before it listened again, and an answer that began before that pause was over went by
//     unheard: the app's message, taken from its list and never shown.)
//     The receiver is off from the bit in which the byte began to the end of that byte: at
//     most 0.59 ms behind the request's last data bit. A byte cannot be followed sooner by
//     the same UART, so nothing the app sends is lost in that time. What is: an answer that
//     begins in it after NOISE crossed the request (a LOW that was no byte). The app answers
//     a request 2 to 9 ms after it.
//     What that means for each thing that can cross:
//      - a newline, or any white space or padding the app wrote behind its last message: the
//        rest of it is stepped over where an answer is waited for, and the answer is read.
//      - a break: nothing is received of it; a run of zeros: the rest of it is received as
//        zeros and stepped over. What the app sends behind either is read whole: the message
//        it pushed, or, if it has left chooser mode, the answer to this very request. Asking
//        again instead would take a second message from the app's list for the one shown.
//      - the first byte of a frame nobody asked for (a pushed message with no break in front
//        of it that the sign could see; an answer that comes long after the sign stopped
//        waiting): the rest of it does not open like a message. It is let by until the line
//        has been silent for a second, and the sign asks again (chooserExchange). After a
//        pushed message the app is in chooser mode, where a request takes nothing.
//      - noise: nothing follows it, and the answer is read (but see the limit above).
//     The crossing counts as a LOW seen (rule 2). Whether it was the app's hand, the line
//     says: the sign looks at it for REQUEST_GAP_MS, with the receiver on (handSeen).
static uint chooserRequest() {
    for (uint looks = 1;; looks++) {
        if (chooserAwaitQuietLine(REQUEST_BUSY_WAIT_MS, REQUEST_QUIET_WAIT_MS, REQUEST_QUIET_MS - REQUEST_GAP_MS,
                                  false) != LINE_QUIET) {
            return REQUEST_DEAD;
        }
        startSerial(); // we stopped listening after the last read, so this starts with an empty buffer
        if (chooserAwaitQuietLine(REQUEST_GAP_MS, REQUEST_GAP_MS, REQUEST_GAP_MS, false) == LINE_QUIET &&
            softSerial.peek() < 0) {
            while (lineWasLow && millis() - lineLowAtMs < CHOOSER_ATTN_FIRST_BYTE_MS) {
                if (softSerial.peek() >= 0) {
                    return REQUEST_UNASKED;
                }
            }
            bool crossed = false;
#if CHOOSER_DEBUG_PANEL_B
            crossed = chooserDebugLine();
#endif
            uint sent = chooserSendRequest('~');
            if (sent != SEND_HELD) {
                if (crossed || sent == SEND_CROSSED) {
                    // (the port has let the byte that crossed go by and is listening again)
                    lineLowAtMs = millis();
                    lineWasLow = true;
                    // A break is still LOW now, and stays so: that is the app's hand (handSeen is set
                    // by this look). Zeros are received, and say so themselves. Anything else is no hand.
                    chooserAwaitQuietLine(REQUEST_GAP_MS, REQUEST_GAP_MS, REQUEST_GAP_MS, false);
                }
                return REQUEST_SENT;
            }
        }
        // a byte is on its way, or has just been received (the receiver's interrupt may have
        // kept every look at the line from seeing it LOW)
        lineLowAtMs = millis();
        lineWasLow = true;
        if (looks >= REQUEST_MAX_LOOKS) {
            return REQUEST_UNASKED;
        }
        stopSerial(); // its start was not seen, or the request was not sent behind it: nothing of it is kept
    }
}

// The line is carrying the rest of something the sign did not hear the start of, or could not
// read. Stops listening and lets it go by: until the line has been quiet for longer than the
// app may pause inside such a frame (restMs: the time its read gave a byte).
// ONE RULE, and it is not applied twice: when the read ended because a byte did not come in that
// time (restMs is 0: see frameRestMs), the frame is over and there is no rest. The line is then
// only looked at for a moment, as before any request.
// False: the silence did not come within REQUEST_BUSY_WAIT_MS and restMs (or the line is held
// LOW): bytes are still arriving, and no request belongs into them.
static bool chooserLetRestPass(unsigned int restMs) {
    stopSerial();
    return chooserAwaitQuietLine(REQUEST_BUSY_WAIT_MS + restMs, REQUEST_QUIET_WAIT_MS,
                                 restMs ? restMs + 10 : REQUEST_GAP_MS, false) == LINE_QUIET;
}

// One exchange with the app: gets its next message into `json`. loop()'s read and the chooser's.
// Asks, and reads the answer; or, if the app is already sending by itself, reads that.
//   Ok          a message is in `json` (an object; whether it has a type is the caller's check
//               for an answer, and checked here for what came unasked)
//   EmptyInput  no answer, or the line is dead
//   another     the last of REQUEST_MAX_TRIES requests in a row was answered with something that
//               could not be read; or the line did not fall silent behind such a thing
// A FRAME IS WHOLE OR IT IS NOT A FRAME. What does not open like a message (`{"`), what opens
// like one and cannot be read to its end, and what arrives unasked and is not a complete object
// with a type, is first of all the rest of something whose start the sign did not hear (the app
// may pause inside a message for longer than the line needs to look quiet). So it is no error
// yet: it is let by, until the line has been quiet for longer than such a pause, and the sign
// asks again. If the line does not fall quiet behind what stood where an answer was due (bytes
// go on arriving for seconds), THIS exchange ends there and sends no further request into
// them. (That is a promise for one exchange. What its caller does next is the caller's: the
// chooser asks once more, at once, as after any read that failed, and loop() shows the error
// and asks when it comes round. A line that carries a byte every half second for five seconds
// is not the app. And behind what came unasked the wait's result is not looked at: no request
// is out then, and rule 2 of chooserRequest holds the next one for 150 ms behind any LOW.)
// What it costs when the bytes were not the answer and the answer came close behind them is
// said above (WHAT STANDS WHERE THE ANSWER IS WAITED FOR). It is reported when the last try
// ends with something that could not be read, or the line did not fall silent. The caller
// stops the listening.
static DeserializationError chooserExchange() {
    DeserializationError unreadable = DeserializationError::EmptyInput;
    for (uint attempt = 0; attempt < REQUEST_MAX_TRIES; attempt++) {
        unreadable = DeserializationError::EmptyInput;
        uint request = chooserRequest();
        if (request == REQUEST_DEAD) {
            return DeserializationError::EmptyInput;
        }
        if (request == REQUEST_UNASKED) {
            bool began = chooserAwaitObject(CHOOSER_ATTN_FIRST_BYTE_MS, CHOOSER_ATTN_BYTE_MS);
            if (began && chooserReadFrame(json, CHOOSER_ATTN_BYTE_MS) == DeserializationError::Ok &&
                json["type"].is<const char*>()) {
                handSeen = true; // nobody asked for this message: the app pushed it
                return DeserializationError::Ok;
            }
            chooserLetRestPass(began ? frameRestMs : CHOOSER_ATTN_BYTE_MS);
            continue;
        }
        DeserializationError err = chooserReadMessage();
        if (err == DeserializationError::Ok || err == DeserializationError::EmptyInput) {
            return err;
        }
        unreadable = err;
        if (!chooserLetRestPass(frameRestMs)) {
            return unreadable; // (the line never fell silent: nothing is asked into it)
        }
    }
    return unreadable;
}

// A read has just ended, at the closing brace. Whatever the app writes straight behind a message
// (a newline, say; or more: up to CHOOSER_TRAILER_WAIT_MS of it) is not the app raising its
// hand: let it go by before the line is watched again. A break is not let by: it is still there
// for the next look at the line. What the app writes behind the message LATER is not waited
// for here, where the text stands still for it: chooserHandRaised() knows it when it comes.
static void chooserLetTrailerPass() {
    chooserAwaitQuietLine(CHOOSER_TRAILER_WAIT_MS, CHOOSER_TRAILER_WAIT_MS, CHOOSER_LINE_QUIET_MS, true);
    if (frameByteMs && millis() - frameEndMs < frameByteMs) {
        frameEndMs = millis(); // (what went by just now was behind that frame: more may follow as late again)
    }
}

// The display's look at the line: is the app raising its hand?
// A LOW is the app's hand, except for one thing it may be instead: a byte the app wrote behind
// the last frame, late. (ONE RULE: the app may pause before it as before any byte of that frame.)
// So for that long after a frame a LOW is looked at more closely, which takes a few ms:
//  - LOW for CHOOSER_BREAK_MIN_US without a gap: a break, or a zero byte. The app's hand.
//  - bytes that are over within CHOOSER_TRAILER_LATE_MS (and the line then quiet for
//    CHOOSER_LINE_QUIET_MS): what the app wrote behind the frame. Not attention. More may
//    follow as late again.
//  - bytes that go on for longer: a message is going by whose attention the sign could not
//    see. That is attention, as it always was (attentionRead lets it go by and the sign asks).
// The sign is not listening here, so it cannot tell a long run of white space from a message:
// an app that writes more than fourteen bytes behind a message, late and close together, is
// asked once for each such run.
static bool chooserHandRaised() {
    if (!lineSeenLow()) {
        return false;
    }
    if (!frameByteMs) {
        return true;
    }
    if (millis() - frameEndMs >= frameByteMs) {
        frameByteMs = 0; // no byte of that frame can come any more
        return true;
    }
    if (chooserAwaitQuietLine(CHOOSER_TRAILER_LATE_MS + CHOOSER_LINE_QUIET_MS,
                              CHOOSER_TRAILER_LATE_MS + CHOOSER_LINE_QUIET_MS, CHOOSER_LINE_QUIET_MS, true) != LINE_QUIET) {
        return true;
    }
    frameEndMs = millis();
    return false;
}

// Waits until `us` microseconds have passed since `sinceUs`, watching the receive line all the
// time. Returns true at once if the app raises its hand. Looks at the line at least once.
static bool chooserWait(unsigned long sinceUs, unsigned long us) {
    do {
        if (chooserHandRaised()) {
            return true;
        }
    } while (micros() - sinceUs < us);
    return false;
}

// Asks for the whole message the normal way and reads the answer into `json`, with the normal
// read's patience (a second per byte). No error flash and no error text on the wire.
// Only ever called with the text standing still or already stopped.
// `hand`: the app had raised its hand before that read ended (handSeen, which is put down here:
// a hand raised behind the answer, which the look at what follows it may see, is the next
// message's).
static bool chooserAsk(bool& hand) {
    DeserializationError err = chooserExchange();
    stopSerial();
    hand = handSeen;
    handSeen = false;
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

// Asks, and does to the display what the answer says (chooserTake). One thing more: if the app
// has raised its hand since a message was last taken, a chooser message is shown from its start
// even when it is the one already showing (see handSeen). Without a hand, the same message as
// the answer to a request goes on from where it is.
static uint chooserAskAndTake() {
    bool hand;
    uint reply = chooserTake(chooserAsk(hand));
    if (reply == CHOOSER_REPLY_SAME && hand) {
        chooserRestart();
        reply = CHOOSER_REPLY_CHANGED;
    }
    if (reply == CHOOSER_REPLY_FAILED && hand) {
        handSeen = true; // no message was taken: the hand is not answered yet
    }
    return reply;
}

// The read underneath the pushes: the whole message, compared with what is shown.
static uint chooserSync() {
    chooser.syncDue = false; // one try per stand-still
    return chooserAskAndTake();
}

// Nothing usable was read: ask the normal way (chooserExchange() first lets whatever is still
// coming in go by).
static uint chooserFallback() {
#if CHOOSER_DEBUG_PANEL_B
    chooserCount(chooserDebug.fallbacks);
#endif
    return chooserAskAndTake();
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
        if (micros() - seenUs >= CHOOSER_BREAK_MIN_US) {
            handSeen = true; // (this wait takes the LOW from the one behind it, which would have said so)
        }
    }
#endif
    if (chooserAwaitQuietLine(CHOOSER_ATTN_HIGH_WAIT_MS, CHOOSER_ATTN_HIGH_WAIT_MS, CHOOSER_LINE_QUIET_MS, false) ==
        LINE_BUSY) {
        // Bytes, and no end to them: this LOW was no break but a bit of a message that is going
        // by (the app's break was too short to be seen, or it gives none). Its start is gone.
        // Let all of it go by; the caller then asks, once.
        chooserLetRestPass(CHOOSER_ATTN_BYTE_MS);
        return false;
    }
    startSerial(); // begins with an empty buffer: nothing of the break is in it
    bool readOk = false;
    bool began = chooserAwaitObject(CHOOSER_ATTN_FIRST_BYTE_MS, CHOOSER_ATTN_BYTE_MS);
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
        chooserLetRestPass(began ? frameRestMs : CHOOSER_ATTN_BYTE_MS);
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
    if (reply != CHOOSER_REPLY_FAILED) {
        handSeen = false; // a message was taken: the hand is answered
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
    handSeen = false;      // ... and whatever hand was seen before it is answered by it

    bool attention = false; // the line was seen LOW during the last wait
    for (;;) {
        uint reply = CHOOSER_REPLY_SAME;
        if (attention || chooserHandRaised()) {
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
            if (chooserHandRaised()) {
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
