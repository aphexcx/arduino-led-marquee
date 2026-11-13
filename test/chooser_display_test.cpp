// Host test for ChooserDisplay.h: the real chooser display code (showAsChooser and everything under
// it) running against a fake sign and a fake app with a virtual clock. No Arduino, no framework.
//
//   clang++ -std=c++11 -Wall -Wextra -o /tmp/chooser_display_test test/chooser_display_test.cpp \
//       && /tmp/chooser_display_test
//
// Add --show to also print a few frames as ASCII art (what panel 1 would look like).
//
// What is real: ChooserDisplay.h, ChooserScroll.h, ArduinoJson, the font and the gamma table.
// What is fake: the LED strip (it records every column and frame), time (a microsecond counter that
// the fakes advance: about 31 us per LED column, 694 us per serial byte at 14400 baud), and the app
// (it answers '~' and '?' the way the contract says, or like the old app, or not at all).
// Not covered here: AVR integer widths (int is 32 bits on the host), real pixel timing, SoftwareSerial.

#undef NDEBUG

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------
// The parts of the sketch that ChooserDisplay.h uses
// ---------------------------------------------------------------------------------------------
#include "../ArduinoJson-v6.18.3.h"

#define uint uint8_t // the sketch's uint is 8 bits (the Mac's own `uint` is 32)

#define PROGMEM
#define pgm_read_byte(address) (*(address))
#define pgm_read_byte_near(address) (*(address))
#define gamma ledGammaTable // math.h already has a gamma()
#define min(a, b) ((a) < (b) ? (a) : (b))

#define COLUMNS_PER_PANEL 60
#define INTERCHAR_SPACE 1

#include "../Gamma.h"
#include "../Fonts.h"

// The sketch's settings (keep in step with the top of LED_Marquee.ino)
#define CHOOSER_HOLD_MS 600
#define CHOOSER_SCROLL_DELAY_MS 35
#define CHOOSER_GAP_COLS 18
#define CHOOSER_TEXT_BRIGHTNESS 145
#define CHOOSER_POLL_EVERY_COLS 6
#define CHOOSER_POLL_EVERY_MS 200
#define CHOOSER_POLL_TIMEOUT_MS 60
#define CHOOSER_MAX_FAILED_POLLS 3

const char MSGTYPE_CHOOSER = 'H';
const char HEART = '\x7F';

const int json_capacity = 601 + JSON_OBJECT_SIZE(8);
StaticJsonDocument<json_capacity> json;

// ---------------------------------------------------------------------------------------------
// Fake time
// ---------------------------------------------------------------------------------------------
static uint64_t nowUs = 0;

static unsigned long millis() {
    nowUs += 4; // a busy wait on millis() makes time pass
    return (unsigned long) (nowUs / 1000);
}

static double ms(uint64_t us) { return us / 1000.0; }

// ---------------------------------------------------------------------------------------------
// Fake sign: 2 panels x 60 columns. Columns sent before show() replace the first columns of the
// strip, the rest keeps what it had (that is how the LED strips behave).
// ---------------------------------------------------------------------------------------------
struct Led {
    uint8_t row, r, g, b;

    bool dark() const { return row == 0 || (r == 0 && g == 0 && b == 0); }
};

static bool operator==(const Led& a, const Led& b) {
    if (a.dark() || b.dark()) return a.dark() && b.dark();
    return a.row == b.row && a.r == b.r && a.g == b.g && a.b == b.b;
}

struct Frame {
    uint64_t tUs;         // when it was latched
    size_t columnsSent;   // how many columns the firmware sent for it
    bool afterClear;      // a clear() came between the previous frame and this one
    std::vector<Led> leds; // the whole sign (120 columns) once this frame is showing
};

static bool interruptsOff = false;
static std::vector<Led> sending;
static std::vector<Led> leds(120, Led{0, 0, 0, 0});
static std::vector<Frame> frames;
static bool clearedSinceLastFrame = false;

static void cli() { interruptsOff = true; }

static void sei() { interruptsOff = false; }

static void sendColumnRGB(uint row, uint r, uint g, uint b) {
    assert(interruptsOff); // pixel timing needs interrupts off
    nowUs += 31;
    sending.push_back(Led{row, r, g, b});
}

static void show() {
    assert(!interruptsOff);
    nowUs += 501;
    assert(sending.size() <= leds.size());
    for (size_t i = 0; i < sending.size(); i++) leds[i] = sending[i];
    frames.push_back(Frame{nowUs, sending.size(), clearedSinceLastFrame, leds});
    sending.clear();
    clearedSinceLastFrame = false;
}

static void clear() {
    assert(sending.empty());
    nowUs += 120 * 31 + 501;
    for (size_t i = 0; i < leds.size(); i++) leds[i] = Led{0, 0, 0, 0};
    clearedSinceLastFrame = true;
}

