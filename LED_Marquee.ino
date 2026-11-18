#define DEBUG false

#define uint8_t uint

#include <Arduino.h>

#include "Color.h"

#include "Gamma.h"

#include "Fonts.h"

#include "Icons.h"

// Scroll geometry of the chooser preview (pure functions, unit-tested on the host: see test/)
#include "ChooserScroll.h"

#include "PixelBitBanging.h"

#include "ArduinoJson-v6.18.3.h"

// Used to receive data on a virtual RX pin instead of the usual pin 0
#include "SoftwareSerial/SoftwareSerial.h"

// Pin 13 has an LED connected on most Arduino boards.
// give it a name:
#define diagnosticLed 13

void diagnosticLedOn() {
    digitalWrite(diagnosticLed, HIGH);   // turn the LED on (HIGH is the voltage level)
}

void diagnosticLedOff() {
    digitalWrite(diagnosticLed, LOW);   // turn the LED on (HIGH is the voltage level)
}

void diagnosticBlink() {
    if (DEBUG) {
        diagnosticLedOn();
        delay(5);
        diagnosticLedOff();
        delay(5);
        diagnosticLedOn();
//        delay(5);
//        diagnosticLedOff();
//        delay(300);
    }
}

volatile unsigned long timeOfLastBeat = 0;
const unsigned int FADE_MILLIS = 250; // number of ms beat flash takes to fade out

// http://www.gammon.com.au/interrupts
ISR (PCINT1_vect) {
        // handle pin change interrupt for A0 to A5 here
        if (PINC & bit (0)) {  // if it was high
            timeOfLastBeat = millis();
            diagnosticLedOn();
        } else {
            diagnosticLedOff();
        }
}

#define rxPin 10
#define txPin 11
SoftwareSerial softSerial(rxPin, txPin); // RX, TX

const char ASOT[]
PROGMEM = "                    ";
/*"I had a dream last night... A vision! I saw a world full of people. "
"Everybody was dancing! And screaming loud! They were just there to listen to "
"the music. Some even had their eyes closed. "
"Everybody was just smiling. "
"It was deep. "
"It was underground. "
"Tranceparent. "
"It was magical. "
"It was a happy place... "
"celebrating music! "
"Celebrating life! "
"Men and women - free - without a worry! "
"Then, when I woke up, I realized: "
"I WANNA BE IN THAT MOMENT! "
"The very essence of my existence is looking for that emotion! "
"And when the weekend comes...   "
"I  L I V E  F O R  T H A T  E N E R G Y . "; //598 chars
*/
//How often to advertise "MSG ME!!!", e.g. every 5 marquee scrolls
#define ADVERTISE_EVERY 5
//Speed of invader sequence, lower is faster
#define INVADER_DELAY 50
//Speed of scrolling text marquee, lower is faster
#define MARQUEE_DELAY 19
//Affects how long the all your base style text stays on screen. Higher is faster
#define ALLYOURBASE_DELAY 10
//Speed of chars spelled out one by one effect, lower is faster
#define CHARS_ONEBYONE_DELAY 50
//Speed (delay between frames) of flashy countdown animation, lower is faster
#define COUNTDOWN_DELAY 500

// ---- Chooser preview (MSGTYPE_CHOOSER): a message too wide for the space beside the count scrolls.
// ---- These six values are the ones to tune:
//How long the start of a scrolling message stands still before it moves (and again every time round), in ms
#define CHOOSER_HOLD_MS 600
//Time per scrolled LED column in ms, lower is faster. 35 is about 4.8 characters a second
//(on the sign a step measures 36 to 37 ms: the millisecond clock stands still while a frame is sent)
#define CHOOSER_SCROLL_DELAY_MS 35
//Blank columns between the end of the message and its start coming round again. 18 is three spaces. 1 to 240
#define CHOOSER_GAP_COLS 18
//Brightness of scrolling text, 0-255 before gamma correction. 145 is where the fade blink of a short message starts
#define CHOOSER_TEXT_BRIGHTNESS 145
//While scrolling, ask the app "anything changed?" ('?') every this many columns. Lower reacts faster to !next
#define CHOOSER_POLL_EVERY_COLS 6
//While a text scrolls the app is only asked for a checksum of its message. The message itself is read again
//every time the text has come round, and at the latest after this many ms (one short hitch if the text is moving).
//Lower is safer against a wrong preview staying up, higher hitches long texts less often. 1000 to 30000
#define CHOOSER_REFRESH_MS 8000
// ---- The rest rarely needs touching:
//Ask the app at least every this many ms, whatever the scroll is doing (the app lane counts on 250 at most)
#define CHOOSER_POLL_EVERY_MS 200
//How long to wait for each byte of the app's answer to a chooser poll, in ms. A late answer costs one visible hitch
#define CHOOSER_POLL_TIMEOUT_MS 60
//After this many requests in a row without a usable answer, ask the normal way (which waits up to a second).
//If the same chooser message comes back the scroll carries on from where it was
#define CHOOSER_MAX_FAILED_POLLS 3

// ---- Ads (MSGTYPE_ONE_BY_ONE, MSGTYPE_CHONKY_SLIDE): a text wider than a panel scrolls to its end by itself.
// ---- Their "dly" is real milliseconds. These three values are the ones to tune (see AdDisplay.h):
//How long the start of a too-wide chonky text stands still before it moves, in ms
#define AD_SCROLL_HOLD_MS 600
//Time per scrolled LED column in ms, lower is faster. 35 is the chooser's pace, about 4.8 one-by-one letters a second
#define AD_SCROLL_STEP_MS 35
//Time per frame of the chonky colour wave in ms. 10 is 100 frames a second, what a dly of 1000 always gave
#define AD_WAVE_FRAME_MS 10

