#define DEBUG false

#define uint8_t uint

#include <Arduino.h>

#include "Color.h"

#include "Gamma.h"

#include "Fonts.h"

#include "Icons.h"

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

const char ASOT[] PROGMEM = "                    ";
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
#define INVADER_DELAY 60
//Speed of scrolling text marquee, lower is faster
#define MARQUEE_DELAY 25
//Affects how long the all your base style text stays on screen. Higher is faster
#define ALLYOURBASE_DELAY 12
//Speed of chars spelled out one by one effect, lower is faster
#define CHARS_ONEBYONE_DELAY 50

// Change this to be at least as long as your pixel string (too long will work fine, just be a little slower)
#define NUM_PANELS 2  // Number of panels. There are 2.
#define COLUMNS_PER_PANEL 60  // Number of columns per panel, in my case, the # of pixels in each LED string. I am using 60LED/M
#define PIXELS NUM_PANELS*COLUMNS_PER_PANEL  // Length of pixels total. I am using 2 meters of 60LED/M

#define INTERCHAR_SPACE 1

// The length of the buffer used to read a new string from the serial port
#define MAX_BUFFER_LEN 600
#define STRINGBUFFER_LEN MAX_BUFFER_LEN + 1
#define PADDED_STRINGBUFFER_LEN STD_STRING_PADDING + STRINGBUFFER_LEN

// Pad this amount so that scrolling starts nicely off the end, for FontStd
#define STRING_PADDING(font_width) NUM_PANELS * COLUMNS_PER_PANEL / (font_width + INTERCHAR_SPACE)

const int json_capacity = STRINGBUFFER_LEN + JSON_OBJECT_SIZE(8);
StaticJsonDocument<json_capacity> json;

// This is what style the current text will be shown in
const char MSGTYPE_CHONKY_SLIDE = 'C';
const char MSGTYPE_ONE_BY_ONE = 'O';
const char MSGTYPE_FLASHY = 'F';
const char MSGTYPE_UTILITY = 'U';
const char MSGTYPE_KEYBOARD = 'K';
const char MSGTYPE_CHOOSER = 'H';
const char MSGTYPE_ICON = 'I';
const char MSGTYPE_TRACKID = 'T';
const char MSGTYPE_DEFAULT = 'D';

// Modes to show keyboard mode in
const char KEYBOARD_MODE_WARNING = 'W';

//vertical tab or \v; single column
const char VT = '\u000B';

/* Return how many columns to pad this string by on each side to get it to be in the middle of the panel.
 * E.g.
 * (60-6)/2 = 27
 * (60-1)/2 = 29
 */
uint getColumnsToPadForString(const char* str, int fontWidth) {
    uint stringColumns = 0;
    strlen(str) * (fontWidth + INTERCHAR_SPACE) - 1;
    //Minus 1 because we don't need an interchar space at the end

    while (*str++) {
        if (*str != VT) {
            stringColumns += (fontWidth + INTERCHAR_SPACE);
        }
    }
    return (COLUMNS_PER_PANEL - stringColumns) / 2;
}

// Just wait long enough without sending any bits to cause the pixels to latch and display the last sent frame
void show() {
    delayMicroseconds((RES / 1000UL) +
                      1);       // Round up since the delay must be _at_least_ this long (too short might not work, too long not a problem)
}

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

static void sendChar(uint ch, uint skip, uint r, uint g, uint b) {

    // For the VT char, we only send a single column
    if (ch != VT) {
        const uint* charbase = FontStd5x7 + ((ch - ' ') * FONTSTD_WIDTH);
        uint col = FONTSTD_WIDTH;

        while (skip--) {
            charbase++;
            col--;
        }

        while (col--) {
            sendColumnRGB(pgm_read_byte_near(charbase++), r, g, b);
        }

        // TODO: FLexible interchar spacing
    }
    sendColumnRGB(0, r, g, b);    // Interchar space

}

static void sendString(const char* str, uint skip, const uint r, const uint g, const uint b) {
    unsigned int l = PIXELS / (FONTSTD_WIDTH + INTERCHAR_SPACE);

    sendChar(*str, skip, r, g, b); // First char is special case because it can be stepped for smooth scrolling

    // Send rest of string
    while (*(++str) && l--) {
        sendChar(*str, 0, r, g, b);
    }
}