// ---------------------------------------------------------------------------------------------
// Fake app and serial link
// ---------------------------------------------------------------------------------------------
struct Request {
    uint64_t tUs;
    char byte;
};

struct ScriptEvent {
    uint64_t atUs;
    std::string message; // what the app would send for '~' from then on
    bool chooserMode;
};

struct FakeApp {
    enum Kind { NEW_APP, OLD_APP } kind;
    std::string message;        // the JSON it would send for '~' right now
    bool chooserMode;
    std::string lastWritten;    // the last full message it wrote to the UART
    uint64_t latencyUs;         // from the request byte to the first byte of the answer
    uint64_t silentFromUs;      // from this time on it does not answer at all
    int damageReplyNumber;      // this reply (counting from 1) arrives damaged; 0 = none
    std::string damagedReply;   // ... as these bytes ("" = cut in half)
    std::vector<ScriptEvent> script;
    std::vector<Request> requests;
    std::vector<std::string> replies;

    void reset(Kind k, const std::string& first) {
        kind = k;
        message = first;
        chooserMode = true;
        lastWritten = first; // the '~' answer that put the sign into the chooser display
        latencyUs = 3000;
        silentFromUs = UINT64_MAX;
        damageReplyNumber = 0;
        damagedReply.clear();
        script.clear();
        requests.clear();
        replies.clear();
    }

    void at(double atMs, const std::string& newMessage, bool stillChooser) {
        script.push_back(ScriptEvent{(uint64_t) (atMs * 1000), newMessage, stillChooser});
    }

    // Returns the bytes that go on the wire for this request ("" = no answer)
    std::string onRequest(char byte) {
        requests.push_back(Request{nowUs, byte});
        while (!script.empty() && script.front().atUs <= nowUs) {
            message = script.front().message;
            chooserMode = script.front().chooserMode;
            script.erase(script.begin());
        }
        if (nowUs >= silentFromUs) return "";

        std::string reply;
        if (kind == NEW_APP && byte == '?' && chooserMode && message == lastWritten) {
            reply = "{}";
        } else {
            reply = message;
            lastWritten = message;
        }
        replies.push_back(reply);
        if ((int) replies.size() == damageReplyNumber) {
            return damagedReply.empty() ? reply.substr(0, reply.size() / 2) : damagedReply;
        }
        return reply;
    }
};

static FakeApp app;

struct FakeSerial {
    bool listening = false;
    unsigned long timeoutMs = 1000;
    std::string incoming;       // bytes on their way to us
    uint64_t firstByteAtUs = 0; // when incoming[0] has fully arrived
    size_t next = 0;

    void setTimeout(unsigned long t) { timeoutMs = t; }

    void print(char byte) {
        assert(listening);
        nowUs += 694; // one byte at 14400 baud
        incoming = app.onRequest(byte);
        next = 0;
        firstByteAtUs = nowUs + app.latencyUs + 694;
    }

    // What ArduinoJson needs from a reader
    int read() {
        assert(listening);
        assert(!interruptsOff); // SoftwareSerial receives in an interrupt
        if (next >= incoming.size()) {
            nowUs += (uint64_t) timeoutMs * 1000;
            return -1;
        }
        uint64_t arrives = firstByteAtUs + next * 694;
        if (arrives > nowUs) {
            if (arrives - nowUs > (uint64_t) timeoutMs * 1000) {
                nowUs += (uint64_t) timeoutMs * 1000;
                return -1;
            }
            nowUs = arrives;
        }
        return (unsigned char) incoming[next++];
    }

    size_t readBytes(char* buffer, size_t length) {
        size_t n = 0;
        while (n < length) {
            int c = read();
            if (c < 0) break;
            buffer[n++] = (char) c;
        }
        return n;
    }
};

static FakeSerial softSerial;
static unsigned long timeoutLeftBehind = 0;

static void startSerial() {
    softSerial.listening = true;
    softSerial.incoming.clear(); // begin() starts with an empty buffer; late bytes were never received
    softSerial.next = 0;
}

static void stopSerial() {
    softSerial.listening = false;
    timeoutLeftBehind = softSerial.timeoutMs;
}

// ---------------------------------------------------------------------------------------------
// The code under test
// ---------------------------------------------------------------------------------------------
#include "../ChooserDisplay.h"

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
static std::string chooserJson(const std::string& text, const std::string& count) {
    return "{\"type\":\"H\",\"str\":\"" + text + "\",\"flashy\":\"" + count + "\"}";
}

static const char* const BYE = "{\"type\":\"D\",\"str\":\"bye\"}";

static std::string repeat(const char* unit, size_t length) {
    std::string s;
    while (s.size() < length) s += unit;
    return s.substr(0, length);
}