// ---- The supply: what the LEDs may draw (see LedBudget.h for the model). 0 = no limit, as it always was.
// ---- A picture that would draw more is dimmed, as a whole, so that it does not. The limits are on what the lit
// ---- LEDs add (the 0.84 A or so that flows with the sign dark is not counted). They hold for every frame of
// ---- every style except two, which are sent as they always were whatever the limits are: the chooser (one
// ---- side only; at most about 3.5 A by the same model) and the starfield (one LED). emulator/README.md has
// ---- the list of everything that lights LEDs and what holds it to the limits.
//The most the LEDs of one frame may draw, both sides of the sign together, in mA
#ifndef LED_CURRENT_LIMIT_MA
#define LED_CURRENT_LIMIT_MA 0
#endif
//The most the LEDs of ONE side may draw, in mA
#ifndef LED_CURRENT_LIMIT_SIDE_MA
#define LED_CURRENT_LIMIT_SIDE_MA 0
#endif
//A chonky ad comes on over this many ms instead of in one frame. 0 = at once
#ifndef AD_RAMP_MS
#define AD_RAMP_MS 0
#endif
// true when one of the two limits is set
#define LED_LIMITS (LED_CURRENT_LIMIT_MA > 0 || LED_CURRENT_LIMIT_SIDE_MA > 0)

// Change this to be at least as long as your pixel string (too long will work fine, just be a little slower)
#define NUM_PANELS 2  // Number of panels. There are 2.
#define COLUMNS_PER_PANEL 60  // Number of columns per panel, in my case, the # of pixels in each LED string. I am using 60LED/M
#define PIXELS NUM_PANELS*COLUMNS_PER_PANEL  // Length of pixels total. I am using 2 meters of 60LED/M

#define INTERCHAR_SPACE 1

// The length of the buffer used to read a new string from the serial port
#define MAX_BUFFER_LEN 600
#define STRINGBUFFER_LEN (MAX_BUFFER_LEN + 1)
#define PADDED_STRINGBUFFER_LEN (STD_STRING_PADDING + STRINGBUFFER_LEN)

// Pad this amount so that scrolling starts nicely off the end, for FontStd
#define STRING_PADDING(font_width) (NUM_PANELS * COLUMNS_PER_PANEL / ((font_width) + INTERCHAR_SPACE))

const int json_capacity = STRINGBUFFER_LEN + JSON_OBJECT_SIZE(8);
StaticJsonDocument <json_capacity> json;

// This is what style the current text will be shown in
const char MSGTYPE_CHONKY_SLIDE = 'C';
const char MSGTYPE_ONE_BY_ONE = 'O';
const char MSGTYPE_FLASHY = 'F';
const char MSGTYPE_COUNTDOWN = 'W';
const char MSGTYPE_STARFIELD = 'S';
const char MSGTYPE_UTILITY = 'U';
const char MSGTYPE_KEYBOARD = 'K';
const char MSGTYPE_CHOOSER = 'H';
const char MSGTYPE_ICON = 'I';
const char MSGTYPE_TRACKID = 'T';
const char MSGTYPE_CHONKYMARQUEE = 'N';
const char MSGTYPE_DEFAULT = 'D';

// Modes to show keyboard mode in
const char KEYBOARD_MODE_WARNING = 'W';

//vertical tab or \v; single column
const char VT = '\u000B';
const char HEART = '\u007F';

// ChooserScroll.h keeps its own copies of these numbers so that it compiles without the sketch
static_assert(CHOOSER_PANEL_COLS == COLUMNS_PER_PANEL, "ChooserScroll.h: panel width");
static_assert(CHOOSER_GLYPH_COLS == FONTSTD_WIDTH && CHOOSER_HEART_COLS == HEART_WIDTH && INTERCHAR_SPACE == 1,
              "ChooserScroll.h: font widths");
static_assert(CHOOSER_CHAR_VT == VT && CHOOSER_CHAR_HEART == HEART, "ChooserScroll.h: special characters");
static_assert(CHOOSER_GAP_COLS >= 1 && CHOOSER_GAP_COLS <= 240, "CHOOSER_GAP_COLS must be 1 to 240");
static_assert(CHOOSER_REFRESH_MS >= 1000 && CHOOSER_REFRESH_MS <= 30000, "CHOOSER_REFRESH_MS must be 1000 to 30000");
static_assert(CHOOSER_POLL_EVERY_MS <= 250, "the app expects a chooser poll at least every 250 ms");

// TextWindow.h (the column arithmetic) and AdDisplay.h keep their own copies of these so that they compile without the sketch
static_assert(TEXT_CHAR_VT == VT && TEXT_CHAR_HEART == HEART && INTERCHAR_SPACE == 1, "TextWindow.h: special characters");

/* Calculates how many empty columns to pad this string on its start and on its end,
 * and stores the results in startPad and endPad. Helps get it to be in the middle of the panel.
 * E.g.
 * (columns - total string width) = startPad, endPad
 * (60-6) = 27, 27
 * (60-5) = 27, 28
 * The width is what the string senders really send: a vertical tab is one column, the heart is
 * as wide as the font says. A string as wide as the panel or wider gets no padding.
 */
void getColumnsToPadForString(const char* str, const Font& font, uint* startPad, uint* endPad) {
    textCentre(textCols(fontMetrics(font), str), COLUMNS_PER_PANEL, startPad, endPad);
}

// Just wait long enough without sending any bits to cause the pixels to latch and display the last sent frame
void show() {
    delayMicroseconds((RES / 1000UL) +
                      1);       // Round up since the delay must be _at_least_ this long (too short might not work, too long not a problem)
}

// Every colour value is shifted right by this before it is sent ('B' utility message). Never more
// than BRIGHTNESS_SHIFT_MAX: the message's handler sees to that, where it stores the value.
#define BRIGHTNESS_SHIFT_MAX 8
uint BRIGHTNESS_SHIFT = 0;

// Send 3 bytes of color data (R,G,B) for a signle pixel down all the connected stringsat the same time
// A 1 bit in "row" means send the color, a 0 bit means send black.
static inline void sendColumnRGB(uint row, uint r, uint g, uint b) {

    sendBitx8(row, g >> BRIGHTNESS_SHIFT, onBits);    // WS2812 takes colors in GRB order
    sendBitx8(row, r >> BRIGHTNESS_SHIFT, onBits);    // WS2812 takes colors in GRB order
    sendBitx8(row, b >> BRIGHTNESS_SHIFT, onBits);    // WS2812 takes colors in GRB order

}

