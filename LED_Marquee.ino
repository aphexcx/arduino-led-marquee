#define DEBUG false

#define uint8_t uint

#include <Arduino.h>

#include "Color.h"

#include "Gamma.h"

// The marquee's colour wheel and its flash on the beat (the jump game's score and runner use them too)
#include "MarqueeColour.h"

#include "Fonts.h"

#include "Icons.h"

// Scroll geometry of the chooser preview (pure functions, unit-tested on the host: see test/)
#include "ChooserScroll.h"

#include "BrightnessShift.h"

#include "PixelBitBanging.h"

#include "ArduinoJson-v6.18.3.h"

// Used to receive data on a virtual RX pin instead of the usual pin 0
#include "SoftwareSerial/SoftwareSerial.h"
// The line to the app (softSerial below): SoftwareSerial with an end to reading, for the jump game
#include "AppSerial.h"

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

// How many ms ago the last beat was (for beatFade). The beat's interrupt writes the time of the
// beat, four bytes, so it is read with interrupts off, and the clock with it: a beat that comes
// between the two would otherwise be counted from the beat before.
static unsigned long msSinceBeat() {
    const uint8_t sreg = SREG;
    cli();
    const unsigned long since = millis() - timeOfLastBeat;
    SREG = sreg;
    return since;
}

#define rxPin 10
#define txPin 11
AppSerial softSerial(rxPin, txPin); // RX, TX

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
// ---- (Contract VERSION 3: the app pushes changes; the sign asks nothing while a text moves. See ChooserDisplay.h)
// ---- These six values are the ones to tune:
//How long the start of a scrolling message stands still before it moves (and again every time round), in ms
#define CHOOSER_HOLD_MS 600
//Time per scrolled LED column in ms, lower is faster. 35 is about 4.8 characters a second
//(on the sign a step measures 36 to 37 ms: the clock loses a tick or two while a frame is sent)
#define CHOOSER_SCROLL_DELAY_MS 35
//Blank columns between the end of the message and its start coming round again. 18 is three spaces. 1 to 240
#define CHOOSER_GAP_COLS 18
//Brightness of scrolling text, 0-255 before gamma correction. 145 is where the fade blink of a short message starts
#define CHOOSER_TEXT_BRIGHTNESS 145
//A message that fits beside the count blinks: it fades to black in a tenth of a second and then stays dark for
//this many ms before it starts again. (Until now the dark time was however long the app took to answer the read
//after every blink, about this long on the sign.) Lower blinks faster. 0 to 60000
#define CHOOSER_BLINK_PAUSE_MS 200
//A message that fits is read again from the app every this many ms, in case a push was lost; the read is done in
//the dark pause of the blink. A scrolling message is read again each time it has come round instead, at its
//stand-still. Lower corrects a wrong short preview sooner (and is how fast the sign follows an app that does not
//push yet). 1000 to 30000. (These are ms of the sign's own clock, which loses time while frames are sent one
//after the other: 8000 measures about 9.7 s.)
#define CHOOSER_REFRESH_MS 8000
// ---- The rest rarely needs touching (the first three are the contract's numbers on the sign's side):
//After the receive line was seen LOW (the app's "attention"), wait at most this long for it to be HIGH again, in ms
#define CHOOSER_ATTN_HIGH_WAIT_MS 100
//... then at most this long for the first byte of the message the app sends unasked (contract: ATTN_FIRST_BYTE_MS).
//If none comes the sign asks with '~'. That is how the chooser ENDS (contract 3.1: on leaving chooser mode the app
//gives attention and sends nothing), so this is also the pause before the next message after "leave chooser".
//On the sign the app's first byte was measured 26 to 38 ms after the end of its break
#define CHOOSER_ATTN_FIRST_BYTE_MS 150
//... and this long for each byte after it. The app may pause inside a message (its sending thread is held up
//for 40 to 80 ms a few times a minute), so this is longer than that
#define CHOOSER_ATTN_BYTE_MS 100
//The line counts as HIGH again once it has been HIGH for this long without a gap, in ms (one byte is 0.7 ms)
#define CHOOSER_LINE_QUIET_MS 3
//A LOW that lasts this long (microseconds) is the app raising its hand: a break, or the first of the 0x00 bytes it
//may send instead of one (nine bit times LOW, 625 us at 14400 baud). No other byte holds the line LOW that long:
//the longest is 0x80 with eight bit times, 556 us, and no character of ASCII text has more than seven
#define CHOOSER_BREAK_MIN_US 590UL
//After a message has been read, wait at most this long for whatever the app writes straight behind it (a newline)
//to end
#define CHOOSER_TRAILER_WAIT_MS 100
//What the app writes behind a message may also come late (it may pause before it as before any byte). Bytes that
//come late and are over within this many ms are that, and not the app raising its hand: room for fourteen bytes
//back to back. Bytes that go on for longer are a message going by (the shortest message is 25 bytes, 17 ms, of
//which the sign may miss the first 5 while it sends a frame)
#define CHOOSER_TRAILER_LATE_MS 10
//Longer than the line is ever HIGH between two bytes sent back to back (nine bit times, 0.63 ms), in ms. For this
//long the sign watches the line with its receiver already on before it sends a request (the last part of
//REQUEST_QUIET_MS); and the line must be HIGH this long before it asks again after a read that ended because a
//byte did not come in its time
#define REQUEST_GAP_MS 1
//Before any '~' is sent (in every mode), the line from the app must have been quiet for this long, in ms, or
//the answer would be read starting in the middle of whatever the app is sending. (The app wants about 10 ms of
//silence between a message it pushed and the next request.) Every request in every mode is this much later
#define REQUEST_QUIET_MS 10
//A line that stays LOW without a gap for this long (ms) is dead (nothing drives it, or it is held): the request
//is not sent and counts as unanswered
#define REQUEST_QUIET_WAIT_MS 500
//... and this is how long (ms) the app may go on sending before the sign gives up waiting for it to finish.
//The longest message, 600 characters all written as \uXXXX escapes, is 2.5 s on the wire
#define REQUEST_BUSY_WAIT_MS 4000
//How long any '~' read waits for the first byte of its answer, in ms (the same as for every later byte)
#define ANSWER_FIRST_BYTE_MS 1000
//... and how much longer than that it may wait, once, when the app raised its hand late in that wait
#define ANSWER_AFTER_BREAK_MS (CHOOSER_ATTN_HIGH_WAIT_MS + CHOOSER_ATTN_FIRST_BYTE_MS)
//After this many reads in a row that brought nothing usable, go back to loop() and its normal '~'
#define CHOOSER_MAX_FAILED_READS 3
//1 = show on panel B what was measured at the last attention (the Uno cannot print), and send the same numbers
//to the app as a line of text in front of the next '~' (see ChooserDisplay.h). For the bench, not for a show:
//ms to the first byte (green), ms to the end of the message (blue), attentions (white), fallbacks to '~' (red)
#ifndef CHOOSER_DEBUG_PANEL_B
#define CHOOSER_DEBUG_PANEL_B 0
#endif

