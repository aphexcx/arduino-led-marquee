// Host test for ChooserDisplay.h: the real chooser display code (showAsChooser and everything under
// it) running against a fake sign and a fake app with a virtual clock. No Arduino, no framework.
//
//   clang++ -std=c++11 -Wall -Wextra -o /tmp/chooser_display_test test/chooser_display_test.cpp \
//       && /tmp/chooser_display_test
//
//   --show        also print a few frames as ASCII art (what panel 1 would look like)
//   --list        print the names of the tests
//   --only NAME   run one test
//
// Add -DCHOOSER_DEBUG_PANEL_B=1 to run the same tests against the build that shows its measurements
// on panel 2 (test/run.sh does both).
//
// What is real: ChooserDisplay.h, ChooserScroll.h, ArduinoJson, the font and the gamma table.
// What is fake: the LED strip (it records every column and frame), time (a microsecond counter that
// the fakes advance: about 31 us per LED column, 694 us per serial byte at 14400 baud), the serial
// line (every byte and every break the app sends is on it bit by bit, so the level of the receive
// pin can be asked at any moment) and the app (contract version 3: it pushes; or an older one).
// Not covered here: AVR integer widths (int is 32 bits on the host), real pixel timing, SoftwareSerial.

#undef NDEBUG

#include <algorithm>
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
#define PSTR(text) (text)
#define memcpy_P memcpy
#define gamma ledGammaTable // math.h already has a gamma()

#define COLUMNS_PER_PANEL 60
#define INTERCHAR_SPACE 1

#include "../Gamma.h"
#include "../Fonts.h"

// The sketch's settings (keep in step with the top of LED_Marquee.ino)
#define CHOOSER_HOLD_MS 600
#define CHOOSER_SCROLL_DELAY_MS 35
#define CHOOSER_GAP_COLS 18
#define CHOOSER_TEXT_BRIGHTNESS 145
#define CHOOSER_BLINK_PAUSE_MS 200
#define CHOOSER_REFRESH_MS 8000
#define CHOOSER_ATTN_HIGH_WAIT_MS 100
#define CHOOSER_ATTN_FIRST_BYTE_MS 150
#define CHOOSER_ATTN_BYTE_MS 100
#define CHOOSER_LINE_QUIET_MS 3
#define CHOOSER_BREAK_MIN_US 590UL
#define CHOOSER_TRAILER_WAIT_MS 100
#define REQUEST_QUIET_MS 10
#define REQUEST_GAP_MS 1
#define REQUEST_QUIET_WAIT_MS 500
#define REQUEST_BUSY_WAIT_MS 4000
#define ANSWER_FIRST_BYTE_MS 1000
#define ANSWER_AFTER_BREAK_MS (CHOOSER_ATTN_HIGH_WAIT_MS + CHOOSER_ATTN_FIRST_BYTE_MS)
#define CHOOSER_MAX_FAILED_READS 3
#ifndef CHOOSER_DEBUG_PANEL_B
#define CHOOSER_DEBUG_PANEL_B 0
#endif

const char MSGTYPE_CHOOSER = 'H';
const char MSGTYPE_DEFAULT = 'D';
const char HEART = '\x7F';

const int json_capacity = 601 + JSON_OBJECT_SIZE(8);
StaticJsonDocument<json_capacity> json;

// ---------------------------------------------------------------------------------------------
// Fake time. Every fake that lets time pass also lets the app act (appAdvance): the app has its
// own clock and does not wait to be asked.
// ---------------------------------------------------------------------------------------------
static uint64_t nowUs = 0;

static void appAdvance(uint64_t toUs);

static void tick(uint64_t us) {
    nowUs += us;
    appAdvance(nowUs);
}

static unsigned long millis() {
    tick(4); // a busy wait on the clock makes time pass
    return (unsigned long) (nowUs / 1000);
}

static unsigned long micros() {
    tick(4);
    return (unsigned long) nowUs;
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
static int clears = 0;

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
    clears++;
}

// ---------------------------------------------------------------------------------------------
// The serial line from the app to the sign
//
// Everything the app sends is on the line at its own time, whether or not the sign listens:
//  - bytes, 8N1 at 14400 baud: the line is LOW for the start bit and for every 0 bit
//  - breaks: the line is LOW for as long as the app holds it
// The sign can do two things with the line. It can ask for its level at any moment
// (chooserLineLow, the attention check). And it can listen (startSerial .. stopSerial): from then
// on whole bytes arrive in its buffer, `wire`. What a listening SoftwareSerial would not have got
// is not in the buffer: a byte whose start bit fell before the sign began to listen is lost, and a
// break is received as one 0x00 byte, and only if the sign was listening when it began.
// ---------------------------------------------------------------------------------------------
static const uint64_t BYTE_US = 694; // one byte at 14400 baud

struct WireByte {
    uint64_t atUs; // when the byte has fully arrived (its start bit fell BYTE_US earlier)
    char c;
    bool held;     // not a byte but what a break is received as: the line is LOW all through it
    bool dip;      // not a byte but what a short LOW is received as (0xFF, if it is over in 1.5 bit times)
};

struct LineByte {
    uint64_t startUs;
    uint8_t c;
};

struct LowSpan {
    uint64_t fromUs, toUs;
};

static std::deque<WireByte> wire;        // bytes the sign has not read yet
static std::vector<LineByte> lineBytes;  // every byte ever put on the line, in time order
static std::vector<LowSpan> lineBreaks;  // every break, in time order
static uint64_t wireBusyUntilUs = 0;

// Puts bytes on the line, the first one arriving at startUs or right behind what is already on it.
// Returns when the last one has arrived.
static uint64_t sendOnWire(const std::string& bytes, uint64_t startUs) {
    uint64_t at = startUs > wireBusyUntilUs + BYTE_US ? startUs : wireBusyUntilUs + BYTE_US;
    for (size_t i = 0; i < bytes.size(); i++) {
        wire.push_back(WireByte{at, bytes[i], false, false});
        lineBytes.push_back(LineByte{at - BYTE_US, (uint8_t) bytes[i]});
        wireBusyUntilUs = at;
        at += BYTE_US;
    }
    return wireBusyUntilUs;
}

// Holds the line LOW from fromUs (or from when the line is free) for lengthUs. Returns when it ends.
static uint64_t breakOnWire(uint64_t fromUs, uint64_t lengthUs) {
    if (fromUs < wireBusyUntilUs) fromUs = wireBusyUntilUs;
    lineBreaks.push_back(LowSpan{fromUs, fromUs + lengthUs});
    if (lengthUs >= BYTE_US) {
        wire.push_back(WireByte{fromUs + BYTE_US, '\0', true, false}); // what a listener makes of it
    } else if (lengthUs >= 10) {
        // SoftwareSerial's receiver takes the falling edge for a start bit (if the line is still
        // LOW when its interrupt gets there, some microseconds later) and samples eight bits
        // from 1.5 bit times on: LOW while the dip lasts, HIGH after it.
        unsigned value = 0;
        for (unsigned bit = 0; bit < 8; bit++) {
            if ((bit * 2 + 3) * BYTE_US / 20 >= lengthUs) value |= 1u << bit;
        }
        wire.push_back(WireByte{fromUs + BYTE_US, (char) value, false, true});
    }
    wireBusyUntilUs = fromUs + lengthUs;
    return wireBusyUntilUs;
}

static bool lineLowAt(uint64_t t) {
    for (size_t i = lineBreaks.size(); i-- > 0;) {
        if (lineBreaks[i].fromUs <= t && t < lineBreaks[i].toUs) return true;
        if (lineBreaks[i].toUs <= t) break;
    }
    // the last byte that started at or before t
    size_t lo = 0, hi = lineBytes.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (lineBytes[mid].startUs <= t) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return false;
    const LineByte& b = lineBytes[lo - 1];
    uint64_t into = t - b.startUs;
    if (into >= BYTE_US) return false;      // between bytes: idle, HIGH
    unsigned bit = (unsigned) (into * 10 / BYTE_US);
    if (bit == 0) return true;              // start bit
    if (bit == 9) return false;             // stop bit
    return ((b.c >> (bit - 1)) & 1) == 0;   // data, least significant bit first
}

// ---------------------------------------------------------------------------------------------
// Fake app
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

struct PendingRequest {
    uint64_t dueUs; // when the app gets to see it
    int number;     // counting from 1
};

struct Push {
    uint64_t breakStartUs, breakEndUs, messageEndUs;
    std::string message; // what the app holds at that moment
    bool frame;          // the message was put on the line behind the break
};

struct FakeApp {
    // V3_APP: contract version 3.1. Whenever its chooser message changes it pushes: a break, a
    //         guard, the message, unasked. When it leaves chooser mode it gives the break and
    //         sends nothing. It answers '~' with the message it holds now, except that a request
    //         that was on its way while a frame was pushed is answered by that frame alone.
    // V2_APP: contract version 2.1 as far as a version 3 sign can tell: it never pushes and
    //         answers '~' with the message it holds now.
    // OLD_APP: the same on the wire; kept apart so a test can say which app it means.
    enum Kind { V3_APP, V2_APP, OLD_APP } kind;
    std::string message;        // the JSON it would send for '~' right now
    bool chooserMode;
    uint64_t latencyUs;         // from the sign's request byte to the app seeing it (the Pico in between
                                // hands bytes on late); the app then answers at once
    uint64_t silentFromUs;      // from this time on it does not answer or push at all
    uint64_t breakUs, guardUs;  // the push: how long the line is held LOW, and the wait before the message
    bool breakAsZeros;          // the push holds the line LOW with a run of 0x00 bytes instead of a break
    uint64_t zerosStallUs;      // ... and its sending thread is held up for this long after the fourth of them
    bool leaveWithFrame;        // contract 3.0: leaving chooser mode pushes the other message as a frame too
    bool oneFramePerRequest;    // contract 3.1 rule 3 (off: 3.0, a request overtaken by a push gets a second frame)
    std::string trailer;        // bytes it writes behind every message (a newline, say)
    uint64_t leftAtUs;          // when it left chooser mode by a push (UINT64_MAX: it has not)
    uint64_t guardUntilUs;      // nothing of the app's begins before this: its last break (or run of zeros) + the guard
    int pushBehindAnswer;       // straight behind its answer to this request (0: none) it pushes pushBehindMessage,
    uint64_t pushBehindGapUs;   // the break beginning this long after the answer's last stop bit
    std::string pushBehindMessage;
    // What happens to push number n (counting from 1):
    std::map<int, std::string> damagedPush; // the message arrives as these bytes instead ("" = cut in half)
    std::set<int> lostPush;                 // nothing of it reaches the line (no break, no message)
    std::set<int> breakOnly;                // the break arrives, the message does not
    // What happens to the answer to request number n (counting from 1):
    std::map<int, std::string> damaged;     // arrives as these bytes instead ("" = cut in half)
    std::set<int> dropped;                  // never arrives
    std::vector<ScriptEvent> script;
    std::deque<PendingRequest> pending;
    bool pushOwed;                          // the message changed while the line was busy: push when it is free
    std::vector<Request> requests;
    std::vector<std::string> replies;       // what the app wrote for each request ("" = nothing)
    std::vector<Push> pushes;
    std::string heard;                      // every byte the sign sent, requests and all

    // Forgets every setting of the test before.
    void reset() {
        kind = V3_APP;
        message.clear();
        chooserMode = true;
        latencyUs = 3000;
        silentFromUs = UINT64_MAX;
        breakUs = 50000;
        guardUs = 20000;
        breakAsZeros = false;
        zerosStallUs = 0;
        leaveWithFrame = false;
        oneFramePerRequest = true;
        trailer.clear();
        leftAtUs = UINT64_MAX;
        guardUntilUs = 0;
        pushBehindAnswer = 0;
        pushBehindGapUs = 0;
        pushBehindMessage.clear();
        damagedPush.clear();
        lostPush.clear();
        breakOnly.clear();
        damaged.clear();
        dropped.clear();
        script.clear();
        pending.clear();
        pushOwed = false;
        requests.clear();
        replies.clear();
        pushes.clear();
        heard.clear();
    }

    // The '~' answer that put the sign into the chooser display has just been written.
    void begin(Kind k, const std::string& first) {
        kind = k;
        message = first;
        chooserMode = true;
        leftAtUs = UINT64_MAX;
        guardUntilUs = 0;
        pending.clear();
        pushOwed = false;
        requests.clear();
        replies.clear();
        pushes.clear();
        heard.clear();
    }

    void at(double atMs, const std::string& newMessage, bool stillChooser) {
        script.push_back(ScriptEvent{(uint64_t) (atMs * 1000), newMessage, stillChooser});
    }

    // The sign has sent a request byte.
    void onRequest(char byte) {
        assert(byte == '~'); // VERSION 3: the sign sends no '?', anywhere
        heard += byte;
        requests.push_back(Request{nowUs, byte});
        replies.push_back("");
        pending.push_back(PendingRequest{nowUs + latencyUs, (int) requests.size()});
    }

    // When the app next does something by itself, UINT64_MAX if never.
    uint64_t nextEventUs() const {
        uint64_t next = UINT64_MAX;
        if (!script.empty()) next = std::min(next, script.front().atUs);
        if (!pending.empty()) next = std::min(next, pending.front().dueUs);
        if (pushOwed) next = std::min(next, wireBusyUntilUs);
        return next;
    }

    void push(uint64_t atUs) {
        pushOwed = false;
        int number = (int) pushes.size() + 1;
        Push p{atUs, atUs, atUs, message, false};
        if (!chooserMode && leftAtUs == UINT64_MAX) leftAtUs = atUs;
        if (atUs >= silentFromUs || lostPush.count(number)) {
            pushes.push_back(p);
            return;
        }
        if (breakAsZeros) {
            p.breakStartUs = (atUs > wireBusyUntilUs + BYTE_US ? atUs : wireBusyUntilUs + BYTE_US) - BYTE_US;
            size_t zeros = (size_t) (breakUs / BYTE_US);
            if (zerosStallUs && zeros > 4) {
                uint64_t fourth = sendOnWire(std::string(4, '\0'), atUs);
                p.breakEndUs = sendOnWire(std::string(zeros - 4, '\0'), fourth + zerosStallUs + BYTE_US);
            } else {
                p.breakEndUs = sendOnWire(std::string(zeros, '\0'), atUs);
            }
            // a listener gets these as 0x00 bytes like any others; they are already in `wire`
        } else {
            p.breakStartUs = atUs < wireBusyUntilUs ? wireBusyUntilUs : atUs;
            p.breakEndUs = breakOnWire(atUs, breakUs);
        }
        p.messageEndUs = p.breakEndUs;
        guardUntilUs = p.breakEndUs + guardUs;
        std::string bytes = message;
        if (damagedPush.count(number)) {
            bytes = damagedPush[number].empty() ? bytes.substr(0, bytes.size() / 2) : damagedPush[number];
        }
        // On leaving chooser mode the app raises its hand and says nothing: the message that
        // follows is taken from its list by the sign's own request, not thrown at a sign that
        // may not be listening.
        if (!breakOnly.count(number) && (chooserMode || leaveWithFrame)) {
            p.messageEndUs = sendOnWire(bytes + trailer, p.breakEndUs + guardUs + BYTE_US);
            p.frame = true;
        }
        pushes.push_back(p);
    }

