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
#include <deque>
#include <map>
#include <set>
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
//
// The link is a queue of bytes with arrival times. A reply is put on it when the app answers and
// stays there: it is NOT thrown away when the sign sends its next request. So a reply can come
// late, into the read window of a later request, ahead of that request's own answer. What the
// real link loses is lost here too: a byte that arrives while the sign is not listening is gone.
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

struct WireByte {
    uint64_t atUs; // when the byte has fully arrived
    char c;
};

static std::deque<WireByte> wire;
static uint64_t wireBusyUntilUs = 0;

// Puts bytes on the link, the first one arriving at startUs or right behind what is already on it.
static void sendOnWire(const std::string& bytes, uint64_t startUs) {
    uint64_t at = startUs > wireBusyUntilUs + 694 ? startUs : wireBusyUntilUs + 694;
    for (size_t i = 0; i < bytes.size(); i++) {
        wire.push_back(WireByte{at, bytes[i]});
        wireBusyUntilUs = at;
        at += 694;
    }
}

// CRC-16/CCITT-FALSE the way the app might write it: byte at a time, no bit loop.
// Deliberately not the code in ChooserScroll.h.
static uint16_t appCrcStep(uint16_t crc, uint8_t byte) {
    uint8_t x = (uint8_t) ((crc >> 8) ^ byte);
    x ^= (uint8_t) (x >> 4);
    return (uint16_t) ((crc << 8) ^ ((uint16_t) x << 12) ^ ((uint16_t) x << 5) ^ (uint16_t) x);
}

// The checksum of a chooser message as the app computes it: from the strings it serialised.
static uint16_t appChecksum(const std::string& chooserMessage) {
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, chooserMessage);
    assert(err == DeserializationError::Ok);
    std::string text = doc["str"] | "";
    std::string count = doc["flashy"] | "";
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < text.size(); i++) crc = appCrcStep(crc, (uint8_t) text[i]);
    crc = appCrcStep(crc, 0);
    for (size_t i = 0; i < count.size(); i++) crc = appCrcStep(crc, (uint8_t) count[i]);
    return crc;
}

static std::string report(uint16_t checksum) {
    return "{\"c\":" + std::to_string(checksum) + "}";
}

static bool isReport(const std::string& reply) {
    return reply.compare(0, 5, "{\"c\":") == 0;
}

struct Late {
    int afterRequests; // delivered at the request that comes this many requests later (1 = the next one)
    uint64_t offsetUs; // ... this long after that request's byte
};

struct Held {
    int countdown;
    uint64_t offsetUs;
    std::string bytes;
};

struct FakeApp {
    // V2_APP: contract version 2: '?' in chooser mode is answered with {"c":N}, the checksum of the
    //         message it holds now. It remembers nothing about what it wrote.
    // V1_APP: contract version 1: '?' is answered with {} while the message equals the last one it
    //         wrote. (The gate's sequence is against this app.)
    // OLD_APP: today's app: every request is answered with the whole message.
    enum Kind { V2_APP, V1_APP, OLD_APP } kind;
    std::string message;        // the JSON it would send for '~' right now
    bool chooserMode;
    std::string lastWritten;    // V1_APP only: the last full message it wrote to the UART
    uint64_t latencyUs;         // from the request byte to the first byte of the answer
    uint64_t silentFromUs;      // from this time on it does not answer at all
    // What happens to the answer to request number n (counting from 1):
    std::map<int, std::string> damaged; // arrives as these bytes instead ("" = cut in half)
    std::map<int, Late> late;           // is held back and arrives during a later request
    std::set<int> dropped;              // never arrives
    std::vector<Held> held;
    std::vector<ScriptEvent> script;
    std::vector<Request> requests;
    std::vector<std::string> replies;   // what the app wrote for each request ("" = nothing)

    // Forgets every setting of the test before.
    void reset() {
        kind = V2_APP;
        message.clear();
        chooserMode = true;
        lastWritten.clear();
        latencyUs = 3000;
        silentFromUs = UINT64_MAX;
        damaged.clear();
        late.clear();
        dropped.clear();
        held.clear();
        script.clear();
        requests.clear();
        replies.clear();
    }

    // The '~' answer that put the sign into the chooser display has just been written.
    void begin(Kind k, const std::string& first) {
        kind = k;
        message = first;
        chooserMode = true;
        lastWritten = first;
        held.clear();
        requests.clear();
        replies.clear();
    }