// ---- Ads (MSGTYPE_ONE_BY_ONE, MSGTYPE_CHONKY_SLIDE): a text wider than a panel scrolls to its end by itself.
// ---- Their "dly" is real milliseconds. These three values are the ones to tune (see AdDisplay.h):
//How long the start of a too-wide chonky text stands still before it moves, in ms
#define AD_SCROLL_HOLD_MS 600
//Time per scrolled LED column in ms, lower is faster. 35 is the chooser's pace, about 4.8 one-by-one letters a second
#define AD_SCROLL_STEP_MS 35
//Time per frame of the chonky colour wave in ms. 10 is 100 frames a second, what a dly of 1000 always gave
#define AD_WAVE_FRAME_MS 10

// ---- The jump game (MSGTYPE_GAME): the app runs it, the sign draws a frame and asks again (see GameDisplay.h).
//How long the sign waits for an answer to begin (and for each further byte of it), in ms
#define GAME_MISS_MS 150
//After an answer that is not a good frame: how long the line must be silent before the sign asks again, in ms
#define GAME_QUIET_MS 20
//Without a good frame for this long the game is over for the sign and it asks the ordinary way again, in ms
#define GAME_LEAVE_MS 1000

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
const char MSGTYPE_GAME = 'G';

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
static_assert(CHOOSER_HOLD_MS >= 1 && CHOOSER_HOLD_MS <= 60000 && CHOOSER_SCROLL_DELAY_MS >= 1 &&
              CHOOSER_SCROLL_DELAY_MS <= 1000 && CHOOSER_BLINK_PAUSE_MS >= 0 && CHOOSER_BLINK_PAUSE_MS <= 60000,
              "the chooser's waits are counted in microseconds in 32 bits");