    // Does the one thing that is due next.
    void fireNext() {
        uint64_t scriptUs = script.empty() ? UINT64_MAX : script.front().atUs;
        uint64_t pendingUs = pending.empty() ? UINT64_MAX : pending.front().dueUs;
        uint64_t owedUs = pushOwed ? wireBusyUntilUs : UINT64_MAX;
        if (scriptUs <= pendingUs && scriptUs <= owedUs) {
            // the message changes (a command came in over Bluetooth)
            message = script.front().message;
            chooserMode = script.front().chooserMode;
            script.erase(script.begin());
            if (kind == V3_APP) {
                // latest wins, never queue: while something is going out, only remember that a push is owed
                if (wireBusyUntilUs > scriptUs) pushOwed = true; else push(scriptUs);
            }
        } else if (owedUs <= pendingUs) {
            push(owedUs);
        } else {
            // a request from the sign reaches the app: it answers with what it holds now
            PendingRequest r = pending.front();
            pending.pop_front();
            if (r.dueUs >= silentFromUs || dropped.count(r.number)) return;
            if (kind == V3_APP && oneFramePerRequest) {
                // one request, at most one frame: a request that was on the wire before the last
                // byte of a pushed frame was written is answered by that frame
                uint64_t sentUs = requests[(size_t) r.number - 1].tUs;
                for (size_t i = 0; i < pushes.size(); i++) {
                    if (pushes[i].frame && r.dueUs >= pushes[i].breakStartUs && sentUs < pushes[i].messageEndUs) return;
                }
            }
            std::string reply = message;
            replies[(size_t) r.number - 1] = reply;
            if (damaged.count(r.number)) {
                reply = damaged[r.number].empty() ? reply.substr(0, reply.size() / 2) : damaged[r.number];
            }
            // (rule 4: nothing leaves behind a break before the guard, an answer neither: glued to
            // the break its first byte would have no start edge)
            sendOnWire(reply + trailer, std::max(r.dueUs, guardUntilUs) + BYTE_US);
            if (r.number == pushBehindAnswer) {
                message = pushBehindMessage;
                Push p{wireBusyUntilUs + pushBehindGapUs, 0, 0, message, true};
                p.breakEndUs = breakAsZeros ? sendOnWire(std::string((size_t) (breakUs / BYTE_US), '\0'),
                                                         p.breakStartUs + BYTE_US)
                                            : breakOnWire(p.breakStartUs, breakUs);
                p.messageEndUs = sendOnWire(message + trailer, p.breakEndUs + guardUs + BYTE_US);
                guardUntilUs = p.breakEndUs + guardUs;
                pushes.push_back(p);
            }
        }
    }
};

static FakeApp app;

// How many requests the sign sent while the app was in chooser mode (the one that follows the
// app's leaving is how the sign gets the next message, and is not counted)
static size_t requestsWhileChoosing() {
    size_t n = 0;
    for (size_t i = 0; i < app.requests.size(); i++) n += app.requests[i].tUs < app.leftAtUs;
    return n;
}

static void appAdvance(uint64_t toUs) {
    while (app.nextEventUs() <= toUs) app.fireNext();
}

// The level of the receive pin, as the sketch reads it between frames. Counts how long the sign
// ever went without looking while it was not listening either.
static uint64_t lastLineReadUs = 0;
static uint64_t longestLineBlindUs = 0;

static bool chooserLineLow() {
    tick(2);
    if (nowUs - lastLineReadUs > longestLineBlindUs) longestLineBlindUs = nowUs - lastLineReadUs;
    lastLineReadUs = nowUs;
    return lineLowAt(nowUs);
}

static int lostToTransmit = 0; // how often the line from the app changed while the sign was sending a byte

struct FakeSerial {
    bool listening = false;
    unsigned long timeoutMs = 1000;

    void setTimeout(unsigned long t) { timeoutMs = t; }

    void print(char byte) {
        assert(listening);
        tick(BYTE_US); // one byte at 14400 baud
        app.onRequest(byte);
    }

    // Text that is not a request (only the bench build sends any, in front of a request)
    void print(const char* text) {
        assert(listening);
        for (; *text; text++) {
            assert(*text != '~' && *text != '?' && *text >= 0x20 && *text < 0x7F);
            tick(BYTE_US);
            app.heard += *text;
        }
    }

    // The byte at the front of the receive buffer, -1 if it is empty. Does not wait.
    int peek() {
        assert(listening);
        assert(!interruptsOff);
        tick(2);
        if (wire.empty() || wire.front().atUs > nowUs) return -1;
        return (unsigned char) wire.front().c;
    }

    // What ArduinoJson needs from a reader: the next byte, waiting for it up to the timeout
    // (a Stream's timedRead)
    int read() {
        assert(listening);
        assert(!interruptsOff); // SoftwareSerial receives in an interrupt
        uint64_t giveUpAtUs = nowUs + (uint64_t) timeoutMs * 1000;
        for (;;) {
            uint64_t next = (!wire.empty() && wire.front().atUs <= giveUpAtUs) ? wire.front().atUs : giveUpAtUs;
            if (app.nextEventUs() <= next) {
                app.fireNext(); // the app does something before then: it may put bytes on the line
                continue;
            }
            if (next > nowUs) nowUs = next;
            break;
        }
        if (wire.empty() || wire.front().atUs > nowUs) return -1;
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
    appAdvance(nowUs);
    softSerial.listening = true;
    // begin() starts with an empty buffer, and a byte whose start bit we did not see is not received
    while (!wire.empty() && wire.front().atUs < nowUs + BYTE_US) wire.pop_front();
}

static void stopSerial() {
    softSerial.listening = false;
    timeoutLeftBehind = softSerial.timeoutMs;
    lastLineReadUs = nowUs; // while it listened, the line was watched by the receiver
}

// The sketch's chooserSendByte(): sends one byte and says whether the line from the app changed
// while it went out (the receive pin's change flag). It decides nothing and does not stop the
// listening; that is the code under test's business.
// What the sign's receiver makes of it, if it is left listening: a byte that began while the
// sign was sending is taken from its middle, and so is every byte that follows it back to back
// (the receiver finds no start bit until the line pauses). Those arrive as damaged bytes. A
// break is still LOW when the receiver gets to it and is the 0x00 it always is. A byte that
// begins when the sign's byte has gone out, or any time after, is good.
static const char DAMAGED = '\x15';

static bool chooserSendByte(char c) {
    assert(softSerial.listening);
    assert(c == '~' || (c != '?' && c >= 0x20 && c < 0x7F));
    uint64_t fromUs = nowUs;
    tick(BYTE_US);
    bool changed = false;
    uint64_t damagedUntilUs = 0; // the arrival time of the last byte known damaged
    for (size_t i = 0; i < wire.size(); i++) {
        uint64_t startUs = wire[i].atUs - BYTE_US;
        bool began = startUs >= fromUs && startUs < nowUs;
        changed = changed || began;
        if (wire[i].held) continue;
        if (wire[i].dip) {
            // (the line is HIGH again when the receiver's interrupt gets to run: nothing received)
            if (began) wire[i].atUs = 0;
            continue;
        }
        if (began || (damagedUntilUs && wire[i].atUs <= damagedUntilUs + BYTE_US + REQUEST_GAP_MS * 1000 / 2)) {
            wire[i].c = DAMAGED;
            damagedUntilUs = wire[i].atUs;
        }
    }
    for (size_t i = 0; i < lineBreaks.size(); i++) {
        // any LOW that began (a break, or one too short to be the app holding the line)
        changed = changed || (lineBreaks[i].fromUs >= fromUs && lineBreaks[i].fromUs < nowUs);
    }
    if (c == '~') {
        app.onRequest(c);
    } else {
        app.heard += c;
    }
    for (size_t i = wire.size(); i-- > 0;) {
        if (wire[i].dip && wire[i].atUs == 0) wire.erase(wire.begin() + (long) i);
    }
    if (changed) {
        lostToTransmit++;
    }
    return changed;
}

// ---------------------------------------------------------------------------------------------
// The code under test
// ---------------------------------------------------------------------------------------------
#include "../ChooserDisplay.h"

// One exchange as loop() and the chooser make it, and the listening stopped behind it
static DeserializationError exchange() {
    DeserializationError err = chooserExchange();
    stopSerial();
    return err;
}

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
    nowUs = 0;
    frames.clear();
    sending.clear();
    clears = 0;
    for (size_t i = 0; i < leds.size(); i++) leds[i] = Led{0x7E, 9, 9, 9}; // leftovers of another effect
    clearedSinceLastFrame = false;
    softSerial = FakeSerial();
    wire.clear();
    lineBytes.clear();
    lineBreaks.clear();
    wireBusyUntilUs = 0;
    lastLineReadUs = 0;
    longestLineBlindUs = 0;
    lostToTransmit = 0;
    lineWasLow = false; // as after a reset of the sign: no LOW has been seen yet
#if CHOOSER_DEBUG_PANEL_B
    chooserDebug = ChooserDebug(); // as after a reset of the sign
#endif
    std::string first = chooserJson(text, count);
    app.begin(kind, first);
    DeserializationError err = deserializeJson(json, first);
    assert(err == DeserializationError::Ok);
}

// Runs showAsChooser() once, as loop() calls it. True: `json` holds a message of another type.
// False: it gave up (nothing usable could be read).
static bool runChooser(FakeApp::Kind kind, const std::string& text, const std::string& count) {
    arrive(kind, text, count);
    bool result = showAsChooser(json["str"], json["flashy"]);
    assert(!softSerial.listening);
    assert(timeoutLeftBehind == 0 || timeoutLeftBehind == 1000); // the normal '~' read keeps its usual timeout
    return result;
}

// The whole of loop() as far as the chooser goes, with the sketch's own code for the two decisions
// (chooserReadMessage() inside readSerialData(), chooserDispatch() in loop()). Returns the type of
// the message loop() would show next, or 0 when a request got no answer (the starfield).
static char runLoop(FakeApp::Kind kind, const std::string& text, const std::string& count) {
    arrive(kind, text, count);
    for (;;) {
        char type = chooserDispatch();
        if (type == CHOOSER_DISPATCH_NO_ANSWER) return 0; // the stars, as after any unanswered request
        if (type != CHOOSER_DISPATCH_NOTHING) return type;
        // nothing to show: loop() ends and readSerialData() asks again
        DeserializationError err = exchange();
        if (err != DeserializationError::Ok) return 0;
    }
}

static void freshApp() {
    app.reset();
}

// The app raises its hand at fromUs, placed by the test itself: a break of 50 ms, or the run of
// 72 zero bytes it sends where it cannot hold the line (which is what the sign's own app does:
// on the sign the break was not seen once, the run of zeros every time). Whatever the app sends
// next, an answer too, keeps the guard. Returns when the attention ends.
static uint64_t raiseHand(uint64_t fromUs, bool zeros) {
    uint64_t end = zeros ? sendOnWire(std::string(72, '\0'), fromUs + BYTE_US) : breakOnWire(fromUs, 50000);
    app.guardUntilUs = end + app.guardUs;
    return end;
}