#if LED_LIMITS
// ---- The supply and the styles in this file (flashy, countdown, keyboard echo, marquee, icons).
// ---- The arithmetic is in LedBudget.h. Two things make a limit hold for them:
//
// 1. WHAT IS COUNTED IS WHAT A SIDE OF THE STRIP SHOWS. Flashy, countdown and keyboard echo draw a
//    frame "dry" first: the same code that will send the frame runs once with supplyDry set, and
//    its columns go into supplyPicture (one byte per column of the strip, the rows that are lit)
//    instead of to the LEDs. The lit LEDs of each side are then counted from that picture, and the
//    fuller side decides. So whatever the frame code does (two copies of a text back to back, a
//    side that spans the join, the 21 characters sendString() stops after, the count's digits, a
//    string that is read past its end) is counted as it is drawn, not as the text reads.
//    The marquee and the icons count without a dry run: see there.
//
// 2. THE STRIP IS THE FRAME. Every frame of these styles writes all 120 columns: when the frame's
//    own columns are fewer, the rest are sent black (supplyFrameEnd). So no LED of an earlier
//    picture, of this style or of another, stays lit beside what was counted.
//
// Everything that lights LEDs sends its columns through supplyColumn(), or is one of the ads'
// two styles (AdDisplay.h, which has its own count and always sends 120 columns), or is not
// limited: the chooser (ChooserDisplay.h), the starfield and clear(). The list is in the README.
#include "LedBudget.h"
static bool supplyDry = false;         // a frame is being drawn to be counted: nothing is sent
static uint supplyLeft = 0;            // columns of the strip that the frame being drawn has not written yet
static uint supplyPicture[PIXELS];     // dry: the rows lit in each column of the strip

// A column of a frame of one of these styles.
// The flags are put back the way the caller left them before the column is sent: sendBitx8 sends a
// colour byte that starts with the carry flag set as v | v >> 1 (see AdDisplay.h), a column's green
// starts with whatever the code before it left in the flag, and the counting here must not change
// what that is. emulator/tests/ads_test.py holds an image whose limit is never reached to the
// colours of the image without limits, LED for LED.
static void supplyColumn(uint row, uint r, uint g, uint b) {
    const uint flags = SREG;
    if (supplyDry) {
        if (supplyLeft) { // (what a frame sends beyond the strip's 120 columns falls off its end)
            supplyPicture[PIXELS - supplyLeft] |= row;
            supplyLeft--;
        }
        return;
    }
    if (supplyLeft) {
        supplyLeft--;
    }
    SREG = flags; // (interrupts are off here, and stay off)
    sendColumnRGB(row, r, g, b);
}

// Before the columns of a frame, dry or sent
static inline void supplyFrameBegin() {
    supplyLeft = PIXELS;
}

// After the columns of a frame that is sent: black for the columns it did not reach
static void supplyFrameEnd() {
    while (supplyLeft) {
        supplyColumn(0, 0, 0, 0);
    }
}

// Starts a dry run: frames drawn from here on are laid over each other in supplyPicture
static void supplyDryBegin() {
    memset(supplyPicture, 0, sizeof(supplyPicture));
    supplyDry = true;
    supplyFrameBegin();
}

// The lit LEDs of each side of what the dry run has drawn so far
static void supplyDryLit(uint16_t lit[NUM_PANELS]) {
    lit[0] = lit[1] = 0;
    for (uint c = 0; c < PIXELS; c++) {
        lit[c >= COLUMNS_PER_PANEL] += ledBits(supplyPicture[c] & onBits);
    }
}

// Ends the dry run. Returns the lit LEDs of the fuller side of what was drawn.
static uint16_t supplyDryEnd() {
    uint16_t lit[NUM_PANELS];
    supplyDry = false;
    supplyDryLit(lit);
    return lit[0] > lit[1] ? lit[0] : lit[1];
}

#define SEND_COLUMN supplyColumn
#define SUPPLY_FRAME_BEGIN() supplyFrameBegin()
#define SUPPLY_FRAME_END() supplyFrameEnd()
#else
// No limit: the styles in this file send their columns as they always did
#define SEND_COLUMN sendColumnRGB
#define SUPPLY_FRAME_BEGIN()
#define SUPPLY_FRAME_END()
#endif

// Turn off all pixels
static inline void clear() {

    cli();
    for (unsigned int i = 0; i < PIXELS; i++) {

        sendColumnRGB(0, 0, 0, 0);
    }
    sei();

    show();
}

// Send the pixels to form the specified char, not including interchar space
// skip is the number of pixels to skip at the begining to enable sub-char smooth scrolling

// TODO: Subtract the offset from the char before starting the send sequence to save time if necessary
// TODO: Also could pad the beginning of the font table to aovid the offset subtraction at the cost of 20*8 bytes of progmem
// TODO: Could pad all chars out to 8 bytes wide to turn the the multiply by FONTSTD_WIDTH into a shift

static void sendChar(
        const uint ch,
        uint skip,
        uint r, uint g, uint b,
        boolean sendTrailingSpace = true,
        const Font& font = fontStd) {

    // For the VT char, we only send a single column (so just the interchar space)
    if (ch != VT) {
        // A byte the font has no picture for is drawn as a space
        const uint* charbase = font.data + ((textInFont(ch) ? ch - ' ' : 0) * font.width);
        uint col = font.width;
        if (ch == HEART) {
            col = font.heartWidth; // the heart can be wider than the other characters
        }
        if (skip > col) {
            skip = col;
        }

        while (skip--) {
            charbase++;
            col--;
        }

        while (col--) {
            SEND_COLUMN(pgm_read_byte_near(charbase++), r, g, b);
        }

        // TODO: FLexible interchar spacing
    }
    if (sendTrailingSpace) {
        SEND_COLUMN(0, r, g, b);    // Interchar space
    }

}

static void sendString(const char* str, const Font& font, uint skip, const uint r, const uint g, const uint b) {
    unsigned int l = PIXELS / (font.width + INTERCHAR_SPACE);

    sendChar(*str, skip, r, g, b, true, font); // First char is special case because it can be stepped for smooth scrolling

    // Send rest of string
    while (*(++str) && l--) {
        sendChar(*str, 0, r, g, b, *(str + 1) != '\0'); //dont send interchar space on last char
    }
}