static_assert(ANSWER_FIRST_BYTE_MS + REQUEST_BUSY_WAIT_MS <= 60000 && CHOOSER_ATTN_BYTE_MS <= ANSWER_FIRST_BYTE_MS,
              "the time a byte is given is counted in ms in 16 bits");
static_assert(CHOOSER_TRAILER_LATE_MS >= 1 && CHOOSER_TRAILER_LATE_MS <= 12,
              "what comes late behind a message must stay shorter than the shortest message less a frame (17 - 5 ms)");
static_assert(REQUEST_GAP_MS >= 1 && REQUEST_GAP_MS < REQUEST_QUIET_MS,
              "the gap must be longer than the line is HIGH inside a run of bytes (0.63 ms at most)");
static_assert(CHOOSER_LINE_QUIET_MS >= 1 && CHOOSER_LINE_QUIET_MS < CHOOSER_ATTN_HIGH_WAIT_MS &&
              CHOOSER_LINE_QUIET_MS < CHOOSER_TRAILER_WAIT_MS,
              "the line must be seen HIGH for longer than it is inside a run of bytes (0.63 ms at most)");

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
uint BRIGHTNESS_SHIFT = 0;

// Send 3 bytes of color data (R,G,B) for a signle pixel down all the connected stringsat the same time
// A 1 bit in "row" means send the color, a 0 bit means send black.
static inline void sendColumnRGB(uint row, uint r, uint g, uint b) {

    sendBitx8(row, g >> BRIGHTNESS_SHIFT, onBits);    // WS2812 takes colors in GRB order
    sendBitx8(row, r >> BRIGHTNESS_SHIFT, onBits);    // WS2812 takes colors in GRB order
    sendBitx8(row, b >> BRIGHTNESS_SHIFT, onBits);    // WS2812 takes colors in GRB order

}

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
            sendColumnRGB(pgm_read_byte_near(charbase++), r, g, b);
        }

        // TODO: FLexible interchar spacing
    }
    if (sendTrailingSpace) {
        sendColumnRGB(0, r, g, b);    // Interchar space
    }

}