// Starts from a clean sign, lets loop()'s '~' read deliver the first message, and runs the display.
static bool runChooser(FakeApp::Kind kind, const std::string& text, const std::string& count) {
    chooserLeft(); // loop() drew something else before this
    nowUs = 0;
    frames.clear();
    sending.clear();
    for (size_t i = 0; i < leds.size(); i++) leds[i] = Led{0x7E, 9, 9, 9}; // leftovers of another effect
    clearedSinceLastFrame = false;
    softSerial = FakeSerial();
    std::string first = chooserJson(text, count);
    std::vector<ScriptEvent> script = app.script;
    uint64_t latency = app.latencyUs, silentFrom = app.silentFromUs;
    int damage = app.damageReplyNumber;
    std::string damaged = app.damagedReply;
    app.reset(kind, first);
    app.script = script;
    app.latencyUs = latency;
    app.silentFromUs = silentFrom;
    app.damageReplyNumber = damage;
    app.damagedReply = damaged;

    DeserializationError err = deserializeJson(json, first);
    assert(err == DeserializationError::Ok);
    bool result = showAsChooser(json["str"], json["flashy"]);
    assert(!softSerial.listening);
    assert(timeoutLeftBehind == 1000); // the normal '~' read gets its usual timeout back
    return result;
}

// The same, with what loop() does when showAsChooser() gives up: ask with a normal '~' (the usual
// one second timeout) and go back in if that brings a chooser message.
static bool runChooserLikeLoop(FakeApp::Kind kind, const std::string& text, const std::string& count) {
    bool other = runChooser(kind, text, count);
    while (!other) {
        startSerial();
        softSerial.print('~');
        DeserializationError err = deserializeJson(json, softSerial);
        stopSerial();
        if (err != DeserializationError::Ok) return false; // loop() shows the starfield
        const char* type = json["type"];
        if (type[0] != MSGTYPE_CHOOSER) {
            chooserLeft();
            return true;
        }
        other = showAsChooser(json["str"], json["flashy"]);
    }
    return true;
}

static void freshApp() {
    app.reset(FakeApp::NEW_APP, "");
}

// The font's column bits for a strip: text, gap, text again... independent of ChooserScroll.h
static std::vector<uint8_t> stripRows(const std::string& text, int gap) {
    std::vector<uint8_t> rows;
    for (size_t i = 0; i < text.size(); i++) {
        unsigned char ch = (unsigned char) text[i];
        int width = ch == 0x7F ? 7 : (ch == 0x0B ? 0 : 5);
        for (int c = 0; c < width; c++) {
            bool inFont = ch >= 0x20 && ch <= 0x7F;
            rows.push_back(inFont ? FontStd5x7[(ch - 0x20) * 5 + c] : 0);
        }
        if (i + 1 < text.size()) rows.push_back(0);
    }
    for (int g = 0; g < gap; g++) rows.push_back(0);
    return rows;
}

// What panel 1 must look like: the count in dim red, a blank column, the text at scroll position pos
static std::vector<Led> expectedPanel(const std::string& text, const std::string& count, int pos, uint8_t level,
                                      bool scrolling) {
    std::vector<Led> panel(60, Led{0, 0, 0, 0});
    std::vector<uint8_t> countRows = stripRows(count, 0);
    size_t x = 0;
    for (; x < countRows.size() && x < 60; x++) panel[x] = Led{countRows[x], 0x80, 0, 0};
    x++; // the blank column
    std::vector<uint8_t> textRows = stripRows(text, scrolling ? CHOOSER_GAP_COLS : 60);
    for (size_t v = 0; x < 60 && !textRows.empty(); x++, v++) {
        panel[x] = Led{textRows[(pos + v) % textRows.size()], level, level, level};
    }
    return panel;
}

static bool panelOneIs(const Frame& f, const std::vector<Led>& expected) {
    for (size_t x = 0; x < 60; x++) {
        if (!(f.leds[x] == expected[x])) return false;
    }
    return true;
}

static bool panelTwoDark(const Frame& f) {
    for (size_t x = 60; x < 120; x++) {
        if (!f.leds[x].dark()) return false;
    }
    return true;
}

// The scroll position a frame shows, or -1 when it shows none of them
static int positionOf(const Frame& f, const std::string& text, const std::string& count, int cycleCols) {
    for (int pos = 0; pos < cycleCols; pos++) {
        if (panelOneIs(f, expectedPanel(text, count, pos, GAMMA(CHOOSER_TEXT_BRIGHTNESS), true))) return pos;
    }
    return -1;
}

static void printFrame(const Frame& f, const char* title) {
    printf("  %s  (t = %.0f ms, %zu columns sent)\n", title, ms(f.tUs), f.columnsSent);
    for (int bit = 7; bit >= 1; bit--) {
        printf("    |");
        for (size_t x = 0; x < 60; x++) {
            const Led& l = f.leds[x];
            bool on = !l.dark() && (l.row & (1 << bit));
            putchar(on ? (l.g == 0 && l.b == 0 ? 'o' : '#') : ' ');
        }
        printf("|\n");
    }
}

static bool wantShow = false;