// How many requests the app answered with a message
static size_t answered() {
    size_t n = 0;
    for (size_t i = 0; i < app.replies.size(); i++) n += !app.replies[i].empty();
    return n;
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

// Panel 2 is never lit by the chooser. (The build with the measurements on panel 2 is the one
// exception; what it shows there has its own test.)
static bool panelTwoDark(const Frame& f) {
    if (CHOOSER_DEBUG_PANEL_B) return true;
    for (size_t x = 60; x < 120; x++) {
        if (!f.leds[x].dark()) return false;
    }
    return true;
}

// The frame sent exactly n columns: panel 1's and no more. (With the measurements on panel 2, the
// frame that brings them up to date goes on into panel 2.)
static bool sentColumns(const Frame& f, size_t n) {
    return f.columnsSent == n || (CHOOSER_DEBUG_PANEL_B && f.columnsSent == 119);
}

static int cycleOf(const std::string& text) {
    return (int) stripRows(text, CHOOSER_GAP_COLS).size();
}

// The scroll position a frame shows, or -1 when it shows none of them
static int positionOf(const Frame& f, const std::string& text, const std::string& count) {
    int cycle = cycleOf(text);
    for (int pos = 0; pos < cycle; pos++) {
        if (panelOneIs(f, expectedPanel(text, count, pos, GAMMA(CHOOSER_TEXT_BRIGHTNESS), true))) return pos;
    }
    return -1;
}

static uint8_t blinkLevel(unsigned int count) {
    return GAMMA((count * 256) / 100);
}

static void printFrame(const Frame& f, const char* title) {
    printf("  %s  (t = %.0f ms, %zu columns sent)\n", title, ms(f.tUs), f.columnsSent);
    for (int bit = 7; bit >= 1; bit--) {
        printf("    |");
        for (size_t x = 0; x < (CHOOSER_DEBUG_PANEL_B ? 120 : 60); x++) {
            const Led& l = f.leds[x];
            bool on = !l.dark() && (l.row & (1 << bit));
            putchar(on ? (l.g == 0 && l.b == 0 ? 'o' : '#') : ' ');
            if (x == 59 && CHOOSER_DEBUG_PANEL_B) putchar('|');
        }
        printf("|\n");
    }
}

static bool wantShow = false;

// The frame that was on the LEDs at time t (the last one latched at or before it)
static size_t frameAt(uint64_t tUs) {
    size_t f = 0;
    while (f + 1 < frames.size() && frames[f + 1].tUs <= tUs) f++;
    return f;
}

// The first frame at or after `from` that shows the start (position 0) of this scrolling message
static size_t firstStartOf(const std::string& text, const std::string& count, size_t from = 0) {
    for (size_t i = from; i < frames.size(); i++) {
        if (positionOf(frames[i], text, count) == 0) return i;
    }
    return frames.size();
}

static const std::string FIRST = "PLUR IS NOT DEAD";           // 16 characters = 95 columns, viewport 42
static const std::string SECOND = "SECOND MESSAGE, ALSO LONG"; // 25 characters = 149 columns
// When FIRST has come round: the stand-still, then one step for each of its other 112 positions
// (to which the few ms it takes to blank the strip and draw are added)
static const double FIRST_PASS_MS = CHOOSER_HOLD_MS + (95 + CHOOSER_GAP_COLS - 1) * CHOOSER_SCROLL_DELAY_MS; // 4520

// Every frame shows one of the known messages whole, at one of its scroll positions: never a
// mixture, never part of a text that was not read to its end.
static void assertOnlyWholePictures(const std::string& a, const std::string& countA, const std::string& b,
                                    const std::string& countB) {
    for (size_t i = 0; i < frames.size(); i++) {
        bool known = positionOf(frames[i], a, countA) >= 0 || positionOf(frames[i], b, countB) >= 0;
        if (!known) {
            printFrame(frames[i], "neither message");
            assert(false);
        }
        assert(panelTwoDark(frames[i]));
    }
}

// ---------------------------------------------------------------------------------------------
// A preview that fits: the blink
// ---------------------------------------------------------------------------------------------

static void previewThatFitsBlinksAndPausesInTheDark() {
    freshApp();
    app.at(1000, BYE, false);
    bool other = runChooser(FakeApp::V3_APP, "HELLO", "3/9");
    assert(other);
    assert(frames.size() >= 58 * 3);

    // panel 2 is blanked once, on entry; then 58 frames per blink of count + blank column + text
    // fading to black, one straight after the other, and a pause in the dark before the next blink
    assert(clears == 1);
    assert(frames[0].afterClear);
    for (size_t i = 0; i < frames.size(); i++) {
        unsigned int count = 57 - (i % 58);
        assert(frames[i].afterClear == (i == 0));
        assert(sentColumns(frames[i], 17 + 1 + 29)); // what the old sendString() calls sent
        assert(panelOneIs(frames[i], expectedPanel("HELLO", "3/9", 0, blinkLevel(count), false)));
        assert(panelTwoDark(frames[i]));
        if (i == 0) continue;
        double sinceLast = ms(frames[i].tUs - frames[i - 1].tUs);
        if (i % 58 == 0) {
            assert(frames[i - 1].leds[18].dark()); // the pause is spent in the dark (the count stays lit)
            assert(!frames[i - 1].leds[0].dark());
            assert(sinceLast >= CHOOSER_BLINK_PAUSE_MS && sinceLast < CHOOSER_BLINK_PAUSE_MS + 3);
        } else {
            assert(sinceLast < 5);
        }
    }
    assert(frames[0].leds[0].r == 0x80 && frames[0].leds[0].g == 0); // the count is dim red
    assert(frames[0].leds[18].r == GAMMA(145));                       // the text starts at today's level
    assert(frames[57].leds[18].dark());                               // ... and ends black
    assert(requestsWhileChoosing() == 0);                                     // and the app was not asked anything

    if (wantShow) printFrame(frames[0], "a preview that fits: \"HELLO\" beside \"3/9\" (o = dim red, # = white)");
}

static void previewThatFitsIsReadAgainEveryRefresh() {
    // An app that never pushes: the message itself is read again every CHOOSER_REFRESH_MS, in
    // the dark pause of a blink, which is no longer for it. That read is what ends this run.
    freshApp();
    app.at(20000, BYE, false);
    assert(runChooser(FakeApp::V2_APP, "HELLO", "3/9"));
    assert(app.requests.size() == 3);
    uint64_t last = 0;
    for (size_t i = 0; i < app.requests.size(); i++) {
        double gap = ms(app.requests[i].tUs - last);
        assert(gap >= CHOOSER_REFRESH_MS && gap < CHOOSER_REFRESH_MS + 400);
        last = app.requests[i].tUs;
        size_t standing = frameAt(app.requests[i].tUs);
        assert(panelOneIs(frames[standing], expectedPanel("HELLO", "3/9", 0, blinkLevel(0), false)));
        if (i + 1 < app.requests.size()) {
            double pause = ms(frames[standing + 1].tUs - frames[standing].tUs);
            assert(pause >= CHOOSER_BLINK_PAUSE_MS && pause < CHOOSER_BLINK_PAUSE_MS + 3);
        }
    }
    assert(app.replies[0] == chooserJson("HELLO", "3/9") && app.replies[2] == BYE);
    assert(ms(nowUs) < 3 * (CHOOSER_REFRESH_MS + 400));
    assert(clears == 1); // the same message again: nothing is redrawn from scratch
}

static void leavingChooserIsAttentionAndThenTheSignAsks() {
    // Contract 3.1: when the app leaves chooser mode it holds the line LOW and sends nothing.
    // The sign stops where it is, waits CHOOSER_ATTN_FIRST_BYTE_MS for a message that does not
    // come, and asks; that request takes the next message from the app's list.
    const char* const texts[] = {"HELLO", "PLUR IS NOT DEAD"};
    for (size_t k = 0; k < 2; k++) {
        for (double offset = 0; offset < 40; offset += 3.7) {
            freshApp();
            app.at(1500 + offset, BYE, false);
            bool other = runChooser(FakeApp::V3_APP, texts[k], "3/9");
            assert(other);
            assert(strcmp(json["type"], "D") == 0);
            assert(strcmp(json["str"], "bye") == 0);
            assert(app.pushes.size() == 1 && !app.pushes[0].frame);
            assert(requestsWhileChoosing() == 0);
            assert(app.requests.size() == 1 && app.replies[0] == BYE);
            double asked = ms(app.requests[0].tUs) - ms(app.pushes[0].breakEndUs);
            // (the bench build sends its line of numbers first: 27 characters, 19 ms)
            assert(asked >= CHOOSER_ATTN_FIRST_BYTE_MS && asked < CHOOSER_ATTN_FIRST_BYTE_MS + REQUEST_QUIET_MS + (CHOOSER_DEBUG_PANEL_B ? 35 : 15));
            // nothing was drawn after the line went LOW: the picture stood still until loop() took over
            assert(frames.back().tUs < app.pushes[0].breakStartUs + 3000);
            assert(ms(nowUs) - ms(app.requests[0].tUs) < 30);
        }
    }
}

static void leavingWhileTheReadAtTheHoldIsOnItsWay() {
    // The text has come round and the sign has sent its '~'. Before the app sees that request it
    // leaves chooser mode: the line goes LOW, nothing follows. Then the request arrives and is
    // answered with the next message from the app's list. The sign is still in that read: the
    // break does not end it, the message is taken, and no second request takes another one.
    for (double offset = 0; offset < 240; offset += 17) {
        freshApp();
        app.latencyUs = 250000;
        app.at(FIRST_PASS_MS + 5 + offset, BYE, false);
        assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
        assert(strcmp(json["type"], "D") == 0 && strcmp(json["str"], "bye") == 0);
        assert(app.pushes.size() == 1 && !app.pushes[0].frame);
        assert(app.requests.size() == 1 && app.replies[0] == BYE);
        assertOnlyWholePictures(FIRST, "3/9", FIRST, "3/9");
    }
}

static void appThatLeavesWithAFrameIsStillUnderstood() {
    // Contract 3.0: leaving chooser mode pushed the other message as a frame. A sign for 3.1
    // takes that too, without asking.
    freshApp();
    app.leaveWithFrame = true;
    app.at(1000, BYE, false);
    bool other = runChooser(FakeApp::V3_APP, "HELLO", "3/9");
    assert(other);
    assert(strcmp(json["type"], "D") == 0);
    assert(strcmp(json["str"], "bye") == 0);
    assert(app.pushes.size() == 1 && app.pushes[0].frame);
    assert(nowUs >= app.pushes[0].messageEndUs && nowUs < app.pushes[0].messageEndUs + 8000);
    assert(app.requests.empty());
}

// ---------------------------------------------------------------------------------------------
// A long text: hold, scroll, and nothing said to the app while it moves
// ---------------------------------------------------------------------------------------------

// The frames of one message scrolling undisturbed: every position in order, every step the same
// length, every stand-still CHOOSER_HOLD_MS. Returns how often the text came round.
static int assertSteadyScroll(const std::string& text, const std::string& count, double holdSlackMs = 5) {
    const int cycle = cycleOf(text);
    int expectPos = 0;
    int wraps = 0;
    for (size_t i = 0; i < frames.size(); i++) {
        const Frame& f = frames[i];
        assert(sentColumns(f, 60)); // the count, the blank column, the whole viewport: never more
        assert(panelTwoDark(f));
        assert(f.afterClear == (i == 0));
        // (compared with the one picture that is due: a text that repeats shows the same
        // viewport at more than one position)
        assert(panelOneIs(f, expectedPanel(text, count, expectPos, GAMMA(CHOOSER_TEXT_BRIGHTNESS), true)));
        if (i + 1 == frames.size()) break;

        double untilNext = ms(frames[i + 1].tUs - f.tUs);
        if (expectPos == 0) {
            assert(untilNext >= CHOOSER_HOLD_MS && untilNext < CHOOSER_HOLD_MS + holdSlackMs);
        } else {
            // one step, to within a fraction of a millisecond: no poll, no read, nothing in between
            assert(untilNext >= CHOOSER_SCROLL_DELAY_MS - 0.5 && untilNext < CHOOSER_SCROLL_DELAY_MS + 0.5);
            if (expectPos == cycle - 1) wraps++;
        }
        expectPos = (expectPos + 1) % cycle;
    }
    return wraps;
}

static void longTextHoldsThenScrollsOneColumnPerDelay() {
    freshApp();
    app.at(12000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
    assert(assertSteadyScroll(FIRST, "3/9") >= 2); // 12 s is more than two times round
    assert(clears == 1);                           // panel 2 was blanked once, on entry

    if (wantShow) {
        printFrame(frames[0], "a long text, held at its start: \"PLUR IS NOT DEAD\" beside \"3/9\"");
        printFrame(frames[20], "20 columns later");
        printFrame(frames[80], "80 columns later: the end, the gap, and the text coming round again");
    }
}

static void noRequestByteWhileATextIsMoving() {
    freshApp();
    app.at(12000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
    const int cycle = cycleOf(FIRST);

    // The request log: one '~' each time the text has come round, sent with the first frame of
    // that stand-still on the LEDs, and nothing else. Not in the first stand-still, which began
    // with the read that brought the text.
    assert(requestsWhileChoosing() == 2);
    for (size_t r = 0; r < requestsWhileChoosing(); r++) {
        assert(app.requests[r].byte == '~');
        size_t f = frameAt(app.requests[r].tUs);
        assert(f > 0 && positionOf(frames[f], FIRST, "3/9") == 0);
        assert(positionOf(frames[f - 1], FIRST, "3/9") == cycle - 1);
        assert(ms(app.requests[r].tUs - frames[f].tUs) < REQUEST_QUIET_MS + 3); // at the first frame of the hold, once the line has been quiet
        assert(app.replies[r] == chooserJson(FIRST, "3/9"));
    }
    // Said the other way round: between the first scrolled frame of a pass and its last, zero bytes
    for (size_t i = 0; i < frames.size(); i++) {
        if (positionOf(frames[i], FIRST, "3/9") != 1) continue;
        size_t last = i;
        while (last + 1 < frames.size() && positionOf(frames[last + 1], FIRST, "3/9") != 0) last++;
        for (size_t r = 0; r < app.requests.size(); r++) {
            assert(app.requests[r].tUs < frames[i].tUs || app.requests[r].tUs > frames[last].tUs);
        }
    }
    // and all that time the receive line was looked at: never left alone for longer than it takes
    // to blank the strip and draw a frame
    assert(ms(longestLineBlindUs) < 10);
}

static void slowAnswerAtTheHoldCostsNothing() {
    // The Pico hands the sign's request to the app a quarter of a second late. The answer still
    // comes inside the stand-still, which stays as long as it was; the scroll is not touched.
    freshApp();
    app.latencyUs = 250000;
    app.at(12000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
    assert(assertSteadyScroll(FIRST, "3/9") >= 2);
    assert(requestsWhileChoosing() == 2);

    // Slower than the stand-still itself: that one stand-still lasts until the answer is in, and
    // the scroll is still not touched.
    freshApp();
    app.latencyUs = 700000;
    app.at(12000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
    assert(assertSteadyScroll(FIRST, "3/9", 200) >= 2);
}

static void sixtyFourCharacterTextBesideAFiveCharacterCount() {
    std::string text = repeat("THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG. ", 64);
    freshApp();
    app.at(16000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, text, "12/34"));
    assert(cycleOf(text) == 64 * 6 - 1 + CHOOSER_GAP_COLS); // 401
    assert(assertSteadyScroll(text, "12/34") == 1);         // 401 columns x 35 ms = 14 s: once round
    assert(requestsWhileChoosing() == 1);                       // the one read, at the stand-still
}

static void messageTheAppNeverPushedIsPickedUpAtTheNextHold() {
    // A missed push, or an app that does not push at all (contract 2.1): the sign shows the old
    // message until its text has come round, reads the message at that stand-still, and is right.
    for (int variant = 0; variant < 2; variant++) {
        freshApp();
        if (variant == 1) app.lostPush.insert(1);
        app.at(2000, chooserJson(SECOND, "4/9"), true);
        app.at(9000, BYE, false);
        assert(runChooser(variant == 0 ? FakeApp::V2_APP : FakeApp::V3_APP, FIRST, "3/9"));

        size_t change = firstStartOf(SECOND, "4/9");
        assert(change < frames.size());
        assert(frames[change].afterClear);
        // the first message went all the way round, undisturbed, although the app had moved on
        const int cycle = cycleOf(FIRST);
        for (size_t i = 0; i < change; i++) assert(positionOf(frames[i], FIRST, "3/9") == (int) (i % cycle));
        assert(change == (size_t) cycle + 1);
        // ... and the new one is up within one read of that stand-still's first frame
        assert(ms(frames[change].tUs - frames[change - 1].tUs) < 150);
        assert(ms(frames[change].tUs) > FIRST_PASS_MS && ms(frames[change].tUs) < FIRST_PASS_MS + 150);
        assert(app.requests.size() >= 1 && ms(app.requests[0].tUs) > FIRST_PASS_MS - 10);
        assertOnlyWholePictures(FIRST, "3/9", SECOND, "4/9");
    }
}

// ---------------------------------------------------------------------------------------------
// The push: attention on the receive line, then the message, unasked
// ---------------------------------------------------------------------------------------------

static void pushedMessageMidScrollRestartsAtTheHold() {
    freshApp();
    app.at(2000, chooserJson(SECOND, "4/9"), true); // "!next" at t = 2 s, in the middle of the scroll
    app.at(5000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
    assert(app.pushes.size() == 2);

    size_t change = firstStartOf(SECOND, "4/9");
    assert(change < frames.size());
    const Frame& restart = frames[change];
    assert(restart.afterClear);
    assert(panelTwoDark(restart));
    // on the display within 150 ms of the end of the break (in fact as soon as the message is in)
    assert(restart.tUs > app.pushes[0].messageEndUs);
    assert(ms(restart.tUs - app.pushes[0].breakEndUs) < 150);
    assert(ms(restart.tUs - app.pushes[0].messageEndUs) < 15);
    // it was mid-scroll before, not at the start, and stopped where it was when the line went LOW
    int before = positionOf(frames[change - 1], FIRST, "3/9");
    assert(before > 10);
    assert(frames[change - 1].tUs < app.pushes[0].breakStartUs + 3000);
    // the new message holds before it moves
    double hold = ms(frames[change + 1].tUs - restart.tUs);
    assert(hold >= CHOOSER_HOLD_MS && hold < CHOOSER_HOLD_MS + 5);
    assert(positionOf(frames[change + 1], SECOND, "4/9") == 1);
    // and the sign did not send a single byte for any of it
    assert(requestsWhileChoosing() == 0);
    assertOnlyWholePictures(FIRST, "3/9", SECOND, "4/9");
}

static void changingOnlyTheCountRestartsToo() {
    freshApp();
    app.at(2000, chooserJson(FIRST, "3/8"), true); // another message was deleted: same text, new count
    app.at(4000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
    size_t change = firstStartOf(FIRST, "3/8");
    assert(change < frames.size());
    assert(frames[change].afterClear);
}

static void pushOfTheSameMessageStartsItAgain() {
    // "A chooser message: show it from its start, whether or not the text changed."
    freshApp();
    app.at(2000, chooserJson(FIRST, "3/9"), true);
    app.at(4000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
    size_t again = firstStartOf(FIRST, "3/9", 1);
    assert(again < frames.size());
    assert(positionOf(frames[again - 1], FIRST, "3/9") > 10);
    assert(ms(frames[again].tUs - app.pushes[0].breakEndUs) < 150);
    double hold = ms(frames[again + 1].tUs - frames[again].tUs);
    assert(hold >= CHOOSER_HOLD_MS && hold < CHOOSER_HOLD_MS + 5);
    assert(clears == 1); // nothing went dark for it: the picture is drawn over itself
    assert(requestsWhileChoosing() == 0);
}

static void scrollingToShortMessageGoesBackToTheBlink() {
    freshApp();
    app.at(2000, chooserJson("HI", "4/9"), true);
    app.at(3000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
    const Frame& last = frames.back();
    assert(sentColumns(last, 17 + 1 + 11));
    // nothing of the long text is left on the panel beside the short one
    bool matched = false;
    for (unsigned int count = 0; count < 58; count++) {
        matched = matched || panelOneIs(last, expectedPanel("HI", "4/9", 0, blinkLevel(count), false));
    }
    assert(matched);
    assert(panelTwoDark(last));
}

// One run with a single push at pushMs. Returns how long after the end of the break the pushed
// message was on the LEDs, in ms; -1 if it never was.
static double pushLatencyMs(const std::string& text, double pushMs, const std::string& pushedText,
                            bool mayCollideWithARead = false) {
    freshApp();
    app.at(pushMs, chooserJson(pushedText, "4/9"), true);
    app.at(pushMs + 900, BYE, false);
    assert(runChooser(FakeApp::V3_APP, text, "3/9"));
    // the push was read as it came: nothing had to be asked (unless it ran into the read at the
    // stand-still, whose own answer then comes in behind it and has to be cleared up)
    if (!mayCollideWithARead) assert(requestsWhileChoosing() == 0);
    for (size_t i = 0; i < frames.size(); i++) {
        if (frames[i].tUs < app.pushes[0].breakStartUs) continue;
        bool shown = cycleOf(pushedText) - CHOOSER_GAP_COLS > 42
                         ? positionOf(frames[i], pushedText, "4/9") == 0
                         : panelOneIs(frames[i], expectedPanel(pushedText, "4/9", 0, blinkLevel(57), false));
        if (shown) return ms(frames[i].tUs) - ms(app.pushes[0].breakEndUs);
    }
    return -1;
}

static void attentionIsSeenInEveryPhaseWhereverTheBreakFalls() {
    // A 50 ms break is never missed: in the stand-still, in the scroll (at every offset inside a
    // step, the moment a frame is being sent included) and in the blink.
    double worst = 0;
    for (double offset = 0; offset < 2 * CHOOSER_SCROLL_DELAY_MS; offset += 0.45) {
        double inHold = pushLatencyMs(FIRST, 100 + offset, SECOND);
        double inScroll = pushLatencyMs(FIRST, 2000 + offset, SECOND);
        double inBlink = pushLatencyMs("HELLO", 500 + offset, SECOND);
        double blinkToBlink = pushLatencyMs("HELLO", 500 + offset, "HI");
        double atTheWrap = pushLatencyMs(FIRST, FIRST_PASS_MS - 30 + offset, SECOND, true); // around the '~' of the hold
        const double all[] = {inHold, inScroll, inBlink, blinkToBlink, atTheWrap};
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
            assert(all[i] >= 0 && all[i] < 150);
            if (all[i] > worst) worst = all[i];
        }
    }
    // the blink once more, through all of one fade and its dark pause
    for (double offset = 0; offset < 110 + CHOOSER_BLINK_PAUSE_MS + 20; offset += 1.3) {
        double toScroll = pushLatencyMs("HELLO", 500 + offset, SECOND);
        double toBlink = pushLatencyMs("HELLO", 500 + offset, "HI");
        assert(toScroll >= 0 && toScroll < 150 && toBlink >= 0 && toBlink < 150);
        if (toScroll > worst) worst = toScroll;
        if (toBlink > worst) worst = toBlink;
    }
    if (wantShow) printf("  a push is on the LEDs at most %.1f ms after the end of its break\n", worst);
}

static void shortBreakMayBeMissedButTheSignEndsUpRight() {
    // A break shorter than one step may go unseen. Then the message's own bytes pull the line
    // LOW; nothing of it can be trusted, so the sign asks. Whatever the length, the sign never
    // shows anything but a whole message and ends on the new one.
    const double lengthsMs[] = {0, 0.2, 1, 3, 10, 20, 34};
    for (size_t l = 0; l < sizeof(lengthsMs) / sizeof(lengthsMs[0]); l++) {
        for (double offset = 0; offset < CHOOSER_SCROLL_DELAY_MS; offset += 2.3) {
            freshApp();
            app.breakUs = (uint64_t) (lengthsMs[l] * 1000);
            app.at(2000 + offset, chooserJson(SECOND, "4/9"), true);
            app.at(5000, BYE, false); // (a short break on leaving may be missed too: found when SECOND has come round)
            assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
            assertOnlyWholePictures(FIRST, "3/9", SECOND, "4/9");
            size_t change = firstStartOf(SECOND, "4/9");
            assert(change < frames.size());
            // at the latest by the next stand-still of the old message; in fact within a second
            assert(ms(frames[change].tUs) < 2000 + offset + 1000);
            for (size_t r = 0; r < app.requests.size(); r++) assert(app.requests[r].byte == '~');
        }
    }
}

static void garbageAfterAttentionFallsBackToTilde() {
    // The break is seen, and then: half a message, bytes that are no JSON, an object that is not a
    // message, a message that stops in the middle of its text, or nothing at all. None of it is
    // shown. The sign asks the normal way and shows what that brings.
    const char* const damage[] = {"", "}\x01\x7Fzz{\"t", "{\"c\":12345}", "{\"type\":\"H\",\"str\":\"SECOND MESS",
                                  "{\"type\":\"H\",\"str\":\"HALF\",\"flashy\":\"4", "[1,2,3]", "{}",
                                  // a whole message, but not at the top: inside an array, behind other bytes
                                  "[{\"type\":\"D\",\"str\":\"bye\"}]", "x{\"type\":\"D\",\"str\":\"bye\"}",
                                  "\"a\":{\"type\":\"D\",\"str\":\"bye\"}}"};
    for (size_t d = 0; d <= sizeof(damage) / sizeof(damage[0]); d++) {
        freshApp();
        if (d < sizeof(damage) / sizeof(damage[0])) {
            app.damagedPush[1] = damage[d];
        } else {
            app.breakOnly.insert(1);
        }
        app.at(2000, chooserJson(SECOND, "4/9"), true);
        app.at(6000, BYE, false);
        assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));

        // never a half-read text, never the damaged bytes
        assertOnlyWholePictures(FIRST, "3/9", SECOND, "4/9");
        // one '~', after the push had gone by, answered with the message itself
        assert(requestsWhileChoosing() == 1 && app.requests[0].byte == '~');
        assert(app.requests[0].tUs > app.pushes[0].messageEndUs);
        assert(app.replies[0] == chooserJson(SECOND, "4/9"));
        size_t change = firstStartOf(SECOND, "4/9");
        assert(change < frames.size() && frames[change].afterClear);
        assert(frames[change].tUs > app.requests[0].tUs);
        assert(ms(frames[change].tUs - app.pushes[0].breakEndUs) < 600);
        // the old message stood still meanwhile: it did not scroll on, and it was not redrawn
        size_t seen = frameAt(app.pushes[0].breakStartUs + 3000);
        assert(change == seen + 1);
    }
}

static void secondChangeDuringAPushIsPushedNext() {
    // latest wins: the app finishes the push it is in, then pushes what it holds by then
    std::string third = "THE THIRD ONE IS LONG TOO";
    freshApp();
    app.at(2000, chooserJson(SECOND, "4/9"), true);
    app.at(2010, chooserJson("SKIPPED, NEVER SENT AT ALL", "5/9"), true);
    app.at(2020, chooserJson(third, "6/9"), true);
    app.at(5000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
    assert(app.pushes.size() == 3); // SECOND, then the newest, then BYE
    assert(app.pushes[1].message == chooserJson(third, "6/9"));
    size_t two = firstStartOf(SECOND, "4/9");
    size_t three = firstStartOf(third, "6/9");
    assert(two < three && three < frames.size());
    assert(ms(frames[two].tUs - app.pushes[0].breakEndUs) < 150);
    assert(ms(frames[three].tUs - app.pushes[1].breakEndUs) < 150);
    assert(positionOf(frames.back(), third, "6/9") > 10);
    assert(requestsWhileChoosing() == 0);
}

static void runOfZeroBytesWorksAsABreak() {
    // If the platform cannot send a break, the app sends 50 ms of 0x00 bytes: LOW nine tenths of
    // the time, with a stop bit in every byte.
    for (double offset = 0; offset < CHOOSER_SCROLL_DELAY_MS; offset += 1.7) {
        freshApp();
        app.breakAsZeros = true;
        app.at(2000 + offset, chooserJson(SECOND, "4/9"), true);
        app.at(4000, BYE, false);
        assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
        size_t change = firstStartOf(SECOND, "4/9");
        assert(change < frames.size());
        assert(ms(frames[change].tUs - app.pushes[0].breakEndUs) < 150);
        assert(requestsWhileChoosing() == 0);
        assertOnlyWholePictures(FIRST, "3/9", SECOND, "4/9");
    }
}

static void pushThatLandsInTheReadAtTheHoldIsStillTaken() {
    // The text has come round and the sign has just sent its '~'. The Pico is slow, so before the
    // app has even seen that request, it pushes. The break arrives where the answer was expected.
    // (twice: an app that answers the overtaken request with the pushed frame alone, as 3.1
    // says, and one that sends a second frame for it, which then arrives unasked)
    for (double offset = 0; offset < 480; offset += 13) {
        freshApp();
        app.latencyUs = 250000;
        app.oneFramePerRequest = offset < 240;
        if (offset >= 240) offset += 0.5;
        app.at(FIRST_PASS_MS + 5 + (offset >= 240 ? offset - 240 : offset), chooserJson(SECOND, "4/9"), true);
        app.at(FIRST_PASS_MS + 4000, BYE, false);
        assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
        assertOnlyWholePictures(FIRST, "3/9", SECOND, "4/9");
        size_t change = firstStartOf(SECOND, "4/9");
        assert(change < frames.size());
        assert(ms(frames[change].tUs - app.pushes[0].breakEndUs) < 150);
        // the new message is drawn from its start once, and then scrolls: the answers that come in
        // behind the push (to requests the sign has given up on) do not start it again
        for (size_t i = change + 1; i < frames.size(); i++) assert(!frames[i].afterClear);
        for (size_t i = change + 1; i < frames.size(); i++) {
            assert(positionOf(frames[i], SECOND, "4/9") == (int) (i - change));
        }
        double hold = ms(frames[change + 1].tUs - frames[change].tUs);
        assert(hold >= CHOOSER_HOLD_MS && hold < CHOOSER_HOLD_MS + 600);
        assert(strcmp(chooser.text, SECOND.c_str()) == 0);
    }
}

static void strayMessageWithoutABreakKeepsThePlaceInTheScroll() {
    // Bytes nobody asked for (an answer that comes long after the sign stopped waiting for it):
    // the line goes LOW, nothing readable follows, the sign asks, gets the message it is already
    // showing, and scrolls on from where it stopped.
    freshApp();
    app.at(4000, BYE, false);
    arrive(FakeApp::V3_APP, FIRST, "3/9");
    uint64_t strayEnd = sendOnWire(chooserJson(FIRST, "3/9"), 2000 * 1000);
    assert(showAsChooser(json["str"], json["flashy"]));

    const int cycle = cycleOf(FIRST);
    int hitches = 0;
    for (size_t i = 0; i < frames.size(); i++) {
        assert(positionOf(frames[i], FIRST, "3/9") == (int) (i % cycle)); // never back to the start
        assert(frames[i].afterClear == (i == 0));
        if (i > 0 && i + 1 < frames.size()) {
            double step = ms(frames[i + 1].tUs - frames[i].tUs);
            if (step > CHOOSER_SCROLL_DELAY_MS + 1) {
                hitches++;
                assert(step < 400);
                assert(frames[i].tUs < strayEnd && frames[i + 1].tUs > strayEnd);
            }
        }
    }
    assert(hitches == 1);
    assert(requestsWhileChoosing() == 1 && app.requests[0].tUs > strayEnd);
}

static void pushThenTheSyncBringsTheSameMessageAgain() {
    // The sign gets the same chooser message twice in a row: pushed, and then again from its own
    // read when that text has come round. The second time changes nothing: no restart, no blanking.
    freshApp();
    app.at(2000, chooserJson(SECOND, "4/9"), true);
    app.at(11000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
    size_t change = firstStartOf(SECOND, "4/9");
    assert(change < frames.size());
    const int cycle = cycleOf(SECOND);
    assert(frames.size() - change > (size_t) cycle + 20); // it came round and went on
    for (size_t i = change; i < frames.size(); i++) {
        int pos = (int) ((i - change) % (size_t) cycle);
        assert(positionOf(frames[i], SECOND, "4/9") == pos);
        assert(frames[i].afterClear == (i == change));
        if (i + 1 == frames.size()) break;
        double step = ms(frames[i + 1].tUs - frames[i].tUs);
        if (pos == 0) {
            assert(step >= CHOOSER_HOLD_MS && step < CHOOSER_HOLD_MS + 5);
        } else {
            assert(step >= CHOOSER_SCROLL_DELAY_MS - 0.5 && step < CHOOSER_SCROLL_DELAY_MS + 0.5);
        }
    }
    assert(requestsWhileChoosing() == 1 && app.replies[0] == chooserJson(SECOND, "4/9"));
}

static void bytesBehindAMessageDoNotStopTheScroll() {
    // An app that writes a newline behind every message. The byte behind the closing brace is
    // not the app raising its hand.
    freshApp();
    app.trailer = "\n";
    app.latencyUs = 200000;
    app.at(11000, BYE, false);
    assert(runChooser(FakeApp::V2_APP, FIRST, "3/9"));
    assert(assertSteadyScroll(FIRST, "3/9", 5) >= 2);
    assert(app.requests.size() == 3); // one read at each stand-still; the third brings the other message

    // a long one: forty spaces, or the whole answer written twice
    const std::string trailers[] = {std::string(40, ' '), chooserJson(FIRST, "3/9")};
    for (size_t k = 0; k < 2; k++) {
        freshApp();
        app.trailer = trailers[k];
        app.latencyUs = 9000;
        app.at(11000, BYE, false);
        assert(runChooser(FakeApp::V2_APP, FIRST, "3/9"));
        assert(assertSteadyScroll(FIRST, "3/9", 5) >= 2);
        assert(app.requests.size() == 3);
    }

    // the same behind a pushed message (and so in front of the next one)
    freshApp();
    app.trailer = "\r\n";
    app.at(2000, chooserJson(SECOND, "4/9"), true);
    app.at(6000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
    size_t change = firstStartOf(SECOND, "4/9");
    assert(change < frames.size());
    assert(ms(frames[change].tUs - app.pushes[0].breakEndUs) < 150);
    for (size_t i = change; i < frames.size(); i++) assert(positionOf(frames[i], SECOND, "4/9") == (int) (i - change));
    assert(requestsWhileChoosing() == 0);
}

static void breakRightBehindAnAnswerIsStillAttention() {
    // The answer to the read at the stand-still, and straight behind it (with no gap at all,
    // and with every gap up to a few ms) the app raises its hand for a new message. Letting a
    // newline behind an answer go by must not let this go by.
    for (double gapMs = 0; gapMs < 12; gapMs += 0.2) {
        freshApp();
        app.breakAsZeros = gapMs >= 6; // ... and the same with a run of 0x00 bytes for a break
        if (gapMs >= 6) gapMs += 0.05;
        app.pushBehindGapUs = (uint64_t) ((gapMs >= 6 ? gapMs - 6 : gapMs) * 1000);
        app.pushBehindAnswer = 1;
        app.pushBehindMessage = chooserJson(SECOND, "4/9");
        app.at(FIRST_PASS_MS + 3000, BYE, false);
        assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
        size_t change = firstStartOf(SECOND, "4/9");
        assert(change < frames.size());
        assert(ms(frames[change].tUs - app.pushes[0].breakEndUs) < 150);
        assert(requestsWhileChoosing() == 1); // the read at the stand-still, and nothing had to be asked after it
        assertOnlyWholePictures(FIRST, "3/9", SECOND, "4/9");
    }
}

static void glitchOnTheLineNeverMakesAColumnComeEarly() {
    // A LOW too short to be anything (2 us). Whether the sign happens to see it or not, no
    // column comes early and the text does not jump.
    int seen = 0;
    for (double offset = 0; offset < CHOOSER_SCROLL_DELAY_MS; offset += 0.37) {
        freshApp();
        app.at(4000, BYE, false);
        arrive(FakeApp::V3_APP, FIRST, "3/9");
        breakOnWire((uint64_t) ((2000 + offset) * 1000), 2);
        assert(showAsChooser(json["str"], json["flashy"]));
        for (size_t i = 0; i + 1 < frames.size(); i++) {
            assert(positionOf(frames[i], FIRST, "3/9") == (int) i);
            // (the bench build's frame with the numbers is 59 columns longer and latches 1.9 ms
            // later in its step, so the step after it measures that much less)
            if (i > 0) assert(ms(frames[i + 1].tUs - frames[i].tUs) >= CHOOSER_SCROLL_DELAY_MS - (CHOOSER_DEBUG_PANEL_B ? 2.5 : 0.5));
        }
        seen += requestsWhileChoosing() > 0;
    }
    assert(seen > 0); // (some of them were seen, or this test tested nothing)
}

// ---------------------------------------------------------------------------------------------
// When nothing usable comes
// ---------------------------------------------------------------------------------------------

static void silentAppIsNoticedOnlyAtTheHold() {
    freshApp();
    app.silentFromUs = 1000 * 1000; // the app dies after 1 s
    bool other = runChooser(FakeApp::V3_APP, FIRST, "3/9");
    assert(!other); // loop() goes back to its normal '~'

    // the text scrolled on to its end without a single hitch: nothing is asked while it moves
    const int cycle = cycleOf(FIRST);
    assert(frames.size() == (size_t) cycle + 1);
    for (size_t i = 1; i + 1 < frames.size(); i++) {
        double step = ms(frames[i + 1].tUs - frames[i].tUs);
        assert(step >= CHOOSER_SCROLL_DELAY_MS - 0.5 && step < CHOOSER_SCROLL_DELAY_MS + 0.5);
    }
    // at the stand-still: the read, then the normal way once more, then loop() takes over
    assert(requestsWhileChoosing() == 2);
    assert(app.requests[0].byte == '~' && app.requests[1].byte == '~');
    assert(ms(app.requests[0].tUs) > FIRST_PASS_MS - 10);
    assert(ms(nowUs) < FIRST_PASS_MS + 2600);
}

static void lineStuckLowIsGivenUpOn() {
    // a cable pulled out, an app that crashed in the middle of a break
    freshApp();
    arrive(FakeApp::V3_APP, FIRST, "3/9");
    app.silentFromUs = 0;
    breakOnWire(1000 * 1000, 3600ull * 1000 * 1000);
    assert(!showAsChooser(json["str"], json["flashy"]));
    assert(ms(nowUs) > 1000 && ms(nowUs) < 1000 + 1500);
    assert(!softSerial.listening);
}

static void oneReadThatFailsThenTheSameMessageKeepsThePicture() {
    // The read at the stand-still gets no answer; the sign asks once more and is given the
    // message it is showing. The picture is still ours: no blanking, no start from scratch.
    freshApp();
    app.dropped.insert(1);
    app.at(FIRST_PASS_MS + 6000, BYE, false);
    assert(runLoop(FakeApp::V3_APP, FIRST, "3/9") == 'D');
    assert(clears == 1);
    const int cycle = cycleOf(FIRST);
    for (size_t i = 0; i < frames.size(); i++) {
        assert(positionOf(frames[i], FIRST, "3/9") == (int) (i % cycle));
        assert(frames[i].afterClear == (i == 0));
    }
    assert(app.replies[1] == chooserJson(FIRST, "3/9"));
}

static void twoUnansweredRequestsEndInTheStarsLikeAnyOther() {
    // The read at the stand-still and the one after it get no answer. That is an app that does
    // not answer: loop() shows its stars, as after any unanswered request, and does not wait a
    // third second with the old text standing there.
    freshApp();
    app.dropped.insert(1);
    app.dropped.insert(2);
    assert(runLoop(FakeApp::V3_APP, FIRST, "3/9") == 0);
    assert(app.requests.size() == 2);

    // The same when the app leaves chooser mode and has nothing to send: one request, one wait
    freshApp();
    app.at(2000, "", false);
    app.silentFromUs = 2060 * 1000; // (after its break)
    assert(runLoop(FakeApp::V3_APP, FIRST, "3/9") == 0);
    assert(app.requests.size() == 1);
    assert(ms(nowUs) < 2000 + 50 + CHOOSER_ATTN_FIRST_BYTE_MS + REQUEST_QUIET_MS + ANSWER_FIRST_BYTE_MS + (CHOOSER_DEBUG_PANEL_B ? 50 : 30)); // (the bench build sends its line of numbers first)
}

static void failedReadsOfAnEarlierVisitAreForgotten() {
    freshApp();
    app.silentFromUs = 0;
    assert(!runChooser(FakeApp::V3_APP, FIRST, "3/9")); // gave up
    freshApp();
    app.at(FIRST_PASS_MS + 1000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
    assert(assertSteadyScroll(FIRST, "3/9") == 1);
}

// ---------------------------------------------------------------------------------------------
// What is drawn, whatever the message
// ---------------------------------------------------------------------------------------------

static void oldAppWithSevenCharacterPreviewBlinks() {
    freshApp();
    app.at(1000, BYE, false);
    assert(runChooser(FakeApp::OLD_APP, "PLUR IS", "3/9")); // 7 characters = 41 columns in a 42 column viewport
    for (size_t i = 0; i < frames.size(); i++) {
        assert(sentColumns(frames[i], 17 + 1 + 41));
        assert(frames[i].afterClear == (i == 0));
    }
    assert(ms(nowUs) > CHOOSER_REFRESH_MS && ms(nowUs) < CHOOSER_REFRESH_MS + 400); // it left at the first read
}

static void neverMoreThanSixtyColumnsWhateverTheCountAndText() {
    const char* counts[] = {"", "1", "3/9", "3/12", "12/34", "12/345", "123/456", "1234/5678", "12345/6789",
                            "123456789012", "\x7F\x7F\x7F\x7F\x7F\x7F\x7F\x7F"};
    const char* texts[] = {"", "A", "HELLO", "PLUR IS", "PLUR IS NOT DEAD", "\x7F LOVE \x7F", "caf\xC3\xA9 \x01\x0B"};
    for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        for (size_t t = 0; t < sizeof(texts) / sizeof(texts[0]); t++) {
            freshApp();
            app.at(1500, BYE, false);
            assert(runChooser(FakeApp::V3_APP, texts[t], counts[c]));
            assert(!frames.empty());
            for (size_t i = 0; i < frames.size(); i++) {
                assert(frames[i].columnsSent <= 60 || sentColumns(frames[i], 60));
                assert(panelTwoDark(frames[i]));
            }
            assert(clears == 1);
        }
    }
}

static void textFromTheAppLongerThanSixtyFourCharactersIsCutNotOverrun() {
    std::string text = repeat("0123456789", 100);
    freshApp();
    app.at(16000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, text, "3/9"));
    assert(strlen(chooser.text) == CHOOSER_MAX_CHARS);
    // the cut copy equals what the app sends again at the stand-still: no restart
    assert(requestsWhileChoosing() == 1);
    assert(clears == 1);
}

static void escapedSixtyFourCharacterMessageArrivesWholeInAPush() {
    // The app's Gson writes ' < > & = as six-byte \uXXXX escapes: the longest message is about
    // 420 bytes on the wire (0.3 s at 14400 baud), far more than the receive buffer holds, and
    // must still be read as it is pushed.
    std::string escaped, decoded;
    const char* const units[] = {"\\u003c", "\\u003e", "\\u0026", "\\u003d", "\\u0027"};
    const char plain[] = {'<', '>', '&', '=', '\''};
    for (int i = 0; i < 64; i++) {
        escaped += units[i % 5];
        decoded += plain[i % 5];
    }
    std::string wireMessage = chooserJson(escaped, "\\u003cempty\\u003e");
    assert(wireMessage.size() > 400);

    freshApp();
    app.at(1000, wireMessage, true);
    app.at(4000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, "HELLO", "3/9"));
    assert(decoded == chooser.text);
    assert(strcmp(chooser.count, "<empty>") == 0);
    assert(requestsWhileChoosing() == 0);

    size_t change = 1; // frames[0] is after a clear too
    while (change < frames.size() && !frames[change].afterClear) change++;
    assert(change < frames.size());
    assert(sentColumns(frames[change], 60));
    assert(ms(frames[change].tUs - app.pushes[0].messageEndUs) < 15);
    int expectPos = 0;
    for (size_t i = change; i < frames.size(); i++) {
        // "<empty>" is 41 columns wide: the viewport is the last 18 columns
        assert(panelOneIs(frames[i], expectedPanel(decoded, "<empty>", expectPos, GAMMA(CHOOSER_TEXT_BRIGHTNESS), true)));
        assert(panelTwoDark(frames[i]));
        expectPos++;
    }
    assert(expectPos > 30);
}

static void sameMessageAfterAnotherDisplayStartsFresh() {
    // chooser, then something else on the LEDs, then the same chooser message again
    freshApp();
    app.at(3000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
    assert(positionOf(frames.back(), FIRST, "3/9") > 30);
    freshApp();
    app.at(3000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9")); // runChooser() scribbles over the LEDs first
    assert(frames[0].afterClear);
    assert(positionOf(frames[0], FIRST, "3/9") == 0);
    assert(panelTwoDark(frames[0]));
}

static void bytesOutsideTheFontAreDrawnAsSpaces() {
    // a control character and a UTF-8 pair must not read font data from somewhere else
    std::string text = "A\x01\xC3\xA9" "B";
    freshApp();
    app.at(500, BYE, false);
    assert(runChooser(FakeApp::V3_APP, text, "3/9"));
    assert(sentColumns(frames[0], 17 + 1 + 29));
    assert(panelOneIs(frames[0], expectedPanel("A   B", "3/9", 0, GAMMA(145), false)));
}

// ---------------------------------------------------------------------------------------------
// loop()'s side: what is a message
// ---------------------------------------------------------------------------------------------

static void untypedObjectIsNeverAMessageForLoop() {
    // loop() must not take any of these for a message, draw for them, or look inside them
    const char* const notMessages[] = {"{}", "{\"c\":123}", "{\"str\":\"X\",\"flashy\":\"1/1\"}", "{\"type\":5}",
                                       "{\"type\":null,\"str\":\"X\"}", "[]", "7", "null"};
    for (size_t i = 0; i < sizeof(notMessages) / sizeof(notMessages[0]); i++) {
        freshApp();
        arrive(FakeApp::V3_APP, "HELLO", "1/1");
        DeserializationError err = deserializeJson(json, notMessages[i]);
        assert(err == DeserializationError::Ok);
        assert(chooserDispatch() == 0);
        assert(frames.empty());
        assert(requestsWhileChoosing() == 0);
    }
    // a type that begins with the byte loop() reads as "the app did not answer" is an unknown
    // type like any other: the default marquee
    freshApp();
    arrive(FakeApp::V3_APP, "HELLO", "1/1");
    deserializeJson(json, "{\"type\":\"\\u0001\",\"str\":\"X\"}");
    assert(chooserDispatch() == MSGTYPE_DEFAULT);
    // a message with a type but nothing else is shown by loop() (which reads a missing str as "")
    freshApp();
    arrive(FakeApp::V3_APP, "HELLO", "1/1");
    deserializeJson(json, "{\"type\":\"S\"}");
    assert(chooserDispatch() == 'S');
    // a chooser message with no str and no flashy is drawn as an empty chooser, not dereferenced
    freshApp();
    app.at(500, BYE, false);
    arrive(FakeApp::V3_APP, "HELLO", "1/1");
    app.message = "{\"type\":\"H\"}";
    deserializeJson(json, "{\"type\":\"H\"}");
    assert(chooserDispatch() == 'D');
    assert(chooser.text[0] == '\0' && chooser.count[0] == '\0');
    assert(!frames.empty());
}

static void normalReadSkipsUntypedObjectsOnlySoFar() {
    // the message behind two untyped objects is read in the same window
    freshApp();
    arrive(FakeApp::V3_APP, "HELLO", "1/1");
    startSerial();
    sendOnWire(std::string("{\"c\":1}{}") + BYE, nowUs + 2000);
    assert(chooserReadMessage() == DeserializationError::Ok);
    stopSerial();
    assert(strcmp(json["type"], "D") == 0);
    // nothing but untyped objects: the read ends (it does not spin for ever), still without a message
    freshApp();
    arrive(FakeApp::V3_APP, "HELLO", "1/1");
    startSerial();
    sendOnWire(repeat("{}", 200), nowUs + 2000);
    uint64_t began = nowUs;
    assert(chooserReadMessage() == DeserializationError::Ok);
    stopSerial();
    assert(ms(nowUs - began) < 100);
    assert(chooserDispatch() == 0);
    // nothing at all: the usual "no answer"
    freshApp();
    arrive(FakeApp::V3_APP, "HELLO", "1/1");
    startSerial();
    assert(chooserReadMessage() == DeserializationError::EmptyInput);
    stopSerial();
}

static void breakInANormalReadIsSteppedOver() {
    // loop() has just sent its '~' (the sign is showing something else) and the app, which has
    // not seen that request yet, raises its hand for a chooser message. A listening receiver
    // gets the break as a 0x00 byte. That is not "no answer": the message comes behind it.
    for (int variant = 0; variant < 2; variant++) {
        freshApp();
        app.silentFromUs = 0; // this test puts the app's bytes on the line itself
        arrive(FakeApp::V3_APP, "HELLO", "1/1");
        startSerial();
        softSerial.print('~');
        uint64_t lowEnd = variant == 0 ? breakOnWire(nowUs + 2000, 50000)
                                       : sendOnWire(std::string(72, '\0'), nowUs + 2000);
        uint64_t messageEnd = sendOnWire(chooserJson(FIRST, "3/9"), lowEnd + 20000);
        assert(chooserReadMessage() == DeserializationError::Ok);
        stopSerial();
        assert(strcmp(json["type"], "H") == 0 && FIRST == (const char*) json["str"]);
        assert(nowUs == messageEnd);
    }
    // a break and then nothing: "no answer", after the usual wait and not a moment longer
    freshApp();
    app.silentFromUs = 0;
    arrive(FakeApp::V3_APP, "HELLO", "1/1");
    startSerial();
    softSerial.print('~');
    uint64_t began = nowUs;
    breakOnWire(nowUs + 2000, 50000);
    assert(chooserReadMessage() == DeserializationError::EmptyInput);
    stopSerial();
    assert(ms(nowUs - began) > ANSWER_FIRST_BYTE_MS - 1 && ms(nowUs - began) < ANSWER_FIRST_BYTE_MS + 5);
    // and with no break at all the wait is the same
    freshApp();
    app.silentFromUs = 0;
    arrive(FakeApp::V3_APP, "HELLO", "1/1");
    startSerial();
    softSerial.print('~');
    began = nowUs;
    assert(chooserReadMessage() == DeserializationError::EmptyInput);
    stopSerial();
    assert(ms(nowUs - began) > ANSWER_FIRST_BYTE_MS - 1 && ms(nowUs - began) < ANSWER_FIRST_BYTE_MS + 5);
}

static void requestIsNotSentIntoAMessageOnItsWay() {
    // loop() was drawing something else (and could not listen) when the app entered chooser mode
    // and pushed. The drawing ends somewhere in the attention, in the guard behind it, in the
    // pushed message, or after it. Having seen the line LOW, the sign owes the app the time its
    // message needs to begin: it listens and does not ask, and the message is read whole.
    // If it saw nothing (the attention was over before it could look) it asks, and never into
    // the message. One moment is left: its request goes out just as the message's first byte
    // begins. That byte is lost with the message's start, and THE REQUEST STANDS: the sign does
    // not ask again (it cannot know that what crossed was a message and not the app leaving
    // chooser mode, where a second request would take a second message from the list). The
    // request has no answer (a request that crossed a pushed message gets none), the read ends
    // as unanswered with nothing taken and no error, and the next request brings the message.
    for (int zeros = 0; zeros < 2; zeros++) {                             // a break; the run of zero bytes
        int crossings = 0;
        for (int guardMs = 20; guardMs <= 38; guardMs += 18) {            // the contract's guard, and the app's slowest
            for (double intoMs = 0; intoMs < 160; intoMs += 0.13) {
                freshApp();
                arrive(FakeApp::V3_APP, "HELLO", "1/1");
                app.message = chooserJson(FIRST, "3/9");
                nowUs = 5000 * 1000;
                uint64_t handUp = nowUs;
                uint64_t handDown = raiseHand(handUp, zeros);
                uint64_t frameStart = handDown + (uint64_t) guardMs * 1000;
                uint64_t frameEnd = sendOnWire(app.message, frameStart + BYTE_US);
                app.pushes.push_back(Push{handUp, handDown, frameEnd, app.message, true});
                nowUs += (uint64_t) (intoMs * 1000);
                uint64_t entered = nowUs;
                DeserializationError got = exchange();
                if (entered + 100 < handDown) {
                    // it saw the line LOW: no request at all, the pushed message is the answer
                    // (the last 0x00 of a run ends with a stop bit: 69 us of HIGH)
                    assert(app.requests.empty() && lostToTransmit == 0);
                } else if (!app.requests.empty()) {
                    // it asked: then it began to, before the pushed message began (it cannot see that
                    // coming) or 10 ms and more after its end: never into it
                    uint64_t asked = app.requests[0].tUs; // (when the request byte had gone out)
                    assert(asked - BYTE_US < frameStart || asked >= frameEnd + 10000);
                }
                assert(app.requests.size() <= 1);
                if (lostToTransmit) {
                    crossings++;
                    assert(app.requests.size() == 1 && answered() == 0);
                    assert(got == DeserializationError::EmptyInput);
                    assert(exchange() == DeserializationError::Ok); // the next request brings it
                    assert(app.requests.size() == 2 && answered() == 1);
                } else {
                    assert(got == DeserializationError::Ok);
                }
                assert(strcmp(json["type"], "H") == 0 && FIRST == (const char*) json["str"]);
            }
        }
        assert(crossings >= 6); // (the sweep did hit that moment, or this test tested nothing)
    }
}

static void requestWaitsHoweverLongTheLineIsBusy() {
    // The gate's case: a 500-character message with escapes is 700 ms on the wire. A request
    // that is due in the middle of it waits for its end, however long that is.
    std::string text;
    for (int i = 0; i < 500; i++) text += i % 3 ? "\\u003c" : "A";
    std::string frame = "{\"type\":\"D\",\"str\":\"" + text + "\"}";
    assert(frame.size() * BYTE_US > 700 * 1000ull * 2);
    for (double intoMs = 1; intoMs < 1400; intoMs += 97) {
        freshApp();
        arrive(FakeApp::V3_APP, "HELLO", "1/1");
        app.message = BYE;
        nowUs = 5000 * 1000;
        uint64_t frameEnd = sendOnWire(frame, nowUs + BYTE_US);
        nowUs += (uint64_t) (intoMs * 1000);
        assert(chooserRequest() == REQUEST_SENT);
        assert(app.requests.size() == 1);
        assert(app.requests[0].tUs >= frameEnd + 10000);
        assert(chooserReadMessage() == DeserializationError::Ok);
        stopSerial();
        assert(strcmp(json["str"], "bye") == 0);
    }
    // A line that never goes quiet (stuck LOW, or bytes without end): no request is sent into
    // it, and the sign does not wait for ever. It is a request without an answer.
    freshApp();
    arrive(FakeApp::V3_APP, "HELLO", "1/1");
    nowUs = 5000 * 1000;
    breakOnWire(nowUs, 3600ull * 1000 * 1000);
    nowUs += 1000;
    uint64_t began = nowUs;
    assert(chooserRequest() == REQUEST_DEAD);
    assert(app.requests.empty() && !softSerial.listening);
    assert(ms(nowUs - began) >= REQUEST_QUIET_WAIT_MS && ms(nowUs - began) < REQUEST_QUIET_WAIT_MS + 10);
    freshApp();
    arrive(FakeApp::V3_APP, "HELLO", "1/1");
    nowUs = 5000 * 1000;
    sendOnWire(std::string(20000, 'x'), nowUs + BYTE_US); // 14 s of bytes
    nowUs += 1000;
    began = nowUs;
    assert(chooserRequest() == REQUEST_DEAD);
    assert(app.requests.empty() && !softSerial.listening);
    assert(ms(nowUs - began) >= REQUEST_BUSY_WAIT_MS && ms(nowUs - began) < REQUEST_BUSY_WAIT_MS + 10);
    // and loop() then shows its stars, as after any request without an answer
    freshApp();
    app.silentFromUs = 0;
    arrive(FakeApp::V3_APP, FIRST, "3/9");
    breakOnWire(1000 * 1000, 3600ull * 1000 * 1000);
    assert(chooserDispatch() == CHOOSER_DISPATCH_NO_ANSWER);
    assert(app.requests.empty()); // nothing was sent into the dead line
}

static void fastValidReplyIsReadNotAskedForTwice() {
    // The answer to a request may begin the moment the request has gone out. It is a good byte:
    // nothing was sent while it began. It is read, and the app is asked ONCE: in the ordinary
    // display every request takes a message from the app's list, and a second request would
    // lose the first one.
    for (uint64_t afterUs = 0; afterUs <= 3000; afterUs += 25) {
        freshApp();
        arrive(FakeApp::V2_APP, "HELLO", "1/1");
        app.message = BYE;
        app.latencyUs = afterUs; // the reply's start bit falls this long after the request's stop bit
        nowUs = 5000 * 1000;
        assert(exchange() == DeserializationError::Ok);
        assert(strcmp(json["type"], "D") == 0 && strcmp(json["str"], "bye") == 0);
        assert(app.requests.size() == 1);
        assert(lostToTransmit == 0);
    }
}

static void attentionThatCrossesTheRequestCostsNoMessage() {
    // The app raises its hand in the very moment the sign's request goes out: with a break, or
    // with the run of zero bytes it sends where it cannot hold the line (the sign's own app).
    // Whether a message follows (a push: that message is the answer) or not (it has left chooser
    // mode: the request is answered with the next message from its list), the sign asks ONCE,
    // and that one answer is the one it shows. A second request would take a second message
    // from the list for the one shown.
    freshApp();
    arrive(FakeApp::V2_APP, "HELLO", "1/1");
    app.message = BYE;
    nowUs = 5000 * 1000;
    assert(exchange() == DeserializationError::Ok);
    const uint64_t sentUs = app.requests[0].tUs; // when the request byte had gone out, in a run without a break
    // (the contract's guard between the attention and what follows it, and an app that is quicker)
    const uint64_t guardsUs[] = {20000, 2000};
    for (int variant = 0; variant < 8; variant++) {
        const bool zeros = variant & 1, withFrame = variant & 2;
        int crossed = 0;
        for (int64_t at = -1500; at <= 500; at += 20) {
            freshApp();
            arrive(FakeApp::V3_APP, "HELLO", "1/1");
            app.message = BYE;
            app.guardUs = guardsUs[variant >> 2];
            nowUs = 5000 * 1000;
            uint64_t handUp = (uint64_t) ((int64_t) sentUs + at);
            uint64_t handDown = raiseHand(handUp, zeros);
            if (withFrame) {
                app.message = chooserJson(FIRST, "3/9");
                uint64_t frameEnd = sendOnWire(app.message, handDown + app.guardUs + BYTE_US);
                app.pushes.push_back(Push{handUp, handDown, frameEnd, app.message, true});
            }
            assert(exchange() == DeserializationError::Ok);
            assert(strcmp(json["type"], withFrame ? "H" : "D") == 0);
            assert(app.requests.size() <= 1); // (none at all when the sign saw the line LOW before it asked)
            if (!withFrame) {
                assert(answered() == 1); // ONE message was taken from the list, and it is the one in `json`
                assert(strcmp(json["str"], "bye") == 0);
            } else if (app.guardUs >= 20000) {
                // the pushed message itself was read: it is the answer to a request that crossed it,
                // and if the sign saw the line LOW in time and did not ask there was nothing to answer
                assert(answered() == 0);
            }
            // (with a guard shorter than the sign's quiet time before a request, a pushed message
            // can go by while the sign waits for the line to be quiet; it then asks, once, and in
            // chooser mode that takes nothing)
            crossed += lostToTransmit > 0;
        }
        assert(crossed >= 20); // (the attention did begin while the request went out, in that many runs)
    }
}

static void pauseInsideAPushedMessageIsNotAReply() {
    // The app pauses inside a message it pushes (its sending thread is held up for 20 to 80 ms).
    // loop() was drawing and comes back to the line at any moment of it: in the attention, in
    // the first part, in the pause, in the second part. The second part is never taken for a
    // reply: the sign shows no error, and ends with the message. A frame is whole or it is not
    // a frame. (If its request goes out in the very moment the second part begins, the request
    // stands and has no answer; the next one brings the message.)
    const std::string frame = chooserJson(FIRST, "3/9");
    const int pausesMs[] = {20, 45, 80};
    int asked = 0, unasked = 0, stood = 0;
    for (int zeros = 0; zeros < 2; zeros++) {
        for (size_t p = 0; p < 3; p++) {
            for (size_t cut = 4; cut < frame.size(); cut += 11) {
                double totalMs = 50 + 20 + frame.size() * 0.7 + pausesMs[p] + 15;
                for (double intoMs = 0; intoMs < totalMs; intoMs += 1.7) {
                    freshApp();
                    arrive(FakeApp::V3_APP, "HELLO", "1/1");
                    app.message = frame;
                    nowUs = 5000 * 1000;
                    uint64_t handUp = nowUs;
                    uint64_t handDown = raiseHand(handUp, zeros);
                    uint64_t firstEnd = sendOnWire(frame.substr(0, cut), handDown + 20000 + BYTE_US);
                    uint64_t frameEnd = sendOnWire(frame.substr(cut), firstEnd + (uint64_t) pausesMs[p] * 1000 + BYTE_US);
                    // (the app pushed it: a request that crosses it gets no other answer)
                    app.pushes.push_back(Push{handUp, handDown, frameEnd, frame, true});
                    nowUs += (uint64_t) (intoMs * 1000);
                    DeserializationError got = exchange();
                    if (got == DeserializationError::EmptyInput && lostToTransmit) {
                        stood++;
                        assert(app.requests.size() == 1);
                        got = exchange();
                    }
                    if (got != DeserializationError::Ok) {
                        printf("pause %d ms after %zu bytes, back at the line %.1f ms after the attention began: %s\n",
                               pausesMs[p], cut, intoMs, got.c_str());
                    }
                    assert(got == DeserializationError::Ok);
                    assert(strcmp(json["type"], "H") == 0 && FIRST == (const char*) json["str"]);
                    assert(app.requests.size() <= 2);
                    asked += app.requests.size() > 0;
                    unasked += app.requests.empty();
                }
            }
        }
    }
    assert(asked > 200 && unasked > 200); // (both ways of getting the message were taken)
    assert(stood < asked / 20);           // (and the request that stands unanswered is the rare one)
}

static void leavingWithZerosAsTheSyncGoesOutTakesOneMessage() {
    // In the chooser, with the sign's own app: it raises its hand with a run of zero bytes. It
    // leaves chooser mode (zeros and no message) in the very moment the sign's read at the
    // stand-still goes out. That request stands and is answered with the next message of the
    // app's list. The sign must not ask a second time: the second answer would be the message
    // after it, and the first one would never be shown.
    // That holds with the contract's guard of 20 ms between the zeros and the answer, and also
    // for an app that answers as soon as 2 ms after them: the sign listens again a millisecond
    // after the run has ended.
    freshApp();
    app.breakAsZeros = true;
    app.at(20000, BYE, false);
    runChooser(FakeApp::V3_APP, FIRST, "3/9");
    assert(!app.requests.empty());
    const uint64_t sentUs = app.requests[0].tUs; // when the first read at the stand-still had gone out
    const uint64_t guardsUs[] = {20000, 2000};
    for (size_t g = 0; g < 2; g++) {
        int crossed = 0;
        for (int64_t at = -1500; at <= 800; at += 20) {
            freshApp();
            app.breakAsZeros = true;
            app.guardUs = guardsUs[g];
            app.at(((int64_t) sentUs + at) / 1000.0, BYE, false);
            assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
            assert(strcmp(json["str"], "bye") == 0);
            size_t tookFromList = 0;
            for (size_t i = 0; i < app.replies.size(); i++) tookFromList += app.replies[i] == BYE;
            assert(tookFromList == 1);
            crossed += lostToTransmit > 0;
        }
        assert(crossed >= 20);
    }
}

static void glitchWhileTheRequestGoesOutCostsNoMessage() {
    // Noise: the line dips for 30 us while the sign's request goes out. The request stands, the
    // app answers it as fast as it ever does (2 ms) or slowly, and that answer is read: one
    // request, one message taken, that message shown.
    freshApp();
    arrive(FakeApp::V2_APP, "HELLO", "1/1");
    app.message = BYE;
    nowUs = 5000 * 1000;
    assert(exchange() == DeserializationError::Ok);
    const uint64_t sentUs = app.requests[0].tUs;
    const uint64_t latencies[] = {2000, 3500, 5000, 9000, 15000, 200000};
    for (size_t l = 0; l < 6; l++) {
        for (uint64_t into = 20; into < BYTE_US; into += 60) {
            freshApp();
            arrive(FakeApp::V2_APP, "HELLO", "1/1");
            app.message = BYE;
            app.latencyUs = latencies[l];
            nowUs = 5000 * 1000;
            breakOnWire(sentUs - BYTE_US + into, 30);
            assert(exchange() == DeserializationError::Ok);
            assert(strcmp(json["str"], "bye") == 0);
            assert(app.requests.size() == 1 && answered() == 1);
            assert(lostToTransmit == 1); // (the dip did fall into the request)
        }
    }
}

static void braceInsideAPushedTextIsNotAMessage() {
    // A pushed chooser message whose text has a '{' in it, and the app pauses right in front of
    // that byte. loop() comes back to the line in the pause, having seen nothing, and asks. What
    // arrives then opens with a brace and is the rest of a message, not a message:
    //   AB{CD                      not JSON from there on
    //   AB{}CD                     an empty object, then not JSON
    //   AB{                        the text ends there: {","flashy":"3/9"}
    //   x{type:'D',str:'pwned'}    an object with a type, as far as a lenient reader goes
    // None of them may be shown as a message or as an error. The sign lets it go by, asks
    // again, and ends with the chooser message. A message opens with a brace and a double quote.
    const char* const texts[] = {"AB{CD", "AB{}CD", "AB{", "x{type:'D',str:'pwned'}", "x{ type:'D',str:'pwned'}"};
    for (int zeros = 0; zeros < 2; zeros++) {
        for (size_t k = 0; k < 5; k++) {
            const std::string frame = chooserJson(texts[k], "3/9");
            const size_t cut = frame.find('{', 1);
            int tails = 0;
            for (int pauseMs = 20; pauseMs <= 80; pauseMs += 30) {
                double totalMs = 50 + 20 + frame.size() * 0.7 + pauseMs + 15;
                for (double intoMs = 0; intoMs < totalMs; intoMs += 0.5) {
                    freshApp();
                    arrive(FakeApp::V3_APP, "HELLO", "1/1");
                    app.message = frame;
                    nowUs = 5000 * 1000;
                    uint64_t handUp = nowUs;
                    uint64_t handDown = raiseHand(handUp, zeros);
                    uint64_t firstEnd = sendOnWire(frame.substr(0, cut), handDown + 20000 + BYTE_US);
                    uint64_t frameEnd = sendOnWire(frame.substr(cut), firstEnd + (uint64_t) pauseMs * 1000 + BYTE_US);
                    app.pushes.push_back(Push{handUp, handDown, frameEnd, frame, true});
                    nowUs += (uint64_t) (intoMs * 1000);
                    DeserializationError got = exchange();
                    if (got == DeserializationError::EmptyInput && lostToTransmit) {
                        got = exchange(); // (its request crossed the second part: it stood unanswered)
                    }
                    if (got != DeserializationError::Ok || strcmp(json["type"], "H") != 0) {
                        printf("text %s, pause %d ms, back at the line %.1f ms after the attention began: %s%s%s\n",
                               texts[k], pauseMs, intoMs, got.c_str(), got == DeserializationError::Ok ? ", shown: " : "",
                               got == DeserializationError::Ok ? (const char*) json["str"] : "");
                    }
                    assert(got == DeserializationError::Ok);
                    assert(strcmp(json["type"], "H") == 0 && strcmp(json["str"], texts[k]) == 0);
                    assert(app.requests.size() <= 2);
                    // (asked in the pause: the tail was what arrived first)
                    tails += app.requests.size() == 2 && app.requests[0].tUs > firstEnd && app.requests[0].tUs < frameEnd;
                }
            }
            assert(tails >= 20); // (the tail did arrive as the answer to a request, in that many runs)
        }
    }
}

static void messageWithoutAnAttentionTheSignCanSeeIsAskedFor() {
    // On the sign, the app's break is not seen at all (measured: 11 pushes, none seen; the first
    // LOW the sign sees is a bit of the message itself). In the chooser that bit is taken for
    // attention, and the message is already going by: its start is gone. The sign lets ALL of
    // it go by, however long it is (the longest the app writes, 64 escaped characters, is 0.3 s
    // on the wire, three times what the sign waits for a break to end), and then asks once.
    // It never takes one message for three attentions and gives up with nothing asked.
    std::string escaped, plain;
    const char* const units[] = {"\\u003c", "\\u003e", "\\u0026", "\\u003d", "\\u0027"};
    const char* const chars = "<>&='";
    for (int i = 0; i < 64; i++) {
        escaped += units[i % 5];
        plain += chars[i % 5];
    }
    const std::string longMessage = chooserJson(escaped, "\\u003cempty\\u003e");
    const std::string shortMessage = chooserJson(SECOND, "4/9");
    const double lengthsMs[] = {0, 0.2, 1, 3};
    double worstMs[2] = {0, 0};
    for (int which = 0; which < 2; which++) {
        for (size_t l = 0; l < 4; l++) {
            for (double offset = 0; offset < CHOOSER_SCROLL_DELAY_MS; offset += 2.3) {
                freshApp();
                app.breakUs = (uint64_t) (lengthsMs[l] * 1000);
                app.at(2000 + offset, which ? longMessage : shortMessage, true);
                app.at(9000, BYE, false);
                bool other = runChooser(FakeApp::V3_APP, FIRST, "3/9");
                assert(other); // not the stars: the chooser went on, and ended when the app left
                assert(strcmp(json["str"], "bye") == 0);
                // the new message was on the LEDs, from its start, well before the app left
                size_t shown = firstStartOf(which ? plain : SECOND, which ? "<empty>" : "4/9");
                assert(shown < frames.size());
                double late = ms(frames[shown].tUs) - (2000 + offset);
                if (late > worstMs[which]) worstMs[which] = late;
                assert(requestsWhileChoosing() <= 2); // (asked once for it; and the read at a stand-still, if one fell due)
            }
        }
    }
    assert(worstMs[0] < 400 && worstMs[1] < 1000);
    printf("a message whose break the sign cannot see is on the LEDs at the latest %.0f ms (49 bytes) and %.0f ms "
           "(the longest, 0.3 s on the wire) after the app began\n", worstMs[0], worstMs[1]);
}

static void attentionWhileLoopWasDrawingIsSimplyMissed() {
    // loop() was drawing another display through the whole of the app's attention and of what
    // followed it, and comes back to the line afterwards. It has seen nothing, and asks.
    // The app had left chooser mode (attention, nothing behind it): the request is answered
    // with the next message from the list. One request, one message.
    // The app had pushed a chooser message: the request gets it again (in chooser mode a
    // request takes nothing).
    const int backAfterMs[] = {1, 15, 40, 120, 200, 400};
    for (int variant = 0; variant < 4; variant++) {
        const bool zeros = variant & 1, withFrame = variant & 2;
        for (size_t k = 0; k < 6; k++) {
            freshApp();
            arrive(FakeApp::V3_APP, "HELLO", "1/1");
            app.message = withFrame ? chooserJson(FIRST, "3/9") : std::string(BYE);
            nowUs = 5000 * 1000;
            uint64_t handUp = nowUs;
            uint64_t over = raiseHand(handUp, zeros);
            if (withFrame) {
                uint64_t handDown = over;
                over = sendOnWire(app.message, handDown + app.guardUs + BYTE_US);
                app.pushes.push_back(Push{handUp, handDown, over, app.message, true});
            }
            nowUs = over + (uint64_t) backAfterMs[k] * 1000;
            assert(exchange() == DeserializationError::Ok);
            assert(strcmp(json["type"], withFrame ? "H" : "D") == 0);
            assert(app.requests.size() == 1 && answered() == 1 && lostToTransmit == 0);
            assert(app.requests[0].tUs >= over + REQUEST_QUIET_MS * 1000);
        }
    }
}

static void attentionWhileTheSignListensAfterALow() {
    // The sign came back from drawing and saw the line LOW for a moment (noise: a dip of 0.1
    // ms). It owes the app the time a message needs to begin, so it listens and does not ask.
    // In that time the app really raises its hand: with a message behind it (read as it comes,
    // no request at all), or with nothing (it has left chooser mode: the sign then asks, once,
    // and is given the next message from the list).
    const int dipToHandMs[] = {5, 30, 60, 100, 140};
    for (int variant = 0; variant < 4; variant++) {
        const bool zeros = variant & 1, withFrame = variant & 2;
        for (size_t k = 0; k < 5; k++) {
            freshApp();
            arrive(FakeApp::V3_APP, "HELLO", "1/1");
            app.message = BYE;
            nowUs = 5000 * 1000;
            breakOnWire(nowUs + 2000, 100); // the dip, as the sign waits for the line to be quiet
            uint64_t handUp = nowUs + 2000 + (uint64_t) dipToHandMs[k] * 1000;
            uint64_t handDown = raiseHand(handUp, zeros);
            if (withFrame) {
                app.message = chooserJson(FIRST, "3/9");
                uint64_t frameEnd = sendOnWire(app.message, handDown + app.guardUs + BYTE_US);
                app.pushes.push_back(Push{handUp, handDown, frameEnd, app.message, true});
            }
            assert(exchange() == DeserializationError::Ok);
            assert(strcmp(json["type"], withFrame ? "H" : "D") == 0);
            if (withFrame) {
                assert(app.requests.empty()); // the pushed message was read as it came
            } else {
                assert(app.requests.size() == 1 && answered() == 1);
                assert(strcmp(json["str"], "bye") == 0);
            }
        }
    }
}

static void pushOrLeaveWhileTheRestOfABadMessageGoesBy() {
    // In the chooser a pushed message arrives damaged (bytes that are no message). The sign
    // lets the rest of it go by, not listening. Right then the app pushes another message, or
    // leaves chooser mode. That has gone by unheard too; the sign then asks, once, and is given
    // what the app holds now. It never ends in the stars and never takes two messages.
    const std::string third = "THIRD MESSAGE, AS LONG AS THE OTHERS";
    const int gapsMs[] = {30, 80, 130, 200};
    for (int variant = 0; variant < 4; variant++) {
        const bool zeros = variant & 1, leave = variant & 2;
        for (size_t k = 0; k < 4; k++) {
            freshApp();
            app.breakAsZeros = zeros;
            app.damagedPush[1] = "}\x01\x7Fzz{\"t";
            app.at(2000, chooserJson(SECOND, "4/9"), true);
            if (leave) {
                app.at(2000 + gapsMs[k], BYE, false);
            } else {
                app.at(2000 + gapsMs[k], chooserJson(third, "5/9"), true);
                app.at(8000, BYE, false);
            }
            assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
            assert(strcmp(json["str"], "bye") == 0);
            size_t tookFromList = 0;
            for (size_t i = 0; i < app.replies.size(); i++) tookFromList += app.replies[i] == BYE;
            assert(tookFromList == 1);
            if (!leave) {
                size_t shown = firstStartOf(third, "5/9");
                assert(shown < frames.size() && ms(frames[shown].tUs) < 2000 + gapsMs[k] + 1000);
            }
            for (size_t r = 0; r < app.requests.size(); r++) assert(app.requests[r].byte == '~');
        }
    }
}

static void messageWithNoVisibleAttentionThatCrossesTheReadAtTheHold() {
    // The app's break cannot be seen (on the sign it is not), and the first byte of the message
    // it pushes begins in the very moment the sign's read at the stand-still goes out. The
    // request stands. The message's start was lost, and it was the answer: none other comes.
    // The read ends unanswered, the sign asks once more and shows the new message. It does not
    // end in the stars, and the old text stands still on its start meanwhile.
    freshApp();
    app.at(20000, BYE, false);
    runChooser(FakeApp::V3_APP, FIRST, "3/9");
    assert(!app.requests.empty());
    const uint64_t sentUs = app.requests[0].tUs; // when the first read at the stand-still had gone out
    int crossed = 0;
    for (int64_t at = -900; at <= 300; at += 20) {
        freshApp();
        app.breakUs = 0;
        // (the message's first start bit falls a guard after the app "raised its hand")
        uint64_t firstBit = (uint64_t) ((int64_t) sentUs + at);
        app.at((firstBit - app.guardUs) / 1000.0, chooserJson(SECOND, "4/9"), true);
        app.at(sentUs / 1000.0 + 9000, BYE, false);
        assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
        assertOnlyWholePictures(FIRST, "3/9", SECOND, "4/9");
        size_t change = firstStartOf(SECOND, "4/9");
        assert(change < frames.size());
        if (lostToTransmit) {
            crossed++;
            // one request stood unanswered (a second of waiting), the next brought the message
            assert(ms(frames[change].tUs - firstBit) < ANSWER_FIRST_BYTE_MS + 500);
            assert(app.replies[0].empty() && app.replies[1] == chooserJson(SECOND, "4/9"));
        } else {
            assert(ms(frames[change].tUs - firstBit) < 500);
        }
    }
    assert(crossed >= 20);
}

static void leavingAndComingBackIsNotAskedIntoTheGuard() {
    // The sign's own app raises its hand with a run of zero bytes. It leaves chooser mode (zeros,
    // nothing behind them) and is back in it a tenth of a second later (zeros, the guard, the
    // message). The sign is listening for what might follow the first zeros when the second
    // run arrives, so its receiver takes those zeros: no look at the pin ever sees them. They
    // are a LOW seen all the same. The sign owes the app the time its message needs to begin,
    // and sends nothing into the guard: not its fallback request, and not the bench build's
    // line of numbers, which would make it deaf for 19 ms. The message is read as it comes.
    const uint64_t guardsUs[] = {20000, 38000};
    for (size_t g = 0; g < 2; g++) {
        for (double backMs = 60; backMs <= 200; backMs += 0.35) {
            freshApp();
            app.breakAsZeros = true;
            app.guardUs = guardsUs[g];
            app.at(2000, BYE, false);
            app.at(2000 + backMs, chooserJson(SECOND, "4/9"), true);
            app.at(9000, BYE, false);
            bool other = runChooser(FakeApp::V3_APP, FIRST, "3/9");
            if (!other) printf("back after %.2f ms, guard %.0f ms: the chooser ended in the stars\n", backMs, ms(guardsUs[g]));
            assert(other && strcmp(json["str"], "bye") == 0);
            assert(app.pushes.size() == 3 && app.pushes[1].frame);
            const Push& back = app.pushes[1];
            for (size_t r = 0; r < app.requests.size(); r++) {
                uint64_t began = app.requests[r].tUs - BYTE_US;
                // nothing is sent from the moment the second run of zeros begins until its message is over
                assert(!(began >= back.breakStartUs && began < back.messageEndUs));
            }
            size_t tookFromList = 0;
            for (size_t i = 0; i < app.replies.size(); i++) tookFromList += app.replies[i] == BYE;
            assert(tookFromList == 1); // (at the end, when the app left for good)
            size_t shown = firstStartOf(SECOND, "4/9");
            assert(shown < frames.size() && ms(frames[shown].tUs - back.messageEndUs) < 250);
        }
    }
}

static void stallInsideTheRunOfZerosIsNotAskedIntoEither() {
    // The app's sending thread is held up for 80 ms inside the run of zeros itself (after four
    // of them). The sign has seen the first four, waits for the line to be HIGH, listens, and
    // the rest of the run arrives as bytes. The message behind it is still read as it comes;
    // nothing is sent into the guard.
    for (double offset = 0; offset < 2 * CHOOSER_SCROLL_DELAY_MS; offset += 0.31) {
        freshApp();
        app.breakAsZeros = true;
        app.zerosStallUs = 80000;
        app.guardUs = offset < CHOOSER_SCROLL_DELAY_MS ? 20000 : 38000; // the contract's guard; the app's slowest
        app.at(2000 + offset, chooserJson(SECOND, "4/9"), true);
        app.at(9000, BYE, false);
        assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
        const Push& push = app.pushes[0];
        for (size_t r = 0; r < app.requests.size(); r++) {
            uint64_t began = app.requests[r].tUs - BYTE_US;
            assert(!(began >= push.breakStartUs && began < push.messageEndUs));
        }
        size_t shown = firstStartOf(SECOND, "4/9");
        assert(shown < frames.size() && ms(frames[shown].tUs - push.messageEndUs) < 250);
    }
}

static void answerThatCannotBeReadIsAskedForAgain() {
    // The answer to a request the sign really sent, damaged on the line. It is not shown, and
    // not flashed as an error at once: what is left of it is let by and the sign asks again.
    // (The damaged message is lost either way.) Only when three requests in a row are answered
    // with something unreadable is that reported, and loop() flashes it.
    const std::string good = BYE;
    const char* const damage[] = {"", "{\"type\":\"D\",\"str\":\"by\x01", "xyz", "{\"type\":\"D\",,}"};
    for (size_t d = 0; d < 4; d++) {
        for (int inARow = 1; inARow <= 3; inARow++) {
            freshApp();
            arrive(FakeApp::V2_APP, "HELLO", "1/1");
            app.message = good;
            for (int n = 1; n <= inARow; n++) app.damaged[n] = damage[d];
            nowUs = 5000 * 1000;
            DeserializationError got = exchange();
            if (inARow < 3) {
                assert(got == DeserializationError::Ok && strcmp(json["str"], "bye") == 0);
                assert(app.requests.size() == (size_t) inARow + 1);
            } else {
                assert(got != DeserializationError::Ok && got != DeserializationError::EmptyInput);
                assert(app.requests.size() == 3);
            }
        }
    }
    // A dip of the line just before the answer is no damage. The receiver makes a 0xFF of it,
    // which no message contains: it is stepped over and the answer is read. One request.
    for (uint64_t before = 800; before <= 2800; before += 400) {
        freshApp();
        arrive(FakeApp::V2_APP, "HELLO", "1/1");
        app.message = good;
        nowUs = 5000 * 1000;
        // (the request goes out about 10.7 ms from now; the app answers 3 ms after it)
        breakOnWire(nowUs + 10700 + 3000 - before, 40);
        assert(exchange() == DeserializationError::Ok && strcmp(json["str"], "bye") == 0);
        assert(app.requests.size() == 1 && answered() == 1);
    }
    // ... and the same with the 0xFF as a byte in front of the answer
    freshApp();
    arrive(FakeApp::V2_APP, "HELLO", "1/1");
    app.message = good;
    app.damaged[1] = "\xFF\xFF" + good;
    nowUs = 5000 * 1000;
    assert(exchange() == DeserializationError::Ok && strcmp(json["str"], "bye") == 0);
    assert(app.requests.size() == 1);
}

#if CHOOSER_DEBUG_PANEL_B
static void attentionThatCrossesTheBenchLineCostsNoMessage() {
    // The bench build sends 27 characters in front of a request and is deaf for all of them.
    // The app leaves chooser mode (zeros or a break, nothing behind) anywhere in that line or in
    // the request behind it. The line and the request go out whole, the request stands, and it
    // is answered with ONE message from the app's list, which is the one the sign shows.
    for (int zeros = 0; zeros < 2; zeros++) {
        freshApp();
        app.breakAsZeros = zeros;
        app.at(1000, chooserJson(SECOND, "4/9"), true); // an attention, so that there are numbers to send
        app.at(60000, BYE, false);
        runChooser(FakeApp::V3_APP, FIRST, "3/9");
        assert(!app.requests.empty() && app.heard.find('#') != std::string::npos);
        const uint64_t sentUs = app.requests[0].tUs; // when the first request, with its line in front, had gone out
        int crossed = 0;
        for (int64_t at = -21000; at <= 800; at += 190) {
            freshApp();
            app.breakAsZeros = zeros;
            app.at(1000, chooserJson(SECOND, "4/9"), true);
            app.at(((int64_t) sentUs + at) / 1000.0, BYE, false);
            assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));
            assert(strcmp(json["str"], "bye") == 0);
            size_t tookFromList = 0;
            for (size_t i = 0; i < app.replies.size(); i++) tookFromList += app.replies[i] == BYE;
            assert(tookFromList == 1);
            crossed += lostToTransmit > 0;
        }
        assert(crossed >= 60); // (the attention did begin inside the line or the request, in that many runs)
    }
}
#endif

static void breakLateInAReadStillGetsItsMessage() {
    // The app raises its hand in the last moments of a read's one-second wait. The message that
    // follows is still waited for.
    for (double atMs = 880; atMs < 999; atMs += 7) {
        freshApp();
        app.silentFromUs = 0;
        arrive(FakeApp::V3_APP, "HELLO", "1/1");
        chooserRequest();
        uint64_t breakEnd = breakOnWire(nowUs + (uint64_t) (atMs * 1000), 50000);
        sendOnWire(chooserJson(FIRST, "3/9"), breakEnd + 38000);
        assert(chooserReadMessage() == DeserializationError::Ok);
        stopSerial();
        assert(FIRST == (const char*) json["str"]);
    }
    // ... but not for ever: a break and nothing behind it ends the read a quarter second later at most
    freshApp();
    app.silentFromUs = 0;
    arrive(FakeApp::V3_APP, "HELLO", "1/1");
    chooserRequest();
    uint64_t began = nowUs;
    breakOnWire(nowUs + 990 * 1000, 50000);
    assert(chooserReadMessage() == DeserializationError::EmptyInput);
    stopSerial();
    assert(ms(nowUs - began) < ANSWER_FIRST_BYTE_MS + ANSWER_AFTER_BREAK_MS + 5);
    // an object without a type, a break, and then the message: read in the one window
    freshApp();
    app.silentFromUs = 0;
    arrive(FakeApp::V3_APP, "HELLO", "1/1");
    chooserRequest();
    uint64_t objectEnd = sendOnWire("{\"c\":7}", nowUs + 3000);
    uint64_t lowEnd = breakOnWire(objectEnd + 1000, 50000);
    sendOnWire(chooserJson(FIRST, "3/9"), lowEnd + 20000);
    assert(chooserReadMessage() == DeserializationError::Ok);
    stopSerial();
    assert(FIRST == (const char*) json["str"]);
    // the same with white space around everything (an app that ends what it writes with a newline)
    freshApp();
    app.silentFromUs = 0;
    arrive(FakeApp::V3_APP, "HELLO", "1/1");
    assert(chooserRequest() == REQUEST_SENT);
    objectEnd = sendOnWire(" \t{\"c\":7}\r\n", nowUs + 3000);
    lowEnd = breakOnWire(objectEnd + 1000, 50000);
    sendOnWire("\r\n " + chooserJson(FIRST, "3/9") + "\r\n", lowEnd + 20000);
    assert(chooserReadMessage() == DeserializationError::Ok);
    stopSerial();
    assert(FIRST == (const char*) json["str"]);
}

// ---------------------------------------------------------------------------------------------
// The measurements on panel 2 (only in the build with CHOOSER_DEBUG_PANEL_B)
// ---------------------------------------------------------------------------------------------
#if CHOOSER_DEBUG_PANEL_B
// The decimal number drawn in `digits` character cells from column x, -1 if they are not digits
static int numberAt(const Frame& f, size_t x, int digits) {
    int value = 0;
    for (int d = 0; d < digits; d++, x += 6) {
        int digit = -1;
        for (int candidate = 0; candidate < 10 && digit < 0; candidate++) {
            bool same = true;
            for (int c = 0; c < 5; c++) {
                uint8_t want = FontStd5x7[('0' + candidate - 0x20) * 5 + c];
                uint8_t got = f.leds[x + c].dark() ? 0 : f.leds[x + c].row;
                same = same && want == got;
            }
            if (same) digit = candidate;
        }
        if (digit < 0) return -1;
        value = value * 10 + digit;
    }
    return value;
}

static void measurementsAreShownOnPanelTwo() {
    freshApp();
    app.damagedPush[2] = "";
    app.at(2000, chooserJson(SECOND, "4/9"), true);
    app.at(3000, chooserJson(FIRST, "3/9"), true); // this one arrives cut in half
    app.at(6000, BYE, false);
    assert(runChooser(FakeApp::V3_APP, FIRST, "3/9"));

    // on entry: nothing measured yet
    assert(numberAt(frames[0], 60, 3) == 0 && numberAt(frames[0], 78, 3) == 0);
    assert(numberAt(frames[0], 96, 2) == 0 && numberAt(frames[0], 108, 2) == 0);
    // after the first push: one attention, no fallback, and the two times
    size_t change = firstStartOf(SECOND, "4/9");
    const Frame& f = frames[change];
    assert(f.columnsSent == 119); // panel 1, then 59 columns of numbers
    int firstByte = numberAt(f, 60, 3), end = numberAt(f, 78, 3);
    double seenMs = ms(frames[change - 1].tUs); // the line went LOW within a step of this frame
    double wantFirst = ms(app.pushes[0].breakEndUs + app.guardUs) - seenMs;
    double wantEnd = ms(app.pushes[0].messageEndUs) - seenMs;
    assert(firstByte > wantFirst - 40 && firstByte <= wantFirst + 2);
    assert(end > wantEnd - 40 && end <= wantEnd + 2);
    assert(numberAt(f, 96, 2) == 1 && numberAt(f, 108, 2) == 0);
    // the frames after it are panel 1 only again, and panel 2 keeps the numbers
    assert(frames[change + 1].columnsSent == 60);
    assert(numberAt(frames[change + 1], 96, 2) == 1);
    // after the push that came cut in half: two attentions, one fallback
    size_t back = firstStartOf(FIRST, "3/9", change);
    assert(back < frames.size());
    assert(numberAt(frames[back], 96, 2) == 2 && numberAt(frames[back], 108, 2) == 1);

    // The same numbers went to the app, as a line of text in front of each request that was due
    // anyway: the one after the push that came cut in half, and the one after the app left
    // chooser mode (attention and no message).
    //     #SSS fFFF eEEE aAA bBB lLLL~
    assert(app.requests.size() == 2);
    assert(app.heard.size() == 2 * 28);
    for (int n = 0; n < 2; n++) {
        std::string line = app.heard.substr((size_t) n * 28, 28);
        if (wantShow) printf("  the sign told the app: %s\n", line.c_str());
        const char* shape = "#ddd fddd eddd add bdd lddd~";
        for (size_t i = 0; i < 28; i++) {
            if (shape[i] == 'd') assert(line[i] >= '0' && line[i] <= '9'); else assert(line[i] == shape[i]);
        }
        assert(line.size() - 1 <= 31);                    // what the app hands over as one request
        assert(atoi(line.c_str() + 1) == n);              // the lines are numbered
        assert(atoi(line.c_str() + 16) == 2 + n);         // attentions: two pushes, then the leaving
        assert(atoi(line.c_str() + 20) == 1 + n);         // fallbacks: the damaged push, then the leaving
        int low = atoi(line.c_str() + 24);
        assert(low >= 50 - CHOOSER_SCROLL_DELAY_MS - 3 && low <= 51); // the break, from when the sign first saw it
    }
    // the first line has what panel 2 showed after that push
    std::string first = app.heard.substr(0, 28);
    assert(atoi(first.c_str() + 6) == numberAt(frames[back], 60, 3));
    assert(atoi(first.c_str() + 11) == numberAt(frames[back], 78, 3));
    // the second: no message came, so there is no first byte and no end
    std::string second = app.heard.substr(28, 28);
    assert(atoi(second.c_str() + 6) == 0 && atoi(second.c_str() + 11) == 0);
    if (wantShow) printFrame(f, "panel 2 after a push: ms to the first byte, ms to the end, attentions, fallbacks");
}
#endif

// ---------------------------------------------------------------------------------------------

struct Test {
    const char* name;
    void (*run)();
};

#define TEST(name) {#name, name}

static const Test tests[] = {
    TEST(previewThatFitsBlinksAndPausesInTheDark),
    TEST(previewThatFitsIsReadAgainEveryRefresh),
    TEST(leavingChooserIsAttentionAndThenTheSignAsks),
    TEST(leavingWhileTheReadAtTheHoldIsOnItsWay),
    TEST(appThatLeavesWithAFrameIsStillUnderstood),
    TEST(longTextHoldsThenScrollsOneColumnPerDelay),
    TEST(noRequestByteWhileATextIsMoving),
    TEST(slowAnswerAtTheHoldCostsNothing),
    TEST(sixtyFourCharacterTextBesideAFiveCharacterCount),
    TEST(messageTheAppNeverPushedIsPickedUpAtTheNextHold),
    TEST(pushedMessageMidScrollRestartsAtTheHold),
    TEST(changingOnlyTheCountRestartsToo),
    TEST(pushOfTheSameMessageStartsItAgain),
    TEST(scrollingToShortMessageGoesBackToTheBlink),
    TEST(attentionIsSeenInEveryPhaseWhereverTheBreakFalls),
    TEST(shortBreakMayBeMissedButTheSignEndsUpRight),
    TEST(garbageAfterAttentionFallsBackToTilde),
    TEST(secondChangeDuringAPushIsPushedNext),
    TEST(runOfZeroBytesWorksAsABreak),
    TEST(pushThatLandsInTheReadAtTheHoldIsStillTaken),
    TEST(strayMessageWithoutABreakKeepsThePlaceInTheScroll),
    TEST(pushThenTheSyncBringsTheSameMessageAgain),
    TEST(bytesBehindAMessageDoNotStopTheScroll),
    TEST(breakRightBehindAnAnswerIsStillAttention),
    TEST(glitchOnTheLineNeverMakesAColumnComeEarly),
    TEST(silentAppIsNoticedOnlyAtTheHold),
    TEST(lineStuckLowIsGivenUpOn),
    TEST(oneReadThatFailsThenTheSameMessageKeepsThePicture),
    TEST(twoUnansweredRequestsEndInTheStarsLikeAnyOther),
    TEST(failedReadsOfAnEarlierVisitAreForgotten),
    TEST(oldAppWithSevenCharacterPreviewBlinks),
    TEST(neverMoreThanSixtyColumnsWhateverTheCountAndText),
    TEST(textFromTheAppLongerThanSixtyFourCharactersIsCutNotOverrun),
    TEST(escapedSixtyFourCharacterMessageArrivesWholeInAPush),
    TEST(sameMessageAfterAnotherDisplayStartsFresh),
    TEST(bytesOutsideTheFontAreDrawnAsSpaces),
    TEST(untypedObjectIsNeverAMessageForLoop),
    TEST(normalReadSkipsUntypedObjectsOnlySoFar),
    TEST(breakInANormalReadIsSteppedOver),
    TEST(requestIsNotSentIntoAMessageOnItsWay),
    TEST(requestWaitsHoweverLongTheLineIsBusy),
    TEST(fastValidReplyIsReadNotAskedForTwice),
    TEST(attentionThatCrossesTheRequestCostsNoMessage),
    TEST(pauseInsideAPushedMessageIsNotAReply),
    TEST(leavingWithZerosAsTheSyncGoesOutTakesOneMessage),
    TEST(glitchWhileTheRequestGoesOutCostsNoMessage),
    TEST(braceInsideAPushedTextIsNotAMessage),
    TEST(messageWithoutAnAttentionTheSignCanSeeIsAskedFor),
    TEST(attentionWhileLoopWasDrawingIsSimplyMissed),
    TEST(attentionWhileTheSignListensAfterALow),
    TEST(pushOrLeaveWhileTheRestOfABadMessageGoesBy),
    TEST(messageWithNoVisibleAttentionThatCrossesTheReadAtTheHold),
    TEST(leavingAndComingBackIsNotAskedIntoTheGuard),
    TEST(stallInsideTheRunOfZerosIsNotAskedIntoEither),
    TEST(answerThatCannotBeReadIsAskedForAgain),
#if CHOOSER_DEBUG_PANEL_B
    TEST(attentionThatCrossesTheBenchLineCostsNoMessage),
#endif
    TEST(breakLateInAReadStillGetsItsMessage),
#if CHOOSER_DEBUG_PANEL_B
    TEST(measurementsAreShownOnPanelTwo),
#endif
};

int main(int argc, char** argv) {
    const char* only = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--show") == 0) {
            wantShow = true;
        } else if (strcmp(argv[i], "--list") == 0) {
            for (size_t t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) printf("%s\n", tests[t].name);
            return 0;
        } else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--show] [--list] [--only NAME]\n", argv[0]);
            return 2;
        }
    }
    int ran = 0;
    for (size_t t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
        if (only && strcmp(only, tests[t].name) != 0) continue;
        tests[t].run();
        ran++;
    }
    if (!ran) {
        fprintf(stderr, "no test named %s\n", only);
        return 2;
    }
    printf("chooser_display_test%s: all %d tests passed\n", CHOOSER_DEBUG_PANEL_B ? " (measurements on panel 2)" : "", ran);
    return 0;
}