// Show the passed string. The last letter of the string will be in the rightmost pixels of the display.
// Skip is how many cols of the 1st char to skip for smooth scrolling
static inline void
sendPaddedString(const char* padding, const char* str, const Font& font, uint skip, const uint r, const uint g, const uint b) {

    unsigned int l = PIXELS / (font.width + INTERCHAR_SPACE);

    if (*padding) {
        sendChar(*padding, skip, r, g, b, true, font); // First char is special case because it can be stepped for smooth scrolling

        while (*(++padding) && l--) {
            sendChar(*padding, 0, r, g, b, true, font);
        }

        sendChar(*str, 0, r, g, b, true, font); // First char is special case because it can be stepped for smooth scrolling
    } else {
        sendChar(*str, skip, r, g, b, true, font); // First char is special case because it can be stepped for smooth scrolling
    }

    // Send rest of string
    while (*(++str) && l--) {
        sendChar(*str, 0, r, g, b, true, font);
    }
}

void sendEmptyColumns(int columns) {
    while (columns--) {
        SEND_COLUMN(0, 0, 0, 0);
    }
}

static inline void
sendStringJustified(const char* s, const Font& font, uint skip, const uint r, const uint g, const uint b,
                    uint startPad, uint endPad) {
    // startPad and endPad are from getColumnsToPadForString(): the caller works them out before it
    // turns interrupts off, so that walking the string does not make a pause in the middle of a frame
    sendEmptyColumns(startPad);
    sendString(s, font, skip, r, g, b);
    sendEmptyColumns(endPad);
}

void setupDiagnosticLed() {
    pinMode(diagnosticLed, OUTPUT);
}

// Set the specified pins up as digital out

void setupLeds() {
    PIXEL_DDR |= onBits;   // Set all used pins to output
}

void startSerial() {
    // set the data rate for the SoftwareSerial port
    softSerial.begin(14400);
    softSerial.flush();
//    Serial.begin(19200);
}

void stopSerial() {
    softSerial.end();
}

void setupSerial() {
    // define pin modes for tx, rx:
    pinMode(rxPin, INPUT);
    pinMode(txPin, OUTPUT);
}

#if LED_LIMITS
// (defined further down, after AdDisplay.h, whose count of a text's LEDs the marquee uses)
static uint16_t supplyAllowed(uint16_t litPerSide);
static uint16_t supplyMostLit(const Font& font, const char* s);
static uint supplyScale(uint16_t litPerSide, uint r, uint g, uint b, bool fades);

// Dims a colour by just as much as it takes for one LED of it to send no more than `allowed`.
// A colour that is within it is left as it is, at no cost; the search for a dimming takes about
// 1.5 ms. Call it before a frame, not in one.
static void supplyFit(uint16_t allowed, uint& r, uint& g, uint& b) {
    if ((uint16_t) ledCarried(r) + ledCarried(g) + ledCarried(b) > allowed) {
        const uint supply = ledBudgetScale(allowed, r, g, b, false, true, BRIGHTNESS_SHIFT, false);
        r = ledScaled(r, supply);
        g = ledScaled(g, supply);
        b = ledScaled(b, supply);
    }
}
#endif

// The columns of one frame of the keyboard echo, from showAsInputStyle()'s own variables: the
// characters in white (the string, except for its last char, which is '_'), the one at idxToBlink
// (or all of them in input warning mode) in the blinking colour, and one dark column.
// It is a macro, not a function, so that the image without limits stays the one it was, byte for
// byte: the same statements in the same place. Use it with interrupts off, or dry (see supplyColumn).
#define INPUT_FRAME(blinkR, blinkG, blinkB, whiteR, whiteG, whiteB) \
    for (uint i = 0; i < length; i++) { \
        if (i == idxToBlink || mode == KEYBOARD_MODE_WARNING) { \
            sendChar(str[i], 0, blinkR, blinkG, blinkB); \
        } else { \
            sendChar(str[i], 0, whiteR, whiteG, whiteB); \
        } \
    } \
    SEND_COLUMN(0x00, 0, 0, 0xff)

// TODO idxToBlink that isn't always last char
void showAsInputStyle(const char* str, int idxToBlink, const char mode) {

//    str = str + STRING_PADDING(FONTSTD_WIDTH) + 1; // drop initial padding

    int length = strlen(str);
    int shiftBy = constrain(length - MAX_CHARS_PER_PANEL, 0, length);
    // Shift string from the start if it is longer than the max chars we can show,
    // so that last MAX_CHARS_PER_PANEL chars are always visible (plus _)
    str = str + shiftBy;

    unsigned int count = 2;
    clear();
    idxToBlink = strlen(str) - 1;

//    cli();
//    // Send string, except for last char (which is '_')
//    for (uint i = 0; i < length; i++) {
//        int r = 0xff;
//        int g = 0xff;
//        int b = 0xff;
////        if (i == idxToBlink ) {
////            sendChar(str[idxToBlink], 0, r, brightness, brightness);
////        } else {
//            sendChar(str[i], 0, GAMMA(r), GAMMA(g), GAMMA(b));
////        }
//    }

#if LED_LIMITS
    // What the frame lights on a side is counted from a dry run of it; white, the brightest colour
    // in it, is brought down to what that many LEDs may send, and so is the blinking colour.
    supplyDryBegin();
    INPUT_FRAME(0, 0, 0, 0, 0, 0);
    const uint16_t allowed = supplyAllowed(supplyDryEnd());
    uint whiteR = GAMMA(0xff), whiteG = GAMMA(0xff), whiteB = GAMMA(0xff);
    supplyFit(allowed, whiteR, whiteG, whiteB);
#endif

    while (count > 0) {
        count--;
        uint brightness = ((count % 100) * 256) / 100;
#if LED_LIMITS
        uint blinkR = 0xff, blinkG = brightness, blinkB = brightness;
        supplyFit(allowed, blinkR, blinkG, blinkB);
#endif

        cli();
        SUPPLY_FRAME_BEGIN();
#if LED_LIMITS
        INPUT_FRAME(blinkR, blinkG, blinkB, whiteR, whiteG, whiteB);
#else
        INPUT_FRAME(0xff, brightness, brightness, GAMMA(0xff), GAMMA(0xff), GAMMA(0xff));
#endif
        SUPPLY_FRAME_END();
        sei();
        show();
    }
}