static int questionMarks() {
    int n = 0;
    for (size_t i = 0; i < app.requests.size(); i++) n += app.requests[i].byte == '?';
    return n;
}

// The longest stretch without a request to the app, in ms
static double longestSilenceMs() {
    uint64_t longest = app.requests.empty() ? 0 : app.requests[0].tUs;
    for (size_t i = 1; i < app.requests.size(); i++) {
        uint64_t gap = app.requests[i].tUs - app.requests[i - 1].tUs;
        if (gap > longest) longest = gap;
    }
    return ms(longest);
}

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

static void previewThatFitsBlinksExactlyAsBefore() {
    freshApp();
    app.at(1000, BYE, false);
    bool other = runChooser(FakeApp::NEW_APP, "HELLO", "3/9");
    assert(other);
    assert(frames.size() >= 58 * 4);

    // every blink: a clear, then 58 frames of count + blank column + text fading to black
    size_t blinks = frames.size() / 58;
    assert(frames.size() == blinks * 58);
    for (size_t i = 0; i < frames.size(); i++) {
        unsigned int count = 57 - (i % 58);
        uint8_t level = GAMMA((count * 256) / 100);
        assert(frames[i].afterClear == (i % 58 == 0));
        assert(frames[i].columnsSent == 17 + 1 + 29); // what the old sendString() calls sent
        assert(panelOneIs(frames[i], expectedPanel("HELLO", "3/9", 0, level, false)));
        assert(panelTwoDark(frames[i]));
    }
    assert(frames[0].leds[0].r == 0x80 && frames[0].leds[0].g == 0); // the count is dim red
    assert(frames[0].leds[18].r == GAMMA(145));                       // the text starts at today's level
    assert(frames[57].leds[18].dark());                               // ... and ends black

    if (wantShow) printFrame(frames[0], "a preview that fits: \"HELLO\" beside \"3/9\" (o = dim red, # = white)");
}

static void previewThatFitsIsPolledWithQuestionMarkBetweenBlinks() {
    freshApp();
    app.at(1000, BYE, false);
    runChooser(FakeApp::NEW_APP, "HELLO", "3/9");
    assert(app.requests.size() >= 4);
    assert(questionMarks() == (int) app.requests.size()); // never '~' while all is well
    for (size_t i = 0; i + 1 < app.replies.size(); i++) assert(app.replies[i] == "{}");
    assert(longestSilenceMs() <= 250);
}

static void leavingChooserHandsTheOtherMessageToLoop() {
    freshApp();
    app.at(1000, BYE, false);
    bool other = runChooser(FakeApp::NEW_APP, "HELLO", "3/9");
    assert(other);
    assert(strcmp(json["type"], "D") == 0);
    assert(strcmp(json["str"], "bye") == 0);
    assert(ms(nowUs) < 1000 + 250);
}

static void longTextHoldsThenScrollsOneColumnPerDelay() {
    std::string text = "PLUR IS NOT DEAD"; // 16 characters = 95 columns, viewport 42
    freshApp();
    app.at(12000, BYE, false);
    assert(runChooser(FakeApp::NEW_APP, text, "3/9"));
    const int cycle = 95 + CHOOSER_GAP_COLS;

    // first frame: the start of the text at the left edge, after a clear
    assert(frames[0].afterClear);
    assert(positionOf(frames[0], text, "3/9", cycle) == 0);

    int expectPos = 0;
    int wraps = 0;
    for (size_t i = 0; i < frames.size(); i++) {
        const Frame& f = frames[i];
        assert(f.columnsSent == 60); // the count, the blank column, the whole viewport: never more
        assert(panelTwoDark(f));
        assert(positionOf(f, text, "3/9", cycle) == expectPos);
        if (i + 1 == frames.size()) break;

        double untilNext = ms(frames[i + 1].tUs - f.tUs);
        if (expectPos == 0) {
            // the hold (the picture is redrawn once at the wrap, so skip to the frame that moves)
            assert(untilNext >= CHOOSER_HOLD_MS && untilNext < CHOOSER_HOLD_MS + 5);
        } else if (expectPos == cycle - 1) {
            // the last scrolled column is on screen for one step, then the picture of position 0
            assert(untilNext >= CHOOSER_SCROLL_DELAY_MS - 1 && untilNext < CHOOSER_SCROLL_DELAY_MS + 2);
            wraps++;
        } else {
            assert(untilNext >= CHOOSER_SCROLL_DELAY_MS - 1 && untilNext < CHOOSER_SCROLL_DELAY_MS + 2);
        }
        expectPos = (expectPos + 1) % cycle;
    }
    assert(wraps >= 2); // 12 s is more than two times round (113 columns x 35 ms + 600 ms hold)

    if (wantShow) {
        printFrame(frames[0], "a long text, held at its start: \"PLUR IS NOT DEAD\" beside \"3/9\"");
        printFrame(frames[20], "20 columns later");
        printFrame(frames[80], "80 columns later: the end, the gap, and the text coming round again");
    }
}