    void at(double atMs, const std::string& newMessage, bool stillChooser) {
        script.push_back(ScriptEvent{(uint64_t) (atMs * 1000), newMessage, stillChooser});
    }

    // The sign has sent a request byte: put on the link whatever arrives because of it.
    void onRequest(char byte) {
        requests.push_back(Request{nowUs, byte});
        int number = (int) requests.size();
        while (!script.empty() && script.front().atUs <= nowUs) {
            message = script.front().message;
            chooserMode = script.front().chooserMode;
            script.erase(script.begin());
        }

        // answers held back from earlier requests come first
        for (size_t i = 0; i < held.size();) {
            if (--held[i].countdown == 0) {
                sendOnWire(held[i].bytes, nowUs + held[i].offsetUs);
                held.erase(held.begin() + (long) i);
            } else {
                i++;
            }
        }

        std::string reply;
        if (nowUs < silentFromUs) {
            if (kind == V2_APP && byte == '?' && chooserMode) {
                reply = report(appChecksum(message));
            } else if (kind == V1_APP && byte == '?' && chooserMode && message == lastWritten) {
                reply = "{}";
            } else {
                reply = message;
                lastWritten = message;
            }
        }
        replies.push_back(reply);
        if (reply.empty() || dropped.count(number)) return;
        if (damaged.count(number)) {
            reply = damaged[number].empty() ? reply.substr(0, reply.size() / 2) : damaged[number];
        }
        if (late.count(number)) {
            held.push_back(Held{late[number].afterRequests, late[number].offsetUs, reply});
            return;
        }
        sendOnWire(reply, nowUs + latencyUs + 694);
    }
};

static FakeApp app;

struct FakeSerial {
    bool listening = false;
    unsigned long timeoutMs = 1000;

    void setTimeout(unsigned long t) { timeoutMs = t; }

    void print(char byte) {
        assert(listening);
        nowUs += 694; // one byte at 14400 baud
        app.onRequest(byte);
    }