// The columns of one frame of the flashy and countdown styles, from showAsCountdownStyle()'s own
// variables: the string (in the middle of the panel when the count is not shown), then, with
// showCount, the count as "d1.d2d3" in digitRed; and the same again for the other panel.
// It is a macro, not a function, so that the image without limits stays the one it was, byte for
// byte: the same statements in the same place. Use it with interrupts off, or dry (see supplyColumn).
#define COUNTDOWN_FRAME(d1, d2, d3, digitRed) \
    if (showCount) { \
        sendString(countdownstr, fontStd, 0, brightness, brightness, brightness); \
    } else { \
        sendStringJustified(countdownstr, fontStd, 0, brightness, brightness, brightness, startPad, endPad); \
    } \
    if (showCount) { \
        sendChar(d1, 0, digitRed, 0, 0); \
        sendChar('.', 0, digitRed, 0, 0); \
        sendChar(d2, 0, digitRed, 0, 0); \
        sendChar(d3, 0, digitRed, 0, 0, false); \
        sendString(countdownstr, fontStd, 0, brightness, brightness, brightness); \
        sendChar(d1, 0, digitRed, 0, 0); \
        sendChar('.', 0, digitRed, 0, 0); \
        sendChar(d2, 0, digitRed, 0, 0); \
        sendChar(d3, 0, digitRed, 0, 0, false); \
    } else { \
        sendStringJustified(countdownstr, fontStd, 0, brightness, brightness, brightness, startPad, endPad); \
    }

// count: at most 1000, so that the first digit of the count is a digit (the callers pass 600 at most)
void showAsCountdownStyle(const char* countdownstr, unsigned int count = 600, boolean showCount = true) {
    uint startPad, endPad; // for the string in the middle of a panel (when the count is not shown)
    getColumnsToPadForString(countdownstr, fontStd, &startPad, &endPad);
#if LED_LIMITS
    // What a frame lights on a side is counted from dry runs of it. The picture is the same in
    // every frame but for the count's digits. First the frame with blanks for digits: the string,
    // which at its brightest is white (the count's point is counted with it). Then, with a count, ten frames
    // laid over that, "0.00" to "9.99", so that each place holds every digit it will ever show:
    // what they add are the digits' LEDs.
    // A digit's LED is red only, and never more than half of what a white LED may send:
    //  - while the count runs it is 0x80 dimmed like the string's white: with the white's top at
    //    s, at most 0.75 (s + 1) as sent (half of s + 1, and sendBitx8 can make it half as much
    //    again), where a white LED may send 3 s or more; and nothing at all when s is 0;
    //  - in the farewell blink it is the red of the string's white: a third of it.
    // So a side counts as its string's LEDs and half of its digits' (rounded up). How far white
    // has to come down for that many is worked out here, once, before any frame; the fade then
    // runs below it (so every brightness below has to fit as well).
    uint16_t litString[NUM_PANELS], litAll[NUM_PANELS];
    supplyDryBegin();
    {
        const uint brightness = 0; // dry: the colours do not matter
        COUNTDOWN_FRAME(' ', ' ', ' ', 0)
        supplyDryLit(litString);
        for (uint digit = '0'; showCount && digit <= '9'; digit++) {
            supplyFrameBegin();
            COUNTDOWN_FRAME(digit, digit, digit, 0)
        }
    }
    supplyDryLit(litAll);
    supplyDryEnd();
    for (uint side = 0; side < NUM_PANELS; side++) {
        litAll[side] = litString[side] + (litAll[side] - litString[side] + 1) / 2;
    }
    const uint supply = supplyScale(litAll[0] > litAll[1] ? litAll[0] : litAll[1], 0xff, 0xff, 0xff, true);
    const uint countRed = ledScaled(0x80, supply);
#else
    const uint countRed = 0x80;
#endif

    clear();

    while (count > 0) {

        count--;

        uint digit1 = count / 100;
        uint digit2 = (count - (digit1 * 100)) / 10;
        uint digit3 = (count - (digit1 * 100) - (digit2 * 10));

        uint char1 = digit1 + '0';
        uint char2 = digit2 + '0';
        uint char3 = digit3 + '0';

        uint brightness = GAMMA(((count % 100) * 256) / 100);
#if LED_LIMITS
        brightness = ledScaled(brightness, supply);
#endif

        cli();
        SUPPLY_FRAME_BEGIN();
        COUNTDOWN_FRAME(char1, char2, char3, countRed)
        SUPPLY_FRAME_END();
        sei();
        delayMicroseconds(COUNTDOWN_DELAY);
        show();
    }

    count = 100;

    // One last farewell blink
    while (count > 0) {

        count--;

        uint brightness = GAMMA(((count % 100) * 256) / 100);
#if LED_LIMITS
        brightness = ledScaled(brightness, supply);
#endif

        cli();
        SUPPLY_FRAME_BEGIN();
        COUNTDOWN_FRAME('0', '0', '0', brightness)
        SUPPLY_FRAME_END();
        sei();
        delayMicroseconds(COUNTDOWN_DELAY);
        show();
    }
}

void showAsFlashyStyle(const char* countdownstr, unsigned int time = 200) {
    showAsCountdownStyle(countdownstr, time, false);
}

void showstarfieldcustom(int stars) {
    const uint field = 40;       // Good size for a field, must be less than 256 so counters fit in a byte
    uint sectors = (PIXELS / field);      // Repeating sectors makes for more stars and faster update

    for (unsigned int i = 0; i < stars; i++) {
        unsigned int r = random(PIXELS * 8);   // Random slow, so grab one big number and we will break it down.

        unsigned int x = r / 8;
        uint y = r & 0x07;                // We use 7 rows
        uint bitmask = (2 << y);           // Start at bit #1 since we never use the bottom bit

        cli();

        unsigned int l = x;
        while (l--) {
            sendColumnRGB(0, 0x00, 0x00, 0x00);
        }
        sendColumnRGB(bitmask, 0x40, 0x40, 0xff);  // Starlight blue

        l = PIXELS - x;
        while (l--) {
            sendColumnRGB(0, 0x00, 0x00, 0x00);
        }

        sei();
        // show(); // Not needed - random is always slow enough to trigger a reset
    }

}

void showstarfield() {
    showstarfieldcustom(300);
}