static void sendString(const char* str, const Font& font, uint skip, const uint r, const uint g, const uint b) {
    unsigned int l = PIXELS / (font.width + INTERCHAR_SPACE);

    sendChar(*str, skip, r, g, b, true, font); // First char is special case because it can be stepped for smooth scrolling

    if (!*str) {
        return; // an empty string: that was its end (as a blank), and the loop below would start behind it
    }

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
        sendColumnRGB(0, 0, 0, 0);
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

// TODO idxToBlink that isn't always last char
void showAsInputStyle(const char* str, int idxToBlink, const char mode) {

//    str = str + STRING_PADDING(FONTSTD_WIDTH) + 1; // drop initial padding

    int length = strlen(str);
    int shiftBy = constrain(length - MAX_CHARS_PER_PANEL, 0, length);
    // Shift string from the start if it is longer than the max chars we can show,
    // so that last MAX_CHARS_PER_PANEL chars are always visible (plus _)
    str = str + shiftBy;
    // ...and only those are drawn. (The whole line's length used to be drawn from the shifted start:
    // the ten characters, then whatever follows the line in memory, and a line of 256 or more never
    // ended, because the loop below counts in 8 bits.)
    length -= shiftBy;

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

    while (count > 0) {
        count--;
        uint brightness = ((count % 100) * 256) / 100;

        cli();
        // Send string, except for last char (which is '_')
        for (uint i = 0; i < length; i++) {
            int r = 0xff;
            int g = 0xff;
            int b = 0xff;
            // Blink idxToBlink char, or all chars if we're in input warning mode
            if (i == idxToBlink || mode == KEYBOARD_MODE_WARNING) {
                sendChar(str[i], 0, 0xff, brightness, brightness);
            } else {
                sendChar(str[i], 0, GAMMA(r), GAMMA(g), GAMMA(b));
            }
        }

        sendColumnRGB(0x00, 0, 0, 0xff);

        sei();
        show();
    }
}

void showAsCountdownStyle(const char* countdownstr, unsigned int count = 600, boolean showCount = true) {
    uint startPad, endPad; // for the string in the middle of a panel (when the count is not shown)
    getColumnsToPadForString(countdownstr, fontStd, &startPad, &endPad);

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

        cli();
        if (showCount) {
            sendString(countdownstr, fontStd, 0, brightness, brightness, brightness);
        } else {
            // align string to middle if we're not showing the countdown.
            sendStringJustified(countdownstr, fontStd, 0, brightness, brightness, brightness, startPad, endPad);
        }

        //  sendChar( '0' , 0 , 0x80, 0 , 0 );

        if (showCount) {
//            sendColumnRGB(0x00, 0, 0, 0xff);
            //Panel A:
            sendChar(char1, 0, 0x80, 0, 0);
            sendChar('.', 0, 0x80, 0, 0);
            sendChar(char2, 0, 0x80, 0, 0);
            sendChar(char3, 0, 0x80, 0, 0, false);

            //Other panel:
            sendString(countdownstr, fontStd, 0, brightness, brightness, brightness);
            sendChar(char1, 0, 0x80, 0, 0);
            sendChar('.', 0, 0x80, 0, 0);
            sendChar(char2, 0, 0x80, 0, 0);
            sendChar(char3, 0, 0x80, 0, 0, false);

        } else {
            // show the string on the other panel too if we're not showing the count
            sendStringJustified(countdownstr, fontStd, 0, brightness, brightness, brightness, startPad, endPad);
        }

        sei();
        delayMicroseconds(COUNTDOWN_DELAY);
        show();
    }

    count = 100;

    // One last farewell blink
    while (count > 0) {

        count--;

        uint brightness = GAMMA(((count % 100) * 256) / 100);

        cli();
        if (showCount) {
            sendString(countdownstr, fontStd, 0, brightness, brightness, brightness);
        } else {
            // align string to middle if we're not showing the countdown.
            sendStringJustified(countdownstr, fontStd, 0, brightness, brightness, brightness, startPad, endPad);
        }

//        sendColumnRGB(0x00, 0, 0, 0xff);

        if (showCount) {
//            sendColumnRGB(0x00, 0, 0,
//                          0xff);   // We need to quickly send a blank byte just to keep from missing our deadlne.

            sendChar('0', 0, brightness, 0, 0);
            sendChar('.', 0, brightness, 0, 0);
            sendChar('0', 0, brightness, 0, 0);
            sendChar('0', 0, brightness, 0, 0, false);

            //Other panel:
            sendString(countdownstr, fontStd, 0, brightness, brightness, brightness);
            sendChar('0', 0, brightness, 0, 0);
            sendChar('.', 0, brightness, 0, 0);
            sendChar('0', 0, brightness, 0, 0);
            sendChar('0', 0, brightness, 0, 0, false);
        } else {
            // show the string on the other panel too if we're not showing the count
            sendStringJustified(countdownstr, fontStd, 0, brightness, brightness, brightness, startPad, endPad);
        }

        sei();
        delayMicroseconds(COUNTDOWN_DELAY);
        show();
    }
}

void showAsFlashyStyle(const char* countdownstr, unsigned int time = 200) {
    showAsCountdownStyle(countdownstr, time, false);
}

void showstarfieldcustom(int stars) {
    if (stars < 0) {
        stars = 0; // (the loop below counts unsigned: -1 stars were 65535 of them, minutes of nothing else)
    }
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
            sendColumnRGB(col << shiftabs, r, g, b);
        }
    } else {
        while (width--) {
            sendColumnRGB((pgm_read_byte_near(charbase++) >> shift) & onBits, r, g, b);
        }
    }

}

// The ads' two styles: showCharsOneByOneOnBothPanels() and showChonkySlideStyleOnBothPanels(), with
// the clock that makes their "dly" real milliseconds and the scrolling of a text wider than a panel.
// They are in their own file so that the host test can run them against a fake sign and a fake timer.
#include "AdDisplay.h"