static void longTextIsPolledEverySixColumnsAndDuringTheHold() {
    std::string text = "PLUR IS NOT DEAD";
    freshApp();
    app.at(12000, BYE, false);
    runChooser(FakeApp::NEW_APP, text, "3/9");
    assert(questionMarks() == (int) app.requests.size());
    assert(longestSilenceMs() <= 250);
    // between two polls there are never more than CHOOSER_POLL_EVERY_COLS frames
    size_t f = 0;
    for (size_t r = 0; r + 1 < app.requests.size(); r++) {
        int between = 0;
        while (f < frames.size() && frames[f].tUs <= app.requests[r].tUs) f++;
        for (size_t k = f; k < frames.size() && frames[k].tUs <= app.requests[r + 1].tUs; k++) between++;
        assert(between <= CHOOSER_POLL_EVERY_COLS);
    }
}

static void slowButTimelyAnswerCausesNoHitch() {
    std::string text = "PLUR IS NOT DEAD";
    freshApp();
    app.latencyUs = 25000; // the app takes 25 ms to answer every poll: still inside one 35 ms step
    app.at(6000, BYE, false);
    runChooser(FakeApp::NEW_APP, text, "3/9");
    const int cycle = 95 + CHOOSER_GAP_COLS;
    for (size_t i = 0; i + 1 < frames.size(); i++) {
        int pos = positionOf(frames[i], text, "3/9", cycle);
        if (pos == 0) continue; // the hold
        double untilNext = ms(frames[i + 1].tUs - frames[i].tUs);
        assert(untilNext < CHOOSER_SCROLL_DELAY_MS + 2);
    }
}

static void sixtyFourCharacterTextBesideAFiveCharacterCount() {
    std::string text = repeat("THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG. ", 64);
    freshApp();
    app.at(16000, BYE, false);
    assert(runChooser(FakeApp::NEW_APP, text, "12/34"));
    const int cycle = 64 * 6 - 1 + CHOOSER_GAP_COLS; // 401
    int expectPos = 0;
    bool wrapped = false;
    for (size_t i = 0; i < frames.size(); i++) {
        assert(frames[i].columnsSent == 60);
        assert(panelTwoDark(frames[i]));
        assert(panelOneIs(frames[i], expectedPanel(text, "12/34", expectPos, GAMMA(CHOOSER_TEXT_BRIGHTNESS), true)));
        expectPos = (expectPos + 1) % cycle;
        wrapped = wrapped || (expectPos == 0);
    }
    assert(wrapped); // 401 columns x 35 ms = 14 s: the whole message has gone past once
}

static void newMessageMidScrollRestartsAtTheHold() {
    std::string first = "PLUR IS NOT DEAD";
    std::string second = "SECOND MESSAGE, ALSO LONG";
    freshApp();
    app.at(2000, chooserJson(second, "4/9"), true); // "!next" at t = 2 s, in the middle of the scroll
    app.at(5000, BYE, false);
    assert(runChooser(FakeApp::NEW_APP, first, "3/9"));

    size_t change = 1; // frames[0] is after a clear too
    while (change < frames.size() && !frames[change].afterClear) change++;
    assert(change < frames.size());
    const Frame& restart = frames[change];
    // on screen within one poll interval (6 columns) plus the transfer of the new message
    assert(ms(restart.tUs) > 2000 && ms(restart.tUs) < 2000 + 6 * 36 + 100);
    assert(positionOf(restart, second, "4/9", 25 * 6 - 1 + CHOOSER_GAP_COLS) == 0);
    assert(panelTwoDark(restart));
    // it was mid-scroll before, not at the start
    assert(positionOf(frames[change - 1], first, "3/9", 95 + CHOOSER_GAP_COLS) > 10);
    // and the new message holds before it moves
    double hold = ms(frames[change + 1].tUs - restart.tUs);
    assert(hold >= CHOOSER_HOLD_MS && hold < CHOOSER_HOLD_MS + 5);
    assert(positionOf(frames[change + 1], second, "4/9", 25 * 6 - 1 + CHOOSER_GAP_COLS) == 1);
    assert(questionMarks() == (int) app.requests.size());
}

static void changingOnlyTheCountRestartsToo() {
    std::string text = "PLUR IS NOT DEAD";
    freshApp();
    app.at(2000, chooserJson(text, "3/8"), true); // another message was deleted: same text, new count
    app.at(4000, BYE, false);
    assert(runChooser(FakeApp::NEW_APP, text, "3/9"));
    size_t change = 1;
    while (change < frames.size() && !frames[change].afterClear) change++;
    assert(change < frames.size());
    assert(positionOf(frames[change], text, "3/8", 95 + CHOOSER_GAP_COLS) == 0);
}