static inline void sendIcon(const uint* fontbase, uint which, int8_t shift, uint width, uint r, uint g, uint b) {

    const uint* charbase = fontbase + (which * width);

    if (shift < 0) {
        uint shiftabs = -1 * shift;
        while (width--) {
            uint col = pgm_read_byte_near(charbase++);
            SEND_COLUMN(col << shiftabs, r, g, b);
        }
    } else {
        while (width--) {
            SEND_COLUMN((pgm_read_byte_near(charbase++) >> shift) & onBits, r, g, b);
        }
    }

}

// The ads' two styles: showCharsOneByOneOnBothPanels() and showChonkySlideStyleOnBothPanels(), with
// the clock that makes their "dly" real milliseconds and the scrolling of a text wider than a panel.
// They are in their own file so that the host test can run them against a fake sign and a fake timer.
#include "AdDisplay.h"

#if LED_LIMITS
// For the styles in this file a frame's first green byte starts with whatever is in the carry
// flag, so green is taken as carried: the most it can be.

// The most LEDs one side can have lit while a marquee scrolls this text: the fullest 60 columns
// in a row of it. A marquee frame is one run of consecutive columns of "blank padding, then the
// text" (sendPaddedString() starts somewhere in it and stops after some characters), a side shows
// 60 consecutive columns of the frame, and the columns the frame does not reach are sent black.
static uint16_t supplyMostLit(const Font& font, const char* s) {
    return adMostLit(font, fontMetrics(font), s);
}

// What one lit LED may send, its three values added up, when a side has litPerSide LEDs lit
static uint16_t supplyAllowed(uint16_t litPerSide) {
    return ledAllowance(litPerSide, LED_LOAD_OF_MA(LED_CURRENT_LIMIT_MA), LED_LOAD_OF_MA(LED_CURRENT_LIMIT_SIDE_MA));
}

// How far a picture with litPerSide LEDs lit on a side, all in this colour, has to be dimmed
// (255 = not at all). `fades`: the picture is also shown at every brightness below this one (the
// flashy fade), so each of those has to fit too.
static uint supplyScale(uint16_t litPerSide, uint r, uint g, uint b, bool fades) {
    return ledBudgetScale(supplyAllowed(litPerSide), r, g, b, false, true, BRIGHTNESS_SHIFT, fades);
}
#endif

void showIconInvaders(const Icon* icon, Color iconColor) {
    Color ic = iconColor;
    int iconWidth = pgm_read_byte_near(icon);
//    char blah[4];
//    itoa(iconWidth, blah, 10);
//    showAsFlashyStyle(blah);

    uint acount = PIXELS / (iconWidth + FONTSTD_WIDTH);      // How many aliens do we have room for?

#if LED_LIMITS
    // The most LEDs a side can have lit: the fuller of the icon's two pictures, as often as an icon
    // (with the 4 dark columns after it) fits on a side, and one more that is cut by the edge.
    // (All eight bits of a column are counted: the bottom one drives no LED where it is, but the
    // rows move up as the icons come in, and it then lights the one above.)
    uint16_t iconLit[2] = {0, 0};
    for (uint f = 0; f < 2; f++) {
        for (uint c = 0; c < iconWidth; c++) {
            iconLit[f] += ledBits(pgm_read_byte_near(icon->data + f * iconWidth + c));
        }
    }
    const uint16_t litPerSide = (iconLit[0] > iconLit[1] ? iconLit[0] : iconLit[1]) *
                                (COLUMNS_PER_PANEL / (iconWidth + 4) + 1);
    const uint supply = supplyScale(litPerSide, GAMMA(ic.r), GAMMA(ic.g), GAMMA(ic.b), false);
    const uint iconR = ledScaled(GAMMA(ic.r), supply);
    const uint iconG = ledScaled(GAMMA(ic.g), supply);
    const uint iconB = ledScaled(GAMMA(ic.b), supply);
#endif

    for (int8_t row = -7; row < 7; row++) {     // Walk down the rows
        //  Walk them 6 pixels per row
        // ALternate direction on each row

        uint start, end, step;

        if (row & 1) {
            start = 1;
            end = 8;
            step = 1;
        } else {
            start = 7;
            end = 0;
            step = -1;
        }

        for (char p = start; p != end; p += step) {
            // Now slowly move aliens
            // work our way though the aliens moving each one to the left
            cli();
            SUPPLY_FRAME_BEGIN();

            // Start with margin
            uint margin = p;

            while (margin--) {
                SEND_COLUMN(0, 0x00, 0x00, 0x00);
            }

            for (uint l = 0; l < acount; l++) {
#if LED_LIMITS
                sendIcon(icon->data, p & 1, row, iconWidth, iconR, iconG, iconB);
#else
                sendIcon(icon->data,
                         p & 1,
                         row,
                         iconWidth,
                         GAMMA(ic.r), GAMMA(ic.g), GAMMA(ic.b));
#endif
//                sendChar(' ', 0, 0x00, 0x00, 0x00); // No over crowding
                sendEmptyColumns(4);
            }

            SUPPLY_FRAME_END();
            sei();

            delay(INVADER_DELAY);
        }
    }
    // delay(200);
}

#define JAB_MAX_BRIGHTNESS 0xff //(255 (100%))
//#define JAB_MAX_BRIGHTNESS 0x7f //(127)
#define JAB_MIN_BRIGHTNESS 0x00
#define JAB_STEPS (JAB_MAX_BRIGHTNESS-JAB_MIN_BRIGHTNESS)

// Keep color step between marquee calls, but still bump the sector by 1 every call so each new message starts with a
// noticeably different color
uint sector = 1;
uint colorStep = 0;