// Show the passed string. The last letter of the string will be in the rightmost pixels of the display.
// Skip is how many cols of the 1st char to skip for smooth scrolling
static inline void
sendPaddedString(const char* padding, const char* str, uint skip, const uint r, const uint g, const uint b) {

    unsigned int l = PIXELS / (FONTSTD_WIDTH + INTERCHAR_SPACE);

    if (*padding) {
        sendChar(*padding, skip, r, g, b); // First char is special case because it can be stepped for smooth scrolling

        while (*(++padding) && l--) {
            sendChar(*padding, 0, r, g, b);
        }

        sendChar(*str, 0, r, g, b); // First char is special case because it can be stepped for smooth scrolling
    } else {
        sendChar(*str, skip, r, g, b); // First char is special case because it can be stepped for smooth scrolling
    }

    // Send rest of string
    while (*(++str) && l--) {
        sendChar(*str, 0, r, g, b);
    }
}

void sendPaddingForString(const char* str, int fontWidth) {
    int padding = getColumnsToPadForString(str, fontWidth);
    while (padding--) {
        sendColumnRGB(0, 0, 0, 0);
    }
}

static inline void
sendStringJustified(const char* s, int fontWidth, uint skip, const uint r, const uint g, const uint b) {
    sendPaddingForString(s, fontWidth);
    sendString(s, skip, r, g, b);
    sendPaddingForString(s, fontWidth);
}

// Keep track of where we are in the color cycle between chars
uint* cyclingColor = (uint * )(calloc(1, sizeof(uint)));

// Send a char with a column-based color cycle
static inline void sendCharColorCycle(const uint* font, const int fontWidth, uint c, uint* r, uint* g, uint* b) {

    const uint* charbase = font + ((c - ' ') * fontWidth);

    uint col = fontWidth;

    while (col--) {

        sendColumnRGB(pgm_read_byte_near(charbase++), *r, *g, *b);

        *cyclingColor += 10;
    }

    sendColumnRGB(0, 0, 0, 0);
    *cyclingColor += 10;

}