static void scrollingToShortMessageGoesBackToTheBlink() {
    freshApp();
    app.at(2000, chooserJson("HI", "4/9"), true);
    app.at(3000, BYE, false);
    assert(runChooser(FakeApp::NEW_APP, "PLUR IS NOT DEAD", "3/9"));
    const Frame& last = frames.back();
    assert(last.columnsSent == 17 + 1 + 11);
    // nothing of the long text is left on the panel beside the short one
    assert(panelOneIs(last, expectedPanel("HI", "4/9", 0, 0, false)));
    assert(panelTwoDark(last));
}

static void oldAppAnsweringWithTheWholeMessageKeepsTheScrollPosition() {
    // an app that does not know '?' sends the whole chooser message for every request
    std::string text = "PLUR IS NOT DEAD";
    freshApp();
    app.at(8000, BYE, false);
    assert(runChooser(FakeApp::OLD_APP, text, "3/9"));
    for (size_t i = 0; i < app.replies.size(); i++) assert(app.replies[i] != "{}");
    const int cycle = 95 + CHOOSER_GAP_COLS;
    int expectPos = 0;
    for (size_t i = 0; i < frames.size(); i++) {
        assert(positionOf(frames[i], text, "3/9", cycle) == expectPos); // never jumps back to the start
        assert(!frames[i].afterClear || i == 0);
        expectPos = (expectPos + 1) % cycle;
    }
    assert(expectPos > 30 || frames.size() > (size_t) cycle);
}

static void oldAppWithSevenCharacterPreviewLooksLikeToday() {
    freshApp();
    app.at(1000, BYE, false);
    assert(runChooser(FakeApp::OLD_APP, "PLUR IS", "3/9")); // 7 characters = 41 columns in a 42 column viewport
    for (size_t i = 0; i < frames.size(); i++) {
        assert(frames[i].columnsSent == 17 + 1 + 41);
        assert(frames[i].afterClear == (i % 58 == 0));
    }
}

static void silentAppKeepsScrollingAndIsGivenUpOnAfterTwentyFailedRequests() {
    std::string text = "PLUR IS NOT DEAD";
    freshApp();
    app.silentFromUs = 1000 * 1000; // the app dies after 1 s
    bool other = runChooser(FakeApp::NEW_APP, text, "3/9");
    assert(!other); // loop() goes back to a normal '~'

    size_t firstUnanswered = 0;
    while (app.requests[firstUnanswered].tUs < app.silentFromUs) firstUnanswered++;
    assert(app.requests.size() - firstUnanswered == CHOOSER_MAX_FAILED_POLLS);
    assert(app.requests[firstUnanswered].byte == '?');
    for (size_t i = firstUnanswered + 1; i < app.requests.size(); i++) assert(app.requests[i].byte == '~');

    // it kept scrolling meanwhile, and no single step stalled longer than the poll timeout
    const int cycle = 95 + CHOOSER_GAP_COLS;
    int moved = 0;
    for (size_t i = 0; i + 1 < frames.size(); i++) {
        if (frames[i].tUs < app.silentFromUs) continue;
        int a = positionOf(frames[i], text, "3/9", cycle), b = positionOf(frames[i + 1], text, "3/9", cycle);
        assert(b == (a + 1) % cycle);
        moved++;
        if (a != 0) assert(ms(frames[i + 1].tUs - frames[i].tUs) < CHOOSER_POLL_TIMEOUT_MS + 5);
    }
    assert(moved >= 2 * 4); // about five columns per failed request
    assert(ms(nowUs) < 1000 + 6000); // gives up within a few seconds
}

static void lostAnswerIsAskedForAgainWithTilde() {
    std::string first = "PLUR IS NOT DEAD";
    std::string second = "SECOND MESSAGE, ALSO LONG";
    freshApp();
    app.at(2000, chooserJson(second, "4/9"), true);
    app.at(5000, BYE, false);
    // find out which reply carries the new message, then run again with that reply cut in half
    runChooser(FakeApp::NEW_APP, first, "3/9");
    int carrier = 0;
    for (size_t i = 0; i < app.replies.size(); i++) {
        if (app.replies[i] == chooserJson(second, "4/9")) carrier = (int) i + 1;
    }
    assert(carrier > 0);

    freshApp();
    app.at(2000, chooserJson(second, "4/9"), true);
    app.at(5000, BYE, false);
    app.damageReplyNumber = carrier;
    assert(runChooser(FakeApp::NEW_APP, first, "3/9"));

    // the app now believes the sign has the new message and would say "{}" to '?': the sign asks '~'
    assert(app.requests[carrier - 1].byte == '?');
    assert(app.requests[carrier].byte == '~');
    assert(app.replies[carrier] == chooserJson(second, "4/9"));
    assert(app.requests[carrier + 1].byte == '?'); // and goes back to '?' once a read worked
    size_t change = 1;
    while (change < frames.size() && !frames[change].afterClear) change++;
    assert(change < frames.size());
    assert(positionOf(frames[change], second, "4/9", 25 * 6 - 1 + CHOOSER_GAP_COLS) == 0);
    assert(ms(frames[change].tUs) < 2000 + 2 * (6 * 36 + 100));
}