    // What ArduinoJson needs from a reader
    int read() {
        assert(listening);
        assert(!interruptsOff); // SoftwareSerial receives in an interrupt
        uint64_t giveUpAtUs = nowUs + (uint64_t) timeoutMs * 1000;
        if (wire.empty() || wire.front().atUs > giveUpAtUs) {
            nowUs = giveUpAtUs;
            return -1;
        }
        if (wire.front().atUs > nowUs) nowUs = wire.front().atUs;
        char c = wire.front().c;
        wire.pop_front();
        return (unsigned char) c;
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
    // begin() starts with an empty buffer: what arrived while we were not listening was never received
    while (!wire.empty() && wire.front().atUs <= nowUs) wire.pop_front();
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

// Puts the sign and the link into the state loop() is in when its '~' read has just brought the
// chooser message: something else on the LEDs, the message in `json`.
static void arrive(FakeApp::Kind kind, const std::string& text, const std::string& count) {
    chooserLeft(); // loop() drew something else before this
    nowUs = 0;
    frames.clear();
    sending.clear();
    for (size_t i = 0; i < leds.size(); i++) leds[i] = Led{0x7E, 9, 9, 9}; // leftovers of another effect
    clearedSinceLastFrame = false;
    softSerial = FakeSerial();
    wire.clear();
    wireBusyUntilUs = 0;
    std::string first = chooserJson(text, count);
    app.begin(kind, first);
    DeserializationError err = deserializeJson(json, first);
    assert(err == DeserializationError::Ok);
}

// Runs showAsChooser() once, as loop() calls it. True: `json` holds a message of another type.
// False: it gave up after too many failed reads.
static bool runChooser(FakeApp::Kind kind, const std::string& text, const std::string& count) {
    arrive(kind, text, count);
    bool result = showAsChooser(json["str"], json["flashy"]);
    assert(!softSerial.listening);
    assert(timeoutLeftBehind == 1000); // the normal '~' read gets its usual timeout back
    return result;
}

static size_t framesAtNormalRead = 0; // frames latched when loop()'s last normal read began

// The whole of loop() as far as the chooser goes, with the sketch's own code for the two decisions
// (chooserReadMessage() inside readSerialData(), chooserDispatch() in loop()). Returns the type of
// the message loop() would show next, or 0 when its normal read got nothing (the starfield).
static char runLoop(FakeApp::Kind kind, const std::string& text, const std::string& count) {
    arrive(kind, text, count);
    for (;;) {
        char type = chooserDispatch();
        if (type) return type;
        // nothing to show: loop() ends and readSerialData() asks again
        framesAtNormalRead = frames.size();
        startSerial();
        softSerial.print('~');
        DeserializationError err = chooserReadMessage();
        stopSerial();
        if (err != DeserializationError::Ok) return 0;
    }
}

static void freshApp() {
    app.reset();
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
    bool other = runChooser(FakeApp::V2_APP, "HELLO", "3/9");
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
    runChooser(FakeApp::V2_APP, "HELLO", "3/9");
    assert(app.requests.size() >= 4);
    assert(questionMarks() == (int) app.requests.size()); // never '~' while all is well
    // every answer but the last (which ends chooser mode) is the checksum of the message on the sign
    for (size_t i = 0; i + 1 < app.replies.size(); i++) {
        assert(app.replies[i] == report(chooserCrc("HELLO", "3/9")));
    }
    assert(longestSilenceMs() <= 250);
}

static void leavingChooserHandsTheOtherMessageToLoop() {
    freshApp();
    app.at(1000, BYE, false);
    bool other = runChooser(FakeApp::V2_APP, "HELLO", "3/9");
    assert(other);
    assert(strcmp(json["type"], "D") == 0);
    assert(strcmp(json["str"], "bye") == 0);
    assert(ms(nowUs) < 1000 + 250);
}

static void longTextHoldsThenScrollsOneColumnPerDelay() {
    std::string text = "PLUR IS NOT DEAD"; // 16 characters = 95 columns, viewport 42
    freshApp();
    app.at(12000, BYE, false);
    assert(runChooser(FakeApp::V2_APP, text, "3/9"));
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
    runChooser(FakeApp::V2_APP, text, "3/9");
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
    runChooser(FakeApp::V2_APP, text, "3/9");
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
    assert(runChooser(FakeApp::V2_APP, text, "12/34"));
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
    assert(runChooser(FakeApp::V2_APP, first, "3/9"));

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
    // the report that did not match was followed at once by one '~' for the message itself
    int tilde = -1;
    for (size_t i = 0; i < app.requests.size(); i++) {
        if (app.requests[i].byte == '~') {
            assert(tilde < 0);
            tilde = (int) i;
        }
    }
    assert(tilde > 0);
    assert(app.replies[tilde - 1] == report(chooserCrc(second.c_str(), "4/9")));
    assert(app.replies[tilde] == chooserJson(second, "4/9"));
    assert(ms(app.requests[tilde].tUs - app.requests[tilde - 1].tUs) < 20); // not at the next poll slot
}

static void changingOnlyTheCountRestartsToo() {
    std::string text = "PLUR IS NOT DEAD";
    freshApp();
    app.at(2000, chooserJson(text, "3/8"), true); // another message was deleted: same text, new count
    app.at(4000, BYE, false);
    assert(runChooser(FakeApp::V2_APP, text, "3/9"));
    size_t change = 1;
    while (change < frames.size() && !frames[change].afterClear) change++;
    assert(change < frames.size());
    assert(positionOf(frames[change], text, "3/8", 95 + CHOOSER_GAP_COLS) == 0);
}

static void scrollingToShortMessageGoesBackToTheBlink() {
    freshApp();
    app.at(2000, chooserJson("HI", "4/9"), true);
    app.at(3000, BYE, false);
    assert(runChooser(FakeApp::V2_APP, "PLUR IS NOT DEAD", "3/9"));
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
    for (size_t i = 0; i < app.replies.size(); i++) assert(!isReport(app.replies[i]));
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
    bool other = runChooser(FakeApp::V2_APP, text, "3/9");
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
    runChooser(FakeApp::V2_APP, first, "3/9");
    int carrier = 0;
    for (size_t i = 0; i < app.replies.size(); i++) {
        if (app.replies[i] == chooserJson(second, "4/9")) carrier = (int) i + 1;
    }
    assert(carrier > 0);

    freshApp();
    app.at(2000, chooserJson(second, "4/9"), true);
    app.at(5000, BYE, false);
    app.damaged[carrier] = "";
    assert(runChooser(FakeApp::V2_APP, first, "3/9"));

    // the '?' whose report did not match, the '~' whose answer was cut in half, and the '~' after it
    assert(app.requests[carrier - 2].byte == '?');
    assert(app.requests[carrier - 1].byte == '~');
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
    app.damaged[3] = "{\"tipe\":\"H\",\"str\":\"X\",\"flashy\":\"1/9\"}"; // parses, but is not a message
    app.damaged[7] = "{\"c\":\"12\"}";  // a report whose checksum is not a number
    app.damaged[11] = "{\"c\":1.5}";    // ... or not a whole number
    app.damaged[15] = "[1,2]";          // not an object at all
    app.script.clear();
    app.at(6000, BYE, false);
    assert(runChooser(FakeApp::V2_APP, text, "3/9"));
    for (int n = 3; n <= 15; n += 4) {
        // a failed read: the next request is '~', and after its good answer '?' again
        assert(app.requests[n - 1].byte == '?');
        assert(app.requests[n].byte == '~');
        assert(app.requests[n + 1].byte == '?');
        // ... at the next poll, not at once: it was not read as a report of another message
        assert(ms(app.requests[n].tUs - app.requests[n - 1].tUs) > 100);
    }
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
            assert(runChooser(FakeApp::V2_APP, texts[t], counts[c]));
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
    assert(runChooser(FakeApp::V2_APP, text, "3/9"));
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
    assert(runChooser(FakeApp::V2_APP, "HELLO", "3/9"));
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

static void escapedTextHasTheChecksumOfItsDecodedBytes() {
    // The app's Gson is html-safe: ' < > & = go out as \uXXXX, while its checksum is over the text
    // itself. The two agree only if the sign stores the decoded bytes. If they did not, every
    // report would differ and the sign would re-read the whole message at every poll.
    // (1) the list-empty text, one of the contract's vectors
    freshApp();
    app.at(1500, BYE, false);
    assert(runChooser(FakeApp::V2_APP, "\\u003cempty\\u003e", "1/0"));
    assert(strcmp(chooser.text, "<empty>") == 0);
    assert(chooser.crc == 7805);
    assert(app.requests.size() > 5);
    for (size_t i = 0; i + 1 < app.requests.size(); i++) {
        assert(app.requests[i].byte == '?');          // never a '~': nothing is read again
        assert(app.replies[i] == "{\"c\":7805}");
    }
    assert(panelOneIs(frames[0], expectedPanel("<empty>", "1/0", 0, GAMMA(145), false)));

    // (2) a scrolling text with an apostrophe, an ampersand and an equals sign
    freshApp();
    app.at(3000, BYE, false);
    assert(runChooser(FakeApp::V2_APP, "DON\\u0027T STOP \\u0026 DANCE, 1+1\\u003d2", "4/4"));
    assert(strcmp(chooser.text, "DON'T STOP & DANCE, 1+1=2") == 0);
    assert(chooser.crc == chooserCrc("DON'T STOP & DANCE, 1+1=2", "4/4"));
    assert(app.requests.size() > 10);
    for (size_t i = 0; i < app.requests.size(); i++) assert(app.requests[i].byte == '?');
    for (size_t i = 1; i < frames.size(); i++) assert(!frames[i].afterClear);
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
    assert(runLoop(FakeApp::V2_APP, first, "3/9") == 'D');
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
    assert(runChooser(FakeApp::V2_APP, text, "3/9"));
    assert(positionOf(frames.back(), text, "3/9", 95 + CHOOSER_GAP_COLS) > 30);
    freshApp();
    app.at(3000, BYE, false);
    assert(runChooser(FakeApp::V2_APP, text, "3/9")); // runChooser() scribbles over the LEDs first
    assert(frames[0].afterClear);
    assert(positionOf(frames[0], text, "3/9", 95 + CHOOSER_GAP_COLS) == 0);
    assert(panelTwoDark(frames[0]));
}

static void bytesOutsideTheFontAreDrawnAsSpaces() {
    // a control character and a UTF-8 pair must not read font data from somewhere else
    std::string text = "A\x01\xC3\xA9" "B";
    freshApp();
    app.at(500, BYE, false);
    assert(runChooser(FakeApp::V2_APP, text, "3/9"));
    assert(frames[0].columnsSent == 17 + 1 + 29);
    assert(panelOneIs(frames[0], expectedPanel("A   B", "3/9", 0, GAMMA(145), false)));
}

static void failedRequestsOfAnEarlierVisitAreForgotten() {
    freshApp();
    app.silentFromUs = 0;
    assert(!runChooser(FakeApp::V2_APP, "HELLO", "3/9")); // gave up
    freshApp();
    app.at(1000, BYE, false);
    assert(runChooser(FakeApp::V2_APP, "HELLO", "3/9"));
    assert(app.requests[0].byte == '?');
    assert(questionMarks() == (int) app.requests.size());
}

// ---------------------------------------------------------------------------------------------
// Late and out-of-order replies (what the review gate found in round 1)
// ---------------------------------------------------------------------------------------------

// The texts on the sign each time it started a message afresh (a clear, then the start of a text).
static std::vector<std::string> restarts(const std::vector<std::pair<std::string, std::string> >& known) {
    std::vector<std::string> seen;
    for (size_t i = 1; i < frames.size(); i++) {
        if (!frames[i].afterClear) continue;
        std::string which = "?";
        for (size_t k = 0; k < known.size(); k++) {
            int cycle = (int) known[k].first.size() * 6 - 1 + CHOOSER_GAP_COLS;
            if (positionOf(frames[i], known[k].first, known[k].second, cycle) == 0) which = known[k].first;
        }
        seen.push_back(which);
    }
    return seen;
}

// The gate's sequence, word for word: the sign shows FIRST MESSAGE 1/2. The answer to its first '?'
// is withheld until after the 60 ms timeout. SECOND MESSAGE 2/2 is selected before the next
// request. When the sign sends its recovery '~', the old answer arrives after 5 ms and the full new
// message after 20 ms.
static void gateSequence(FakeApp::Kind kind) {
    std::string first = "FIRST MESSAGE", second = "SECOND MESSAGE";
    freshApp();
    app.latencyUs = 20000;
    app.late[1] = Late{1, 5000};
    app.at(300, chooserJson(second, "2/2"), true); // the first '?' goes out at 200 ms, the next request at 400
    app.at(4000, BYE, false);
    assert(runLoop(kind, first, "1/2") == 'D');
    assert(!chooser.onScreen); // loop() is about to draw the other message over the chooser
    assert(ms(app.requests[0].tUs) < 300 - CHOOSER_POLL_TIMEOUT_MS);

    assert(app.requests[0].byte == '?');
    assert(app.replies[0] == (kind == FakeApp::V1_APP ? std::string("{}") : report(chooserCrc("FIRST MESSAGE", "1/2"))));
    assert(app.requests[1].byte == '~');
    assert(ms(app.requests[1].tUs) > 300);                  // the selection changed before the recovery request
    assert(app.replies[1] == chooserJson(second, "2/2"));   // the app has now sent the new message once

    // the sign ends on the new message, soon after
    assert(strcmp(chooser.text, "SECOND MESSAGE") == 0);
    assert(strcmp(chooser.count, "2/2") == 0);
    std::vector<std::pair<std::string, std::string> > known;
    known.push_back(std::make_pair(first, "1/2"));
    known.push_back(std::make_pair(second, "2/2"));
    std::vector<std::string> seen = restarts(known);
    assert(seen.size() == 1 && seen[0] == second);
    size_t change = 1;
    while (!frames[change].afterClear) change++;
    assert(ms(frames[change].tUs) < 300 + 800);
    assert(positionOf(frames.back(), second, "2/2", 14 * 6 - 1 + CHOOSER_GAP_COLS) >= 0);
}

static void gateSequenceAgainstTheVersionOneAppEndsOnTheNewMessage() {
    gateSequence(FakeApp::V1_APP);
}

static void gateSequenceAgainstTheVersionTwoAppEndsOnTheNewMessage() {
    gateSequence(FakeApp::V2_APP);
    // the late report in the '~' window was a failed read, not "nothing changed": '~' was sent again
    assert(app.requests[2].byte == '~');
    assert(app.requests[3].byte == '?');
}

static void staleFullMessageAcceptedLateIsCorrectedByTheNextReport() {
    // The sign shows A. Its first poll is lost. The recovery '~' is answered with A in full, but that
    // answer is held back. B is selected; the next '~' brings B. Then the held A arrives, inside a
    // later poll's window and ahead of that poll's own answer: the sign takes it and is back on A
    // while the app is on B. The next report must put that right.
    std::string a = "MESSAGE A IS LONG", b = "MESSAGE B IS LONG TOO";
    freshApp();
    app.dropped.insert(1);
    app.late[2] = Late{2, 1000};
    app.at(500, chooserJson(b, "2/2"), true); // between the second request (400 ms) and the third (600)
    app.at(4000, BYE, false);
    assert(runLoop(FakeApp::V2_APP, a, "1/2") == 'D');

    const char expect[] = {'?', '~', '~', '?', '?', '~', '?'};
    for (size_t i = 0; i < sizeof(expect); i++) assert(app.requests[i].byte == expect[i]);
    if (wantShow) {
        for (size_t i = 0; i < 8; i++) {
            printf("  stale-message sequence: %7.1f ms  sign asks %c  app writes %s\n", ms(app.requests[i].tUs),
                   app.requests[i].byte, app.replies[i].c_str());
        }
    }
    assert(ms(app.requests[1].tUs) < 500 && ms(app.requests[2].tUs) > 500);
    assert(app.replies[1] == chooserJson(a, "1/2")); // the answer that was held back
    assert(app.replies[2] == chooserJson(b, "2/2"));
    assert(app.replies[3] == report(chooserCrc(b.c_str(), "2/2"))); // arrived behind the stale A, unread
    assert(app.replies[4] == report(chooserCrc(b.c_str(), "2/2"))); // the report that caught it
    assert(ms(app.requests[5].tUs - app.requests[4].tUs) < 20);

    std::vector<std::pair<std::string, std::string> > known;
    known.push_back(std::make_pair(a, "1/2"));
    known.push_back(std::make_pair(b, "2/2"));
    std::vector<std::string> seen = restarts(known);
    assert(seen.size() == 3 && seen[0] == b && seen[1] == a && seen[2] == b); // new, stale, new again
    assert(strcmp(chooser.text, b.c_str()) == 0);
    // the stale picture lasted one poll period
    size_t stale = 1, fixed;
    while (!frames[stale].afterClear) stale++;
    stale++;
    while (!frames[stale].afterClear) stale++;
    fixed = stale + 1;
    while (!frames[fixed].afterClear) fixed++;
    assert(ms(frames[fixed].tUs - frames[stale].tUs) < 300);
}

static void lateReportInLoopsNormalReadIsSkippedAndTheMessageBehindItIsRead() {
    // Three failed reads send the sign back to loop(). Into loop()'s normal read comes, first, the
    // late answer to the poll (an object with no type), and behind it the real answer.
    std::string text = "PLUR IS NOT DEAD";
    freshApp();
    app.late[1] = Late{3, 5000};
    app.dropped.insert(2);
    app.dropped.insert(3);
    app.at(3000, BYE, false);
    assert(runLoop(FakeApp::V2_APP, text, "3/9") == 'D');

    const char expect[] = {'?', '~', '~', '~', '?'};
    for (size_t i = 0; i < sizeof(expect); i++) assert(app.requests[i].byte == expect[i]);
    assert(app.replies[0] == report(chooserCrc(text.c_str(), "3/9")));
    assert(app.replies[3] == chooserJson(text, "3/9"));
    // nothing was drawn for the untyped object and nothing else in between: the scroll went on
    // from where it was, one column after the other, without a clear
    const int cycle = 95 + CHOOSER_GAP_COLS;
    int expectPos = 0;
    for (size_t i = 0; i < frames.size(); i++) {
        assert(i == 0 || !frames[i].afterClear);
        assert(positionOf(frames[i], text, "3/9", cycle) == expectPos);
        expectPos = (expectPos + 1) % cycle;
    }
}

static void untypedObjectIsNeverAMessageForLoop() {
    // loop() must not take any of these for a message, draw for them, or look inside them
    const char* const notMessages[] = {"{}", "{\"c\":123}", "{\"str\":\"X\",\"flashy\":\"1/1\"}", "{\"type\":5}",
                                       "{\"type\":null,\"str\":\"X\"}", "[]", "7", "null"};
    for (size_t i = 0; i < sizeof(notMessages) / sizeof(notMessages[0]); i++) {
        freshApp();
        arrive(FakeApp::V2_APP, "HELLO", "1/1");
        DeserializationError err = deserializeJson(json, notMessages[i]);
        assert(err == DeserializationError::Ok);
        assert(chooserDispatch() == 0);
        assert(frames.empty());
        assert(app.requests.empty());
        assert(!chooser.onScreen);
    }
    // a message with a type but nothing else is shown by loop() (which reads a missing str as "")
    freshApp();
    arrive(FakeApp::V2_APP, "HELLO", "1/1");
    deserializeJson(json, "{\"type\":\"S\"}");
    assert(chooserDispatch() == 'S');
    // a chooser message with no str and no flashy is drawn as an empty chooser, not dereferenced
    freshApp();
    app.at(500, BYE, false);
    arrive(FakeApp::V2_APP, "HELLO", "1/1");
    app.message = "{\"type\":\"H\"}";
    deserializeJson(json, "{\"type\":\"H\"}");
    assert(chooserDispatch() == 'D');
    assert(chooser.text[0] == '\0' && chooser.count[0] == '\0');
    assert(!frames.empty());
}

static void normalReadSkipsUntypedObjectsOnlySoFar() {
    // the message behind two untyped objects is read in the same window
    freshApp();
    arrive(FakeApp::V2_APP, "HELLO", "1/1");
    startSerial();
    sendOnWire(std::string("{\"c\":1}{}") + BYE, nowUs + 2000);
    assert(chooserReadMessage() == DeserializationError::Ok);
    stopSerial();
    assert(strcmp(json["type"], "D") == 0);
    // nothing but untyped objects: the read ends (it does not spin for ever), still without a message
    freshApp();
    arrive(FakeApp::V2_APP, "HELLO", "1/1");
    startSerial();
    sendOnWire(repeat("{}", 200), nowUs + 2000);
    uint64_t began = nowUs;
    assert(chooserReadMessage() == DeserializationError::Ok);
    stopSerial();
    assert(ms(nowUs - began) < 100);
    assert(chooserDispatch() == 0);
    // nothing at all: the usual "no answer"
    freshApp();
    arrive(FakeApp::V2_APP, "HELLO", "1/1");
    startSerial();
    assert(chooserReadMessage() == DeserializationError::EmptyInput);
    stopSerial();
}

static void versionOneEmptyObjectIsAFailedRead() {
    // A version 1 app says {} for "unchanged since what I wrote". That is no longer believed: the
    // sign asks for the message itself every time.
    freshApp();
    app.at(2000, chooserJson("NEXT ONE", "2/9"), true);
    app.at(3000, BYE, false);
    assert(runChooser(FakeApp::V1_APP, "HELLO", "3/9")); // true: it never gave up
    for (size_t i = 0; i < 8; i += 2) {
        assert(app.requests[i].byte == '?' && app.replies[i] == "{}");
        assert(app.requests[i + 1].byte == '~' && app.replies[i + 1] == chooserJson("HELLO", "3/9"));
    }
    assert(strcmp(chooser.text, "NEXT ONE") == 0);
    assert(positionOf(frames.back(), "NEXT ONE", "2/9", 47 + CHOOSER_GAP_COLS) >= 0);
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
    RUN(escapedTextHasTheChecksumOfItsDecodedBytes);
    RUN(appSlowerThanThePollTimeoutKeepsItsPlaceInTheScroll);
    RUN(sameMessageAfterAnotherDisplayStartsFresh);
    RUN(bytesOutsideTheFontAreDrawnAsSpaces);
    RUN(failedRequestsOfAnEarlierVisitAreForgotten);
    RUN(gateSequenceAgainstTheVersionOneAppEndsOnTheNewMessage);
    RUN(gateSequenceAgainstTheVersionTwoAppEndsOnTheNewMessage);
    RUN(staleFullMessageAcceptedLateIsCorrectedByTheNextReport);
    RUN(lateReportInLoopsNormalReadIsSkippedAndTheMessageBehindItIsRead);
    RUN(untypedObjectIsNeverAMessageForLoop);
    RUN(normalReadSkipsUntypedObjectsOnlySoFar);
    RUN(versionOneEmptyObjectIsAFailedRead);
    printf("chooser_display_test: all %d tests passed\n", tests);
    return 0;
}