void marquee(const char* marqueePtr, const Font& font = fontStd, bool pad = true, uint marqueeDelay = MARQUEE_DELAY) {
    // Dynamically allocate memory for paddingString based on the font width
    char* paddingString = (char*) malloc(STRING_PADDING(font.width) + 1);
    if (!paddingString) {
        // Handle memory allocation failure if necessary
        return;
    }
    memset(paddingString, ' ', STRING_PADDING(font.width));
    paddingString[STRING_PADDING(font.width)] = '\0'; // null terminate the string
    const char* paddingPtr = paddingString;
    if (pad) {
        paddingPtr = paddingString;
    } else {
        paddingPtr += STRING_PADDING(font.width) / 2;
    }

    // Text foreground color cycle effect
    sector++;
    if (sector == 3) {
        sector = 0;
    }

    float beatPct = 0.0f;
    unsigned long delta;
    uint charWidth;
#if LED_LIMITS
    // What one lit LED may send when a side shows the fullest 60 columns of this text (see supplyMostLit)
    const uint16_t allowed = supplyAllowed(supplyMostLit(font, marqueePtr));
#endif

    while (*marqueePtr) {

        if (colorStep == JAB_STEPS) {
            colorStep = 0;
            sector++;
            if (sector == 3) {
                sector = 0;
            }
        } else {
            colorStep++;
        }

        uint rampup = GAMMA(JAB_MIN_BRIGHTNESS + colorStep);
        uint rampdown = GAMMA(JAB_MIN_BRIGHTNESS + (JAB_STEPS - colorStep));

        uint r0, g0, b0, r, g, b;

        switch (sector) {
            case 0:
                r0 = rampup;
                g0 = rampdown;
                b0 = JAB_MIN_BRIGHTNESS;
                break;
            case 1:
                r0 = rampdown;
                g0 = JAB_MIN_BRIGHTNESS;
                b0 = rampup;
                break;
            case 2:
                r0 = JAB_MIN_BRIGHTNESS;
                g0 = rampup;
                b0 = rampdown;
                break;
        };

        if (!*paddingPtr && *marqueePtr == HEART) {
            charWidth = font.heartWidth;
        } else {
            charWidth = font.width;
        }

        for (uint step = 0; step < charWidth + INTERCHAR_SPACE; step++) {
            // step though each column of the 1st char for smooth scrolling

            delta = millis() - timeOfLastBeat;
            // beat proportion
            beatPct = constrain((float) delta / (float) FADE_MILLIS, 0.0f, 1.0f);

            if (beatPct >= 1.0f) {
                r = r0;
                g = g0;
                b = b0;
            } else if (beatPct > 0.0f) {
                // Fade between white and rgb depending on how long ago the beat was
                r = (uint)((1.0f - beatPct) * (float) JAB_MAX_BRIGHTNESS + beatPct * (float) r0);
                g = (uint)((1.0f - beatPct) * (float) JAB_MAX_BRIGHTNESS + beatPct * (float) g0);
                b = (uint)((1.0f - beatPct) * (float) JAB_MAX_BRIGHTNESS + beatPct * (float) b0);
            } else {
                // beatPct == 0.0, meaning it just happened.
                // Flash on beats
                // Full white to save doing the computation cycles
                r = JAB_MAX_BRIGHTNESS;
                g = JAB_MAX_BRIGHTNESS;
                b = JAB_MAX_BRIGHTNESS;
            }
#if LED_LIMITS
            // The text's own colour is seldom over the limit; the white of a beat flash is. Only then
            // is the dimming worked out (about 1.5 ms, before the frame, so such a step is that much
            // longer): the flash stays a flash, as bright as the supply allows, and fades into the
            // text's colour as before.
            supplyFit(allowed, r, g, b);
#endif

            cli();
            SUPPLY_FRAME_BEGIN();

            sendPaddedString(paddingPtr, marqueePtr, font, step, r, g, b);

            SUPPLY_FRAME_END();
            sei();

            delay(marqueeDelay); // speed. higher = slower

            PORTB |= 0x01;
            delay(1);
            PORTB &= ~0x01;
        }

        if (*paddingPtr) {
            paddingPtr++;
        } else {
            marqueePtr++;
        }
    }

    free(paddingString);
}

// The chooser display (MSGTYPE_CHOOSER): showAsChooser() with its scrolling and polling, and the two
// decisions readSerialData() and loop() make about a reply (chooserReadMessage, chooserDispatch).
// It is in its own file so that the host test can run it against a fake sign and a fake app.
#include "ChooserDisplay.h"

// Notifies the IMX that we're ready to retrieve custom message data
bool readSerialData() {
    startSerial();
    // Flushing Serial input buffer
//            while (softSerial.available())
//                softSerial.read();

//            while (isspace(Serial.peek())) {
//                Serial.read();
//            }

    // send special symbol so IMX knows to respond with custom message data
    softSerial.print('~');
//    int firstByte = softSerial.read();
//
//    switch (firstByte) {
//
//    }

    // Read the JSON document from the serial port (stepping over a late answer to a chooser poll,
    // which is an object without a "type" and not a message)
    DeserializationError err = chooserReadMessage();

    if (err == DeserializationError::Ok) {
        stopSerial();
        diagnosticBlink();
        return true;
    } else if (err == DeserializationError::EmptyInput) {
        return false;
    } else {
        softSerial.print(err.c_str());

//        char bytes[10] = {'\0'};
//        softSerial.readBytes(bytes, 1);
//        softSerial.print(":");
//        softSerial.print(bytes);
//
//        // do I need to clear out the rest of the unread serial stream?
//        while(softSerial.available()) {
//            softSerial.read();
//        }
        stopSerial();
        showAsFlashyStyle((char*) err.c_str());
        diagnosticBlink();
        diagnosticBlink();
        diagnosticBlink();
        diagnosticBlink();
        return false;
    }
}

int loopcount = 0;

void setup() {
//    strcpy_P(currentBuffer, ASOT);

//    if (DEBUG) {
//        currentBuffer[10] = 0x00; //chop off intro when debugging
//    }

    delay(100); //Try to introduce a bit of a delay to clear out serial noise on boot
    setupDiagnosticLed();
    setupLeds();
    adClockSetup(); // Timer1 becomes the clock of the ads (AdDisplay.h)

    showAsFlashyStyle("I", 60);
    showAsFlashyStyle("LIVE", 60);
    showAsFlashyStyle("FOR", 60);
    showAsFlashyStyle("THAT", 60);
    showAsFlashyStyle("ENERGY!", 60);
//    showAsCountdownStyle("NEW\u000BMSG\u000B", 600);
    showstarfield();

    setupSerial();

    // A0 pin change interrupt
    digitalWrite(A0, HIGH);  // enable pull-up
    PCMSK1 |= bit(PCINT8);  // want pin A0
    PCIFR |= bit(PCIF1);   // clear any outstanding interrupts
    PCICR |= bit(PCIE1);   // enable pin change interrupts for A0 to A5

    loopcount = 0;
}