static void damagedObjectWithoutTypeIsNotTakenForNothingChanged() {
    std::string text = "PLUR IS NOT DEAD";
    freshApp();
    app.at(3000, BYE, false);
    app.damageReplyNumber = 3;
    app.damagedReply = "{\"tipe\":\"H\",\"str\":\"X\",\"flashy\":\"1/9\"}"; // parses, but is not a message
    assert(runChooser(FakeApp::NEW_APP, text, "3/9"));
    assert(app.requests[2].byte == '?');
    assert(app.requests[3].byte == '~');
    assert(app.requests[4].byte == '?');
    // the picture did not change
    for (size_t i = 1; i < frames.size(); i++) assert(!frames[i].afterClear);
}

static void neverMoreThanSixtyColumnsWhateverTheCountAndText() {
    const char* counts[] = {"", "1", "3/9", "3/12", "12/34", "12/345", "123/456", "1234/5678", "12345/6789",
                            "123456789012", "\x7F\x7F\x7F\x7F\x7F\x7F\x7F\x7F"};
    const char* texts[] = {"", "A", "HELLO", "PLUR IS", "PLUR IS NOT DEAD", "\x7F LOVE \x7F", "caf\xC3\xA9 \x01\x0B"};
    for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        for (size_t t = 0; t < sizeof(texts) / sizeof(texts[0]); t++) {
            freshApp();
            app.at(1500, BYE, false);
            assert(runChooser(FakeApp::NEW_APP, texts[t], counts[c]));
            assert(!frames.empty());
            for (size_t i = 0; i < frames.size(); i++) {
                assert(frames[i].columnsSent <= 60);
                assert(panelTwoDark(frames[i]));
            }
            assert(longestSilenceMs() <= 250);
        }
    }
}

static void textFromTheAppLongerThanSixtyFourCharactersIsCutNotOverrun() {
    std::string text = repeat("0123456789", 100);
    freshApp();
    app.at(1000, BYE, false);
    assert(runChooser(FakeApp::NEW_APP, text, "3/9"));
    assert(strlen(chooser.text) == CHOOSER_MAX_CHARS);
    // the cut copy equals what the app keeps sending: no restart on every poll
    for (size_t i = 1; i < frames.size(); i++) assert(!frames[i].afterClear);
}

static void escapedSixtyFourCharacterMessageArrivesWholeOnAPoll() {
    // The app's Gson writes ' < > & = as six-byte \uXXXX escapes: the longest message is about
    // 420 bytes on the wire (0.3 s at 14400 baud) and must still be read on a short-timeout poll.
    std::string escaped, decoded;
    const char* const units[] = {"\\u003c", "\\u003e", "\\u0026", "\\u003d", "\\u0027"};
    const char plain[] = {'<', '>', '&', '=', '\''};
    for (int i = 0; i < 64; i++) {
        escaped += units[i % 5];
        decoded += plain[i % 5];
    }
    std::string wire = chooserJson(escaped, "\\u003cempty\\u003e");
    assert(wire.size() > 400);

    freshApp();
    app.at(1000, wire, true);
    app.at(4000, BYE, false);
    assert(runChooser(FakeApp::NEW_APP, "HELLO", "3/9"));
    assert(decoded == chooser.text);
    assert(strcmp(chooser.count, "<empty>") == 0);
    assert(chooser.failedPolls == 0);

    size_t change = 0;
    while (change < frames.size() && frames[change].columnsSent != 60) change++;
    assert(change < frames.size());
    assert(ms(frames[change].tUs) < 1000 + 250 + 400); // one poll interval plus the transfer
    int expectPos = 0;
    for (size_t i = change; i < frames.size(); i++) {
        // "<empty>" is 41 columns wide: the viewport is the last 18 columns
        assert(panelOneIs(frames[i], expectedPanel(decoded, "<empty>", expectPos, GAMMA(CHOOSER_TEXT_BRIGHTNESS), true)));
        assert(panelTwoDark(frames[i]));
        expectPos++;
    }
    assert(expectPos > 30);
}