// Show the passed string with the arcade font and a nice vertical color cycle effect
// columnsPrefix: the # of empty columns to add before the string, useful for spacing
static inline void
sendStringColorCycle(const uint* font, const int fontWidth, const char* s, int columnsPrefix, uint* r, uint* g,
                     uint* b) {

    unsigned int l = PIXELS / (fontWidth + INTERCHAR_SPACE);

    while (columnsPrefix--) {
        sendColumnRGB(0, 0x00, 0x00, 0x00);
    }

    while (l--) {

        char c;

        c = *s++;

        if (!c) break;

        sendCharColorCycle(font, fontWidth, c, r, g, b);
    }
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
    softSerial.begin(57600);
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

void showAsChooser(const char* blinkyStr, const char* countyStr) {

//    int maxStrLen = 10; // max number of chars to show //TODO or, could scroll the whole thing
//    blinkyStr[maxStrLen] = 0x00; //chop here


    int blinkyLen = strlen(blinkyStr);
    int shiftBy = constrain(blinkyLen - MAX_CHARS_PER_PANEL - strlen(countyStr), 0, blinkyLen);
    // Shift string from the start if it is longer than the max chars we can show,
    // so that last MAX_CHARS_PER_PANEL chars are always visible (plus _)
//    blinkyStr = blinkyStr + shiftBy;

    unsigned int count = 58;

    clear();
    while (count > 0) {

        count--;

        uint brightness = GAMMA(((count % 100) * 256) / 100);

        cli();

        // County part
        sendString(countyStr, 0, 0x80, 0, 0);

        sendColumnRGB(0x00, 0, 0, 0xff);

        // Blinky part
//        while (*blinkyStr) {
//        for (int s = 0; s <= shiftBy; s++) {
//            if (s == blinkyLen) {
//                break;
//            }
//            for (uint step = 0; step < FONTSTD_WIDTH +
//                                       INTERCHAR_SPACE; step++) {
//                // step though each column of the 1st char for smooth scrolling
//                sendString(blinkyStr, step, brightness, brightness, brightness);
        sendString(blinkyStr, 0, brightness, brightness, brightness);
//            }
//            blinkyStr = blinkyStr + s;
//        }
        sei();
//        delay(MARQUEE_DELAY); // speed. higher = slower

        show();
    }
}

void showcountdown(const char* countdownstr, unsigned int count = 600) {

    // Start sequence.....

//    const char *countdownstr = "NEW MSG IN   ";
//    const char *countdownstr = "NOW PLAYING";
//    const char *countdownstr1 = "NOW";
//    const char *countdownstr2 = "PLAYING";

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
        sendStringJustified(countdownstr, FONTSTD_WIDTH, 0, brightness, brightness, brightness);

        sendColumnRGB(0x00, 0, 0, 0xff);

        //  sendChar( '0' , 0 , 0x80, 0 , 0 );

        sendChar(char1, 0, 0x80, 0, 0);
        sendChar('.', 0, 0x80, 0, 0);
        sendChar(char2, 0, 0x80, 0, 0);
        sendChar(char3, 0, 0x80, 0, 0);

        sei();
        show();
    }

    count = 100;

    // One last farewell blink
    while (count > 0) {

        count--;

        uint brightness = GAMMA(((count % 100) * 256) / 100);

        cli();
        sendStringJustified(countdownstr, FONTSTD_WIDTH, 0, brightness, brightness, brightness);
//        sendColumnRGB(0x00, 0, 0, 0xff);
//        sendString(countdownstr2, 0, brightness, brightness, brightness);

        sendColumnRGB(0x00, 0, 0,
                      0xff);   // We need to quickly send a blank byte just to keep from missing our deadlne.
        sendChar('0', 0, brightness, 0, 0);
        sendChar('.', 0, brightness, 0, 0);
        sendChar('0', 0, brightness, 0, 0);
        sendChar('0', 0, brightness, 0, 0);


        sei();
        show();
    }
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

        // show(); // Not needed - random is alwasy slow enough to trigger a reset

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

void showCharsOneByOneOnBothPanels(const char* str, Color textColor, int delayMs = 500) {
    Color tc = textColor;
    clear();
    for (uint p = 0; p < strlen(str); p++) {
        cli();
        sendPaddingForString(str, FONTSTD_WIDTH);
//        sendStringColorCycle("                ");
//        sendIcon(enemies, which, 0, ENEMIES_WIDTH, r, g, b);
        for (uint i = 0; i <= p; i++) {
            sendChar(*(str + i), 0, GAMMA(tc.r), GAMMA(tc.g), GAMMA(tc.b));
        }
        sendPaddingForString(str, FONTSTD_WIDTH);
        sei();
        delay(CHARS_ONEBYONE_DELAY);
    }
    delay(delayMs);
}

void showStringColorCycleOnBothPanels(const uint* font, const int fontWidth, const char* str, int columnsPrefix,
                                      uint* r, uint* g, uint* b, uint slide) {
    clear();
    for (slide; slide; slide -= 10) {
        *cyclingColor = (slide & 0xff);
        cli();
        sendStringColorCycle(font, fontWidth, str, columnsPrefix, r, g, b);
        sendStringColorCycle(font, fontWidth, str, columnsPrefix + 5, r, g, b);
        sei();
        show();
        delay(ALLYOURBASE_DELAY);
    }

}

void showallyourbasestyleOnBothPanels(const char* str, int columnsPrefix) {
//    const char *allyourbase = "CAT: ALL YOUR BASE ARE BELONG TO US !!!";
    uint g = 0;
    uint b = 0x80;
    showStringColorCycleOnBothPanels(FontChonk, FONTCHONK_WIDTH, str, columnsPrefix, cyclingColor, &g, &b, 1250);
}

void showInstagramAd() {
    showCharsOneByOneOnBothPanels("INSTAGRAM:", Color{0xfe, 0xd4, 0x3b});
    showCharsOneByOneOnBothPanels("@APHEXCX", Color{0xef, 0x0c, 0x1d}, 1000);
//    showCharsOneByOne("TWITTER:  TWITTER:  ", GAMMA(0x1d), GAMMA(0xa1), GAMMA(0xf2));
//    showCharsOneByOneAndWait("  @APHEX    @APHEX ", GAMMA(0x1d), GAMMA(0xa1), GAMMA(0xf2), 1000);
//    showCharsOneByOne("SOUNDCLOUDSOUNDCLOUD", GAMMA(0xff), GAMMA(0x77), GAMMA(0x00));
//    showCharsOneByOneAndWait("DADPARTYSFDADPARTYSF", GAMMA(0xff), GAMMA(0x77), GAMMA(0x00), 1000);
    uint r = GAMMA(0xef);
    uint g = GAMMA(0x0c);
    uint b = GAMMA(0x2d);
//    // Idk why I can't get the slide value to show up for longer, even 30000 is really short, so just call it twice
//    showStringColorCycleOnBothPanels(FontStd5x7, FONTSTD_WIDTH, " @APHEXCX ", 0, cyclingColor, &g, &b, 1250);
    //TODO cycle color without slide
//    showStringColorCycleOnBothPanels(FontStd5x7, FONTSTD_WIDTH, " @APHEXCX ", 0, cyclingColor, &g, &b, 1250);

//    showallyourbasestyle("INSTAGRAM INSTAGRAM ");
//    showallyourbasestyle("@APHEXCX  @APHEXCX ");
}

void showMainAd() {
    showallyourbasestyleOnBothPanels("DAD SF", 4);
    showallyourbasestyleOnBothPanels("DAD SF", 4);
    showallyourbasestyleOnBothPanels("X X X ", 6);
    showallyourbasestyleOnBothPanels("BAAAHS", 4);
    showallyourbasestyleOnBothPanels("BAAAHS", 4);
//    showallyourbasestyleOnBothPanels("TRANCE", 4);
//    showallyourbasestyleOnBothPanels("2019!!", 4);
}

void showTalkToUsAd() {

//    showCharsOneByOne("THANK YOU THANK YOU ", GAMMA(0xfe), GAMMA(0xd4), GAMMA(0x3b));
//    showCharsOneByOne("FOR RIDINGFOR RIDING", GAMMA(0xfe), GAMMA(0xd4), GAMMA(0x3b));
//    showCharsOneByOneAndWait("  BAAAHS    BAAAHS  ", GAMMA(0xea), GAMMA(0x17), GAMMA(0x8c), 1000);

//    showCharsOneByOne("WANT TO...WANT TO...", GAMMA(0x04), GAMMA(0xff), GAMMA(0x19));
//    showCharsOneByOneAndWait("ADD A MSG?ADD A MSG?", GAMMA(0x04), GAMMA(0xff), GAMMA(0x19), 1200);
//    showCharsOneByOneAndWait("ASK US!!<3ASK US!!<3", GAMMA(0xff), GAMMA(0x05), GAMMA(0x5d), 1200);
//    showCharsOneByOneAndWait("CUM ON IN!CUM ON IN!", GAMMA(0xff), GAMMA(0x05), GAMMA(0x5d), 1200);
}

void showMsgMeAd() {
    clear();
    showMainAd();
    showInstagramAd();
    showTalkToUsAd();
//    showCharsOneByOne(" MSG ME!!! MSG ME!!! ", GAMMA(0xE6), GAMMA(0x00), GAMMA(0x7E));

//    showCharsOneByOne(1, " = 10 POINTS", 0x00, 0xff, 0x00);
}

void showIconInvaders(ICON icon) {

    uint acount = PIXELS / (ICON_WIDTH + FONTSTD_WIDTH);      // How many aliens do we have room for?

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
                sendIcon(icons + (icon * ICON_WIDTH), p & 1, row, ICON_WIDTH, GAMMA(0x4f), GAMMA(0x62), GAMMA(0xd2));
//                sendChar(' ', 0, 0x00, 0x00, 0x00); // No over crowding
                sendColumnRGB(0, 0x00, 0x00, 0x00);
                sendColumnRGB(0, 0x00, 0x00, 0x00);
                sendColumnRGB(0, 0x00, 0x00, 0x00);
                sendColumnRGB(0, 0x00, 0x00, 0x00);
                sendColumnRGB(0, 0x00, 0x00, 0x00);
            }

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

void marquee(const char* marqueePtr, bool pad = true, uint marqueeDelay = MARQUEE_DELAY) {
    //TODO change the harcoded padding to be generated by the length of STRING_PADDING
    const char paddingString[STRING_PADDING(FONTSTD_WIDTH) + 1] = "                    ";
    const char* paddingPtr = paddingString;
    if (pad) {
        paddingPtr = paddingString;
    } else {
        paddingPtr += STRING_PADDING(FONTSTD_WIDTH) / 2;
    }

    // Text foreground color cycle effect
    sector++;
    if (sector == 3) {
        sector = 0;
    }

    float beatPct = 0.0f;
    unsigned long delta;

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

        for (uint step = 0; step < FONTSTD_WIDTH + INTERCHAR_SPACE; step++) {
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

            cli();

            sendPaddedString(paddingPtr, marqueePtr, step, r, g, b);

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

}

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

    // Read the JSON document from the serial port
    DeserializationError err = deserializeJson(json, softSerial);

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
        showcountdown((char*) err.c_str());
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

    showcountdown("I", 80);
    showcountdown("LIVE", 80);
    showcountdown("FOR", 80);
    showcountdown("THAT", 80);
    showcountdown("ENERGY!", 80);
    showstarfield();

    setupSerial();

    // A0 pin change interrupt
    digitalWrite(A0, HIGH);  // enable pull-up
    PCMSK1 |= bit (PCINT8);  // want pin A0
    PCIFR |= bit (PCIF1);   // clear any outstanding interrupts
    PCICR |= bit (PCIE1);   // enable pin change interrupts for A0 to A5

    loopcount = 0;
}

void loop() {
    diagnosticLedOn();
    bool readSuccessful = readSerialData();
    diagnosticLedOff();

    if (readSuccessful) {
        const char* str = json["str"];
        const char msgType = json["type"].as<const char*>()[0];
        switch (msgType) {
            case MSGTYPE_ONE_BY_ONE: {
                showCharsOneByOneOnBothPanels(str, {json["r"], json["g"], json["b"]}, json["dly"]);
                break;
            }
            case MSGTYPE_CHONKY_SLIDE: {
                showallyourbasestyleOnBothPanels(str, 4);  //TODO extract color //TODO implement delay
                break;
            }
            case MSGTYPE_CHOOSER: {
                showAsChooser(str, json["flashy"]);
                break;
            }
            case MSGTYPE_UTILITY: { // Utility messages
                char subtype = json["subtype"].as<const char*>()[0];

                switch (subtype) {
                    case 'M': { // Microphone enable/disable
                        if (*str == 'E') {
                            PCICR |= bit (PCIE1); // enable pin change interrupts for A0 to A5
                            showcountdown("MIC ON", 200);
                        } else if (*str == 'D') {
                            PCICR &= ~bit (PCIE1); // disable pin change interrupts for A0 to A5
                            showcountdown("MIC OFF", 200);
                        }
                        break;
                    }
                    case 'B': { // Brightness shift
                        uint shift;
                        if (strlen(str) == 0) {
                            shift = BRIGHTNESS_SHIFT;
                        } else {
                            shift = atoi(str);
                        }
                        BRIGHTNESS_SHIFT = shift;
                        char out[10];
                        sprintf(out, "BRIGHT=%d", shift);
                        showcountdown(out, 200);
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
                showcountdown(str);
                showstarfield();
                break;
            }
            case MSGTYPE_ICON: {
                showIconInvaders(ENEMY1);

//                switch (*str) {
//                    case '1': {
//                        showIconInvaders(ENEMY1);
//                        break;
//                    }
//                    case '2': {
//                        showIconInvaders(ENEMY2);
//                        break;
//                    }
//                    case 'E': {
//                        showIconInvaders(EXPLOSION);
//                        break;
//                    }
////                    case 'A': {
////                        showIconInvaders(ANJUNA);
////                        break;
////                    }
//                    case 'B': {
//                        showIconInvaders(BAAAHS);
//                        break;
//                    }
//                    default: {
//                        showcountdown(strcat("INVALID ICON: ",str), 100);
//                        break;
//                    }
//                }
                break;
            }
            case MSGTYPE_TRACKID: {
                //TODO support a chonky marquee
//                sector = 1; //TODO support color setting
                marquee(str, false, MARQUEE_DELAY + 10);
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
        showcountdown();
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