void loop() {
    diagnosticLedOn();
    bool readSuccessful = readSerialData();
    diagnosticLedOff();

    if (readSuccessful) {
        // A chooser message is shown in there, for as long as the chooser is on screen (the app is
        // asked '?' between frames); what comes back is the type of the message that ended it.
        // 0 means there is nothing to show: what was read is not a message (it has no "type"), or
        // the chooser's reads kept failing. Draw nothing and ask again with a normal '~'.
        const char msgType = chooserDispatch();
        if (!msgType) {
            loopcount++;
            return;
        }
        const char* str = json["str"];
        if (!str) {
            str = ""; // a message without a text is shown as an empty one
        }
        switch (msgType) {
            case MSGTYPE_ONE_BY_ONE: {
                showCharsOneByOneOnBothPanels(str, {json["r"], json["g"], json["b"]}, json["dly"]);
                break;
            }
            case MSGTYPE_CHONKY_SLIDE: {
                showChonkySlideStyleOnBothPanels(str, json["dly"], {json["r"], json["g"], json["b"]}, json["scroll"]);
                break;
            }
            case MSGTYPE_UTILITY: { // Utility messages
                char subtype = json["subtype"].as<const char*>()[0];

                switch (subtype) {
                    case 'M': { // Microphone enable/disable
                        if (*str == 'E') {
                            PCICR |= bit(PCIE1); // enable pin change interrupts for A0 to A5
                            showAsFlashyStyle("MIC ON", 200);
                        } else if (*str == 'D') {
                            PCICR &= ~bit(PCIE1); // disable pin change interrupts for A0 to A5
                            showAsFlashyStyle("MIC OFF", 200);
                        }
                        break;
                    }
                    case 'B': { // Brightness shift
                        // 0 = full brightness; every step halves it; 8 = dark (nothing is left of a
                        // value). The number is brought into that range HERE, once, before it is
                        // stored: everything that sends and everything that estimates what will be
                        // sent reads the stored value. (A larger number used to be stored as it
                        // came: "200" gave a shift the compiler does not define, and in the image
                        // it built, full brightness.) Below 0 counts as 0.
                        uint shift;
                        if (strlen(str) == 0) {
                            shift = BRIGHTNESS_SHIFT;
                        } else {
                            const int asked = atoi(str);
                            shift = asked < 0 ? 0 : asked > BRIGHTNESS_SHIFT_MAX ? BRIGHTNESS_SHIFT_MAX : asked;
                        }
                        BRIGHTNESS_SHIFT = shift;
                        char out[10];
                        sprintf(out, "BRIGHT=%d", shift);
                        showAsFlashyStyle(out, 200);
                        break;
                    }
                }
                break;
            }
            case MSGTYPE_KEYBOARD: { // Keyboard input
                char keyboardInputMode = json["mode"].as<const char*>()[0];

                showAsInputStyle(str, strlen(str) - 1 - 1, keyboardInputMode);
                break;
            }
            case MSGTYPE_FLASHY: {
                showAsFlashyStyle(str);
                showstarfield();
                break;
            }
            case MSGTYPE_COUNTDOWN: {
                showAsCountdownStyle(str);
                showstarfield();
                break;
            }
            case MSGTYPE_STARFIELD: {
//                showAsCountdownStyle(str);
                showstarfieldcustom(json["stars"]);
                break;
            }
            case MSGTYPE_ICON: {
                Color iconColor = {json["r"], json["g"], json["b"]};
                switch (*str) {
                    case '1': {
                        showIconInvaders(&ENEMY1, iconColor);
                        break;
                    }
                    case '2': {
                        showIconInvaders(&ENEMY2, iconColor);
                        break;
                    }
                    case 'X': {
                        showIconInvaders(&EXPLOSION, iconColor);
                        break;
                    }
                    case 'A': {
                        showIconInvaders(&ANJUNA, iconColor);
                        break;
                    }
                    case 'B': {
                        showIconInvaders(&BAAAHS, iconColor);
                        break;
                    }
                    case 'D': {
                        showIconInvaders(&DREAMSTATE, iconColor);
                        break;
                    }
                    case 'E': { //TODO EDC
                        showIconInvaders(&DREAMSTATE, iconColor);
                        break;
                    }
                    default: {
#if LED_LIMITS
                        // The text is put together in a buffer of its own. (The line below writes
                        // the message's text into the memory after a string constant, more of it
                        // with every such message: over the fonts, over the brightness shift. A
                        // limit cannot hold on a sign whose memory is overwritten. It is left as it
                        // is where there are no limits, so that that image stays the one it was.)
                        char invalid[14 + 21 + 1];
                        strcpy_P(invalid, PSTR("INVALID ICON: "));
                        strncat(invalid, str, 21);
                        showAsCountdownStyle(invalid, 100);
#else
                        showAsCountdownStyle(strcat("INVALID ICON: ", str), 100);
#endif
                        break;
                    }
                }
                break;
            }
            case MSGTYPE_TRACKID: {
//                sector = 1; //TODO support color setting
                marquee(str, fontStd, false, MARQUEE_DELAY + 10);
                break;
            }
            case MSGTYPE_CHONKYMARQUEE: {
                marquee(str, fontChonk, true, MARQUEE_DELAY - 6);
                showstarfieldcustom(100);
                break;
            }
            case MSGTYPE_DEFAULT:
            default: {
                marquee(str);
                showstarfieldcustom(100);
                break;
            }
        }
    } else {
        chooserLeft(); // the starfield (or the error flash before it) replaces the chooser picture
        showstarfieldcustom(600);
    }

/*
    // Returns true if we should show the new msg alert
    if (readSerialData()) {
        showAsCountdownStyle();
        showstarfield();
    } else {
        //TODO can move the contents of this else block above the readSerialData call to make new messages
        // more responsive
        if (!showInChooserStyle && !showInInputStyle) { //dont advertise when in chooser mode
            showstarfieldcustom(100);
            if (loopcount % ADVERTISE_EVERY == 0) {
                showMsgMeAd();
                showIconInvaders();
            }
        }
    }
    */

    // TODO: Actually sample the state of the pullup on unused pins and OR it into the mask so we maintain the state.
    // Must do AFTER the cli().
    // TODO: Add offBits also to maintain the pullup state of unused pins.

    loopcount++;
}