static void appSlowerThanThePollTimeoutKeepsItsPlaceInTheScroll() {
    // Every answer comes 80 ms after the request: too late for a poll (60 ms), fine for the normal
    // read. The sign must not restart the scroll every time it falls back to the normal read.
    std::string first = "PLUR IS NOT DEAD";
    std::string second = "SECOND MESSAGE, ALSO LONG";
    const int cycle = 95 + CHOOSER_GAP_COLS;
    freshApp();
    app.latencyUs = 80000;
    app.at(9000, chooserJson(second, "4/9"), true);
    app.at(13000, BYE, false);
    assert(runChooserLikeLoop(FakeApp::NEW_APP, first, "3/9"));
    assert(strcmp(json["str"], "bye") == 0);
    assert(ms(nowUs) < 13000 + 1500);

    size_t change = 1;
    while (change < frames.size() && !frames[change].afterClear) change++;
    assert(change < frames.size());
    // the first message scrolled on without ever jumping back, all the way round
    int expectPos = 0;
    for (size_t i = 0; i < change; i++) {
        assert(positionOf(frames[i], first, "3/9", cycle) == expectPos);
        expectPos = (expectPos + 1) % cycle;
    }
    assert(change > (size_t) cycle);
    // the new message was picked up by a normal read within about a second
    assert(positionOf(frames[change], second, "4/9", 25 * 6 - 1 + CHOOSER_GAP_COLS) == 0);
    assert(ms(frames[change].tUs) > 9000 && ms(frames[change].tUs) < 9000 + 1200);
    int tildes = (int) app.requests.size() - questionMarks();
    assert(tildes > 20);
}

static void sameMessageAfterAnotherDisplayStartsFresh() {
    // chooser, then something else on the LEDs, then the same chooser message again
    std::string text = "PLUR IS NOT DEAD";
    freshApp();
    app.at(3000, BYE, false);
    assert(runChooser(FakeApp::NEW_APP, text, "3/9"));
    assert(positionOf(frames.back(), text, "3/9", 95 + CHOOSER_GAP_COLS) > 30);
    freshApp();
    app.at(3000, BYE, false);
    assert(runChooser(FakeApp::NEW_APP, text, "3/9")); // runChooser() scribbles over the LEDs first
    assert(frames[0].afterClear);
    assert(positionOf(frames[0], text, "3/9", 95 + CHOOSER_GAP_COLS) == 0);
    assert(panelTwoDark(frames[0]));
}

static void bytesOutsideTheFontAreDrawnAsSpaces() {
    // a control character and a UTF-8 pair must not read font data from somewhere else
    std::string text = "A\x01\xC3\xA9" "B";
    freshApp();
    app.at(500, BYE, false);
    assert(runChooser(FakeApp::NEW_APP, text, "3/9"));
    assert(frames[0].columnsSent == 17 + 1 + 29);
    assert(panelOneIs(frames[0], expectedPanel("A   B", "3/9", 0, GAMMA(145), false)));
}

static void failedRequestsOfAnEarlierVisitAreForgotten() {
    freshApp();
    app.silentFromUs = 0;
    assert(!runChooser(FakeApp::NEW_APP, "HELLO", "3/9")); // gave up
    freshApp();
    app.at(1000, BYE, false);
    assert(runChooser(FakeApp::NEW_APP, "HELLO", "3/9"));
    assert(app.requests[0].byte == '?');
    assert(questionMarks() == (int) app.requests.size());
}

int main(int argc, char** argv) {
    wantShow = argc > 1 && strcmp(argv[1], "--show") == 0;
    int tests = 0;
#define RUN(test) do { test(); tests++; } while (0)
    RUN(previewThatFitsBlinksExactlyAsBefore);
    RUN(previewThatFitsIsPolledWithQuestionMarkBetweenBlinks);
    RUN(leavingChooserHandsTheOtherMessageToLoop);
    RUN(longTextHoldsThenScrollsOneColumnPerDelay);
    RUN(longTextIsPolledEverySixColumnsAndDuringTheHold);
    RUN(slowButTimelyAnswerCausesNoHitch);
    RUN(sixtyFourCharacterTextBesideAFiveCharacterCount);
    RUN(newMessageMidScrollRestartsAtTheHold);
    RUN(changingOnlyTheCountRestartsToo);
    RUN(scrollingToShortMessageGoesBackToTheBlink);
    RUN(oldAppAnsweringWithTheWholeMessageKeepsTheScrollPosition);
    RUN(oldAppWithSevenCharacterPreviewLooksLikeToday);
    RUN(silentAppKeepsScrollingAndIsGivenUpOnAfterTwentyFailedRequests);
    RUN(lostAnswerIsAskedForAgainWithTilde);
    RUN(damagedObjectWithoutTypeIsNotTakenForNothingChanged);
    RUN(neverMoreThanSixtyColumnsWhateverTheCountAndText);
    RUN(textFromTheAppLongerThanSixtyFourCharactersIsCutNotOverrun);
    RUN(escapedSixtyFourCharacterMessageArrivesWholeOnAPoll);
    RUN(appSlowerThanThePollTimeoutKeepsItsPlaceInTheScroll);
    RUN(sameMessageAfterAnotherDisplayStartsFresh);
    RUN(bytesOutsideTheFontAreDrawnAsSpaces);
    RUN(failedRequestsOfAnEarlierVisitAreForgotten);
    printf("chooser_display_test: all %d tests passed\n", tests);
    return 0;
}