void showIconInvaders(const Icon* icon, Color iconColor) {
    Color ic = iconColor;
    int iconWidth = pgm_read_byte_near(icon);
//    char blah[4];
//    itoa(iconWidth, blah, 10);
//    showAsFlashyStyle(blah);

    uint acount = PIXELS / (iconWidth + FONTSTD_WIDTH);      // How many aliens do we have room for?

    // A row of icons stops 1 to 17 columns short of the end of the strip (by the icon's width and
    // where the row stands), and what a frame does not reach keeps what the strip showed before:
    // the end of the text before the icons stayed lit beside them. So the sign is cleared first.
    clear();

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

            // Start with margin
            uint margin = p;

            while (margin--) {
                sendColumnRGB(0, 0x00, 0x00, 0x00);
            }

            for (uint l = 0; l < acount; l++) {
                sendIcon(icon->data,
                         p & 1,
                         row,
                         iconWidth,
                         GAMMA(ic.r), GAMMA(ic.g), GAMMA(ic.b));
//                sendChar(' ', 0, 0x00, 0x00, 0x00); // No over crowding
                sendEmptyColumns(4);
            }

            sei();

            delay(INVADER_DELAY);
        }
    }
    // delay(200);
}

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
    if (sector == MARQUEE_WHEEL_SECTORS) {
        sector = 0;
    }

    uint charWidth;

    while (*marqueePtr) {

        if (colorStep == JAB_STEPS) {
            colorStep = 0;
            sector++;
            if (sector == MARQUEE_WHEEL_SECTORS) {
                sector = 0;
            }
        } else {
            colorStep++;
        }

        const Color c0 = marqueeWheel(sector, colorStep);

        if (!*paddingPtr && *marqueePtr == HEART) {
            charWidth = font.heartWidth;
        } else {
            charWidth = font.width;
        }

        for (uint step = 0; step < charWidth + INTERCHAR_SPACE; step++) {
            // step though each column of the 1st char for smooth scrolling

            // Flash on beats: white at the beat, fading back to the wheel's colour
            const unsigned long sinceBeat = msSinceBeat();
            const uint r = beatFade(c0.r, sinceBeat);
            const uint g = beatFade(c0.g, sinceBeat);
            const uint b = beatFade(c0.b, sinceBeat);

            cli();

            sendPaddedString(paddingPtr, marqueePtr, font, step, r, g, b);

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

// The level of the serial line from the app, read straight from the pin (SoftwareSerial is not involved and need
// not be listening). The line idles HIGH. The app holds it LOW to say "I have a new chooser message for you".
static inline bool chooserLineLow() {
    return digitalRead(rxPin) == LOW;
}

// Sends one byte to the app and says whether the line FROM the app changed while it went out. SoftwareSerial
// sends the start bit and the eight data bits with interrupts off, so its receiver cannot take a byte that begins
// in that time. That something began is known, not guessed: the receive pin's change flag, which the pin sets with
// interrupts off too (PB2 is the only pin of its group that is watched, and only while the sign listens).
// THE SIGN LISTENS FROM THE MOMENT ITS OWN BYTE CAN BE ANSWERED. With the last data bit the byte is in the app's
// hands: a quick UART looks at the stop bit half way and may begin its answer in the 35 us that are left of it.
// So the flag is last looked at where the stop bit begins, and interrupts are back on from there: the bundled
// SoftwareSerial's write() does both (see rxChangedDuringWrite() there). What begins in the stop bit or later is
// received like any byte and is not reported.
// AND IT LOSES ONE BYTE, NOT WHAT FOLLOWS IT. write() looks at the flag after every bit it sends, so it knows in
// which bit time the app's byte began and when it is over. It keeps the receiver off until then (it returns up
// to eight bit times, 0.56 ms, later for that) and switches it on between that byte and the next: whatever the
// app sends behind the byte that crossed, back to back or later, is received whole. (If what crossed was no byte
// but noise, the receiver is off for that long all the same.) What the sign does about a crossing is in
// chooserRequest().
static bool chooserSendByte(char c) {
    softSerial.write(c);
    return softSerial.rxChangedDuringWrite();
}

// Sends a request byte, UNLESS A BYTE OF THE APP'S HAS BEEN RECEIVED AND NOT READ: then nothing is sent
// (SEND_HELD). The sign must not ask with something of the app's already heard: what it hears after its request
// would then be the middle of that, and it could not tell where the answer begins. Looking at the receive buffer
// first is not enough: a byte that begins after that look is received by the interrupt, which holds the program
// for the length of the byte and then lets it go on to send. So the look is made inside the bundled
// SoftwareSerial, with interrupts off, right in front of the start bit (writeUnlessReceived() there).
#define SEND_SENT 0    // the byte went out, and nothing of the app's began meanwhile
#define SEND_CROSSED 1 // it went out, and something of the app's began while it did (see chooserSendByte)
#define SEND_HELD 2    // it was not sent: a byte of the app's is in the receive buffer
static uint chooserSendRequest(char c) {
    if (!softSerial.writeUnlessReceived(c)) {
        return SEND_HELD;
    }
    return softSerial.rxChangedDuringWrite() ? SEND_CROSSED : SEND_SENT;
}

// The chooser display (MSGTYPE_CHOOSER): showAsChooser() with its scrolling and its watch on the receive line,
// and the two decisions readSerialData() and loop() make about a reply (chooserReadMessage, chooserDispatch).
// It is in its own file so that the host test can run it against a fake sign and a fake app.
#include "ChooserDisplay.h"

// The jump game (MSGTYPE_GAME): showAsGame() draws the frame it is given and asks for the next,
// and what it does with an answer that is not a good frame. The frame itself (the text of the
// message and the picture it makes) is GameFrame.h, which the host test runs.
#include "GameDisplay.h"

// Notifies the IMX that we're ready to retrieve custom message data
bool readSerialData() {
    // Flushing Serial input buffer
//            while (softSerial.available())
//                softSerial.read();

//            while (isspace(Serial.peek())) {
//                Serial.read();
//            }

    // send special symbol so IMX knows to respond with custom message data
    // (once the line is quiet: not into a message the app is pushing)
//    int firstByte = softSerial.read();
//
//    switch (firstByte) {
//
//    }

    // ... and read the JSON document from the serial port (stepping over what is not a message: the
    // 0x00 a break is received as, white space, an object without a "type"; and letting by, without
    // an error, the rest of a message the app sent by itself while the sign could not listen)
    DeserializationError err = chooserExchange();

    if (err == DeserializationError::Ok) {
        stopSerial();
        diagnosticBlink();
        return true;
    } else if (err == DeserializationError::EmptyInput) {
        // Stop listening here too. The stars that follow are drawn with interrupts off: a message
        // that came in meanwhile (the app pushes without being asked now) would be received in
        // pieces and make the next read fail with an error on the sign.
        stopSerial();
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
        // A chooser message is shown in there, for as long as the chooser is on screen (the app
        // pushes what changes; the receive line is watched between frames); what comes back is
        // the type of the message that ended it.
        char msgType = chooserDispatch();
        // A game frame is shown in there too, for as long as the app answers with game frames.
        // What ended the game is then in `json` (a chooser message is shown in turn); nothing is,
        // when the app's frames stopped coming: ask again with a normal '~'.
        while (msgType == MSGTYPE_GAME) {
            msgType = showAsGame() ? chooserDispatch() : CHOOSER_DISPATCH_NOTHING;
        }
        // Two answers mean there is no message to show: what was read is not one (it has no "type":
        // draw nothing and ask again), or the chooser ended because the app stopped answering.
        if (msgType == CHOOSER_DISPATCH_NOTHING) {
            loopcount++;
            return;
        }
        if (msgType == CHOOSER_DISPATCH_NO_ANSWER) {
            showstarfieldcustom(600); // as after any request that was not answered
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
                        // stored. (It used to be stored as it came, the low byte of what atoi()
                        // made of it: "200" gave a shift the compiler does not define, and in the
                        // image it built, full brightness; "256" was 0.) More than 8 is 8 however
                        // many digits the number has; below 0, and a text that is no number, is 0.
                        uint shift;
                        if (strlen(str) == 0) {
                            shift = BRIGHTNESS_SHIFT;
                        } else {
                            shift = brightnessShiftFromText(str, BRIGHTNESS_SHIFT_MAX);
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
                        // The text is put together in a buffer of its own. (strcat() onto the string
                        // constant wrote the icon's name into the memory behind the constant, and
                        // left the constant longer for the next such message.) 21 characters of the
                        // name are more than the sign shows of it.
                        char invalid[36];
                        strcpy_P(invalid, PSTR("INVALID ICON: "));
                        strncat(invalid, str, 21);
                        showAsCountdownStyle(invalid, 100);
                        break;
                    }
                }
                break;
            }
            case MSGTYPE_TRACKID: {
//                sector = 1; //TODO support color setting
                // A track id starts half way across, and its frames end with its text: one of nine
                // characters or fewer never reaches the right-hand end of side B, and what was
                // there stayed lit beside it, and after it (no starfield follows). So the sign is
                // cleared first. (The other marquees begin with frames longer than the strip.)
                clear();
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
