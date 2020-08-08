#define DEBUG false

#define uint8_t uint

#include <Arduino.h>

#include "Color.h"

#include "Fonts.h"

// Pin 13 has an LED connected on most Arduino boards.
// give it a name:
#define diagnosticLed 13

void diagnosticLedOn() {
    digitalWrite(diagnosticLed, HIGH);   // turn the LED on (HIGH is the voltage level)
}

void diagnosticLedOff() {
    digitalWrite(diagnosticLed, LOW);   // turn the LED on (HIGH is the voltage level)
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

#include "ArduinoJson-v6.16.0.h"

// Used to receive data on a virtual RX pin instead of the usual pin 0
#include "SoftwareSerial/SoftwareSerial.h"

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
#define INVADER_DELAY 50
//Speed of scrolling text marquee, lower is faster
#define MARQUEE_DELAY 25
//Affects how long the all your base style text stays on screen. Higher is faster
#define ALLYOURBASE_DELAY 12
//Speed of chars spelled out one by one effect, lower is faster
#define CHARS_ONEBYONE_DELAY 50

// The char indicating we should show the text as a new message alert (currently BEL, \x07)
#define BEL 7

// Start of header. 1 char follows, defining what style to show the text in
// Current options:
// 'C' for chonky font with a color slide
// 'O' for one-by-one characters
// 'F' flashing announcement with countdown
// 'D' default (marquee scroll with a slow color cycle)
#define SOH 1

// Color code. 3 bytes follow: r, g, b int values, then STX for start of text.
#define ACK 6

// Start of text. String starts here
#define STX 2

// Unused
#define ETX '\x03'

// The char indicating we should show this string in flashing input style
#define EOT 4
// The char indicating we should show this string in input style
#define ENQ 5

// Device control 1 (used for chooser mode)
#define DC1 17
// The char indicating the end of extra data passed in after a control char
#define DLE 10

// Device control 4; used for enabling/disabling the beat detector mic (connected to A0 pin change interrupt)
#define DC4 20
#define VT 11

// Change this to be at least as long as your pixel string (too long will work fine, just be a little slower)
#define NUM_PANELS 2  // Number of panels. There are 2.
#define COLUMNS_PER_PANEL 60  // Number of columns per panel, in my case, the # of pixels in each LED string. I am using 60LED/M
#define PIXELS NUM_PANELS*COLUMNS_PER_PANEL  // Length of pixels total. I am using 2 meters of 60LED/M

#define INTERCHAR_SPACE 1

// The length of the buffer used to read a new string from the serial port
#define MAX_BUFFER_LEN 600
#define STRINGBUFFER_LEN MAX_BUFFER_LEN + 1
#define PADDED_STRINGBUFFER_LEN STD_STRING_PADDING + STRINGBUFFER_LEN

#define JSON_OVERHEAD 200
#define JSON_SIZE STRINGBUFFER_LEN + JSON_OVERHEAD

// Pad this amount so that scrolling starts nicely off the end, for FontStd
#define STRING_PADDING(font_width) NUM_PANELS * COLUMNS_PER_PANEL / (font_width + INTERCHAR_SPACE)

// Leave room for a safety null terminator at the end.
//char bufferA[STRINGBUFFER_LEN + 1] = " ";

//                                     "The jaws that bite, the claws that catch! "
//                                     "Beware the Jubjub bird, and shun "
//                                     "The frumious Bandersnatch! ";
//char bufferB[STRINGBUFFER_LEN + 1] = "                     "; //note extra +1 char here (out of a abundance of caution)

//char *currentBuffer = bufferA;
char currentBuffer[STRINGBUFFER_LEN] = {'\0'};
//char jsonBuffer[JSON_SIZE] = {'\0'};

char* extraDataBuffer = strdup(currentBuffer + STRING_PADDING(FONTSTD_WIDTH));

// Modes to show keyboard mode in
char keyboardInputMode = NULL;
#define KEYBOARD_MODE_WARNING 'W'

// This is what style the current text will be shown in
const char MSGTYPE_CHONKY_SLIDE = 'C';
const char MSGTYPE_ONE_BY_ONE = 'O';
const char MSGTYPE_FLASHY = 'F';
const char MSGTYPE_UTILITY = 'U';
const char MSGTYPE_KEYBOARD = 'K';
const char MSGTYPE_CHOOSER = 'H';
const char MSGTYPE_ICON = 'I';
const char MSGTYPE_DEFAULT = 'D';

char msgType = MSGTYPE_DEFAULT;

Color textColor = {0, 0, 0};
unsigned short textDelay = 500;


/* Return how many columns to pad this string by on each side to get it to be in the middle of the panel.
 * E.g.
 * (60-6)/2 = 27
 * (60-1)/2 = 29
 */
uint getColumnsToPadForString(const char* str, int fontWidth) {
    uint stringColumns = strlen(str) * (fontWidth + INTERCHAR_SPACE) -
                         1; //Minus 1 because we don't need an interchar space at the end
    return (COLUMNS_PER_PANEL - stringColumns) / 2;
}

// These values depend on which pins your 8 strings are connected to and what board you are using
// More info on how to find these at http://www.arduino.cc/en/Reference/PortManipulation

// PORTD controls Digital Pins 0-7 on the Uno

// You'll need to look up the port/bit combination for other boards.

// Note that you could also include the DigitalWriteFast header file to not need to to this lookup.

#define PIXEL_PORT  PORTD  // Port of the pin the pixels are connected to
#define PIXEL_DDR   DDRD   // Port of the pin the pixels are connected to


static const uint onBits = 0b11111110;   // Bit pattern to write to port to turn on all pins connected to LED strips.
// If you do not want to use all 8 pins, you can mask off the ones you don't want
// Note that these will still get 0 written to them when we send pixels
// TODO: If we have time, we could even add a variable that will and/or into the bits before writing to the port to support any combination of bits/values

// These are the timing constraints taken mostly from
// imperically measuring the output from the Adafruit library strandtest program

// Note that some of these defined values are for refernce only - the actual timing is determinted by the hard code.

#define T1H  814    // Width of a 1 bit in ns - 13 cycles
#define T1L  438    // Width of a 1 bit in ns -  7 cycles

#define T0H  312    // Width of a 0 bit in ns -  5 cycles
#define T0L  936    // Width of a 0 bit in ns - 15 cycles

// Phase #1 - Always 1  - 5 cycles
// Phase #2 - Data part - 8 cycles
// Phase #3 - Always 0  - 7 cycles

#define RES 500000   // Width of the low gap between bits to cause a frame to latch

// Here are some convience defines for using nanoseconds specs to generate actual CPU delays

#define NS_PER_SEC (1000000000L)          // Note that this has to be SIGNED since we want to be able to check for negative values of derivatives

#define CYCLES_PER_SEC (F_CPU)

#define NS_PER_CYCLE ( NS_PER_SEC / CYCLES_PER_SEC )

#define NS_TO_CYCLES(n) ( (n) / NS_PER_CYCLE )


// Sends a full 8 bits down all the pins, representing a single color of 1 pixel
// We walk though the 8 bits in colorbyte one at a time. If the bit is 1 then we send the 8 bits of row out. Otherwise we send 0.
// We send onBits at the first phase of the signal generation. We could just send 0xff, but that mught enable pull-ups on pins that we are not using.

/// Unforntunately we have to drop to ASM for this so we can interleave the computaions durring the delays, otherwise things get too slow.

// OnBits is the mask of which bits are connected to strips. We pass it on so that we
// do not turn on unused pins becuase this would enable the pullup. Also, hopefully passing this
// will cause the compiler to allocate a Register for it and avoid a reload every pass.
static inline void sendBitx8(const uint row, const uint colorbyte, const uint onBits) {

    asm volatile (


    "L_%=: \n\r"

    "out %[port], %[onBits] \n\t"                 // (1 cycles) - send either T0H or the first part of T1H. Onbits is a mask of which bits have strings attached.

    // Next determine if we are going to be sending 1s or 0s based on the current bit in the color....

    "mov r0, %[bitwalker] \n\t"                   // (1 cycles)
    "and r0, %[colorbyte] \n\t"                   // (1 cycles)  - is the current bit in the color byte set?
    "breq OFF_%= \n\t"                            // (1 cycles) - bit in color is 0, then send full zero row (takes 2 cycles if branch taken, count the extra 1 on the target line)

    // If we get here, then we want to send a 1 for every row that has an ON dot...
    "nop \n\t  "                                  // (1 cycles)
    "out %[port], %[row]   \n\t"                  // (1 cycles) - set the output bits to [row] This is phase for T0H-T1H.
    // ==========
    // (5 cycles) - T0H (Phase #1)


    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t "                                   // (1 cycles)

    "out %[port], __zero_reg__ \n\t"              // (1 cycles) - set the output bits to 0x00 based on the bit in colorbyte. This is phase for T0H-T1H
    // ==========
    // (8 cycles) - Phase #2

    "ror %[bitwalker] \n\t"                      // (1 cycles) - get ready for next pass. On last pass, the bit will end up in C flag

    "brcs DONE_%= \n\t"                          // (1 cycles) Exit if carry bit is set as a result of us walking all 8 bits. We assume that the process around us will tak long enough to cover the phase 3 delay

    "nop \n\t \n\t "                             // (1 cycles) - When added to the 5 cycles in S:, we gte the 7 cycles of T1L

    "jmp L_%= \n\t"                              // (3 cycles)
    // (1 cycles) - The OUT on the next pass of the loop
    // ==========
    // (7 cycles) - T1L


    "OFF_%=: \n\r"                                // (1 cycles)    Note that we land here becuase of breq, which takes takes 2 cycles

    "out %[port], __zero_reg__ \n\t"              // (1 cycles) - set the output bits to 0x00 based on the bit in colorbyte. This is phase for T0H-T1H
    // ==========
    // (5 cycles) - T0H

    "ror %[bitwalker] \n\t"                      // (1 cycles) - get ready for next pass. On last pass, the bit will end up in C flag

    "brcs DONE_%= \n\t"                          // (1 cycles) Exit if carry bit is set as a result of us walking all 8 bits. We assume that the process around us will tak long enough to cover the phase 3 delay

    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t "                                   // (1 cycles)

    "jmp L_%= \n\t"                               // (3 cycles)
    // (1 cycles) - The OUT on the next pass of the loop
    // ==========
    //(15 cycles) - T0L


    "DONE_%=: \n\t"

    // Don't need an explicit delay here since the overhead that follows will always be long enough

    ::
    [port]    "I"(_SFR_IO_ADDR(PIXEL_PORT)),
    [row]   "d"(row),
    [onBits]   "d"(onBits),
    [colorbyte]   "d"(colorbyte),     // Phase 2 of the signal where the actual data bits show up.
    [bitwalker] "r"(
            0x80)                      // Alocate a register to hold a bit that we will walk down though the color byte

    );

    // Note that the inter-bit gap can be as long as you want as long as it doesn't exceed the reset timeout (which is A long time)

}


// Just wait long enough without sending any bits to cause the pixels to latch and display the last sent frame
void show() {
    delayMicroseconds((RES / 1000UL) +
                      1);       // Round up since the delay must be _at_least_ this long (too short might not work, too long not a problem)
}


// Send 3 bytes of color data (R,G,B) for a signle pixel down all the connected stringsat the same time
// A 1 bit in "row" means send the color, a 0 bit means send black.
static inline void sendColumnRGB(uint row, uint r, uint g, uint b) {

    sendBitx8(row, g, onBits);    // WS2812 takes colors in GRB order
    sendBitx8(row, r, onBits);    // WS2812 takes colors in GRB order
    sendBitx8(row, b, onBits);    // WS2812 takes colors in GRB order

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

static inline void sendStringJustified(char* s, int fontWidth, uint skip, const uint r, const uint g, const uint b) {
    sendPaddingForString(s, fontWidth);
    sendString(s, skip, r, g, b);
    sendPaddingForString(s, fontWidth);
}

// Keep track of where we are in the color cycle between chars
uint* cyclingColor = (uint * )(calloc(1, sizeof(uint)));

int loopcount = 0;

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
    softSerial.begin(9600);
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

// https://learn.adafruit.com/led-tricks-gamma-correction/the-quick-fix

const uint PROGMEM
        gamma[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2,
        2, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5,
        5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10,
        10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16,
        17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25,
        25, 26, 27, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36,
        37, 38, 39, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 50,
        51, 52, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68,
        69, 70, 72, 73, 74, 75, 77, 78, 79, 81, 82, 83, 85, 86, 87, 89,
        90, 92, 93, 95, 96, 98, 99, 101, 102, 104, 105, 107, 109, 110, 112, 114,
        115, 117, 119, 120, 122, 124, 126, 127, 129, 131, 133, 135, 137, 138, 140, 142,
        144, 146, 148, 150, 152, 154, 156, 158, 160, 162, 164, 167, 169, 171, 173, 175,
        177, 180, 182, 184, 186, 189, 191, 193, 196, 198, 200, 203, 205, 208, 210, 213,
        215, 218, 220, 223, 225, 228, 231, 233, 236, 239, 241, 244, 247, 249, 252, 255
};

// Map 0-255 visual brightness to 0-255 LED brightness
#define GAMMA(x) (pgm_read_byte(&gamma[x]))

// TODO idxToBlink that isn't always last char
void showAsInputStyle(char* str, int idxToBlink, int mode) {

//    str = str + STRING_PADDING(FONTSTD_WIDTH) + 1; // drop initial padding

    int length = strlen(str);
    int shiftBy = constrain(length - MAX_CHARS_PER_PANEL, 0, length);
    // Shift string from the start if it is longer than the max chars we can show,
    // so that last MAX_CHARS_PER_PANEL chars are always visible (plus _)
    str = str + shiftBy;

    unsigned int count = 20;

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

void showAsChooser(char* blinkyStr, char* countyStr) {

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

void showcountdown(char* countdownstr, unsigned int count = 600) {

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
            uint row = pgm_read_byte_near(charbase++);
            sendColumnRGB(row << shiftabs, r, g, b);
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

//TODO take char, pad (& center) to MAX_CHARS_PER_PANEL, and multiply by NUM_PANELS
//void showCharsOneByOneOnBothPanels(const char *pointsStr, Color textColor, int delayMs = 500) {
//    showCharsOneByOneAndWait(pointsStr, textColor.r, textColor.g, textColor.b, delayMs);
//}

//void showCharsOneByOne(const char *pointsStr, uint r, uint g, uint b) {
//    showCharsOneByOneAndWait(pointsStr, r, g, b, 500);
//}

void showStringColorCycleOnBothPanels(const uint* font, const int fontWidth, char* str, int columnsPrefix,
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

void showallyourbasestyleOnBothPanels(char* str, int columnsPrefix) {
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

#define ENEMIES_WIDTH 12

//TODO hmm, the last one is an explosion and I've never seen that displayed before
const uint enemies[]
        PROGMEM = {
                // anjuna v1: upside down anjuna
//              0x00, 0x0C, 0x1e, 0x3b, 0x19, 0x0b, 0x05, 0x09, 0x11, 0x22, 0x44, 0x48, 0x50, 0x60, 0x40, 0x00,
                // anjuna v2: inner downward triangle touches bar
//                0x08, 0x18, 0x3C, 0x6E, 0x4C, 0x68, 0x50, 0x48, 0x44, 0x22, 0x11, 0x09, 0x05, 0x03, 0x01,
                // anjuna v3: space between bar and downward triangle, necessitating a larger downward triangle
//                0x08, 0x1C, 0x3E, 0x6C, 0x48, 0x60, 0x50, 0x48, 0x44, 0x22, 0x11, 0x09, 0x05, 0x03, 0x01,
//                0x08, 0x1C, 0x3E, 0x6C, 0x48, 0x60, 0x50, 0x48, 0x44, 0x22, 0x11, 0x09, 0x05, 0x03, 0x01,
                // anjuna v3 with filled in bar
//                0x08, 0x1C, 0x3E, 0x6C, 0x48, 0x60, 0x70, 0x78, 0x7C, 0x3E, 0x1F, 0x0F, 0x07, 0x03, 0x01
                // baaahs v1: upside down
//                0x06, 0x0d, 0x1D, 0x33, 0x47, 0x42, 0x47, 0x33, 0x1D, 0x0d, 0x06,
                // baaahs v2:
//                0x30, 0x58, 0x5C, 0x66, 0x71, 0x21, 0x71, 0x66, 0x5C, 0x58, 0x30,
//                0x30, 0x58, 0x5C, 0x66, 0x71, 0x21, 0x71, 0x66, 0x5C, 0x58, 0x30

                //
                0x70, 0xf4, 0xfe, 0xda, 0xd8, 0xf4, 0xf4, 0xd8, 0xda, 0xfe, 0xf4, 0x70, // Enemy 1 - open
                0x72, 0xf2, 0xf4, 0xdc, 0xd8, 0xf4, 0xf4, 0xd8, 0xdc, 0xf4, 0xf2, 0x72, // Enemy 1 - close
                0x1c, 0x30, 0x7c, 0xda, 0x7a, 0x78, 0x7a, 0xda, 0x7c, 0x30, 0x1c, 0x00, // Enemy 2 - open
                0xf0, 0x3a, 0x7c, 0xd8, 0x78, 0x78, 0x78, 0xd8, 0x7c, 0x3a, 0xf0, 0x00, // Enemy 2 - closed
                0x92, 0x54, 0x10, 0x82, 0x44, 0x00, 0x00, 0x44, 0x82, 0x10, 0x54, 0x92, // Explosion
        };

void showInvaders() {

    uint acount = PIXELS / (ENEMIES_WIDTH + FONTSTD_WIDTH);      // How many aliens do we have room for?

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
                sendIcon(enemies, p & 1, row, ENEMIES_WIDTH, GAMMA(0x4f), GAMMA(0x62), GAMMA(0xd2));
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

void marquee() {
    //TODO change the harcoded padding to be generated by the length of STRING_PADDING
    const char paddingString[STRING_PADDING(FONTSTD_WIDTH) + 1] = "                    ";
    const char* paddingPtr = paddingString;
    const char* marqueePtr = currentBuffer;

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

            delay(MARQUEE_DELAY); // speed. higher = slower

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
    StaticJsonDocument<JSON_SIZE> doc;

    startSerial();
    // send special symbol so IMX knows to respond with custom message data
    softSerial.print(msgType);

    // Read the JSON document from the serial port
    DeserializationError err = deserializeJson(doc, softSerial);

    keyboardInputMode = NULL;

    if (err == DeserializationError::Ok) {
        strcpy(currentBuffer, doc["string"]);
        msgType = doc["type"].as<const char*>()[0];
        switch (msgType) {
            case MSGTYPE_ONE_BY_ONE:
            case MSGTYPE_CHONKY_SLIDE: { //OneByOne, ChonkySlide
                textColor = {doc["r"], doc["g"], doc["b"]};
                textDelay = doc["delayMs"];
                break;
            }
            case MSGTYPE_CHOOSER: { // Chooser Mode
                strcpy(extraDataBuffer, doc["flashy"]);
                break;
            }
            case MSGTYPE_UTILITY: { // Utility messages
                char subtype = doc["subtype"].as<const char*>()[0];

                switch (subtype) {
                    case 'M': { // Microphone enable/disable
                        if (*currentBuffer == 'E') {
                            PCICR |= bit (PCIE1); // enable pin change interrupts for A0 to A5
                        } else if (*currentBuffer == 'D') {
                            PCICR &= ~bit (PCIE1); // disable pin change interrupts for A0 to A5
                        }
                        break;
                    }
                }
                break;
            }

            case MSGTYPE_KEYBOARD: { // Keyboard input
                keyboardInputMode = doc["mode"].as<const char*>()[0];
                break;
            }
        }

        stopSerial();
        diagnosticBlink();
        return true;
    } else {
        softSerial.print(err.c_str());

        // do I need to clear out the rest of the unread serial stream?
//        while(softSerial.available()) {
//            softSerial.read();
//        }
        stopSerial();
//        showcountdown((char*) err.c_str());
        diagnosticBlink();
        diagnosticBlink();
        diagnosticBlink();
        diagnosticBlink();
        return false;
    }
}

void setup() {
    strcpy_P(currentBuffer, ASOT);

    if (DEBUG) {
        currentBuffer[10] = 0x00; //chop off intro when debugging
    }

    delay(100); //Try to introduce a bit of a delay to clear out serial noise on boot
    setupDiagnosticLed();
    setupLeds();

    showcountdown("I", 80);
    showcountdown("LIVE", 80);
    showcountdown("FOR", 80);
    showcountdown("THAT", 80);
    showcountdown("!ENERGY!", 80);
//    showcountdown("!!ENERGY!!", 80);
//    showcountdown("!ENERGY!", 80);
    showstarfield();

    setupSerial();

    // A0 pin change interrupt
    digitalWrite(A0, HIGH);  // enable pull-up
    PCMSK1 |= bit (PCINT8);  // want pin A0
    PCIFR |= bit (PCIF1);   // clear any outstanding interrupts
    PCICR |= bit (PCIE1);   // enable pin change interrupts for A0 to A5
    // TODO disable based on IMX command

    loopcount = 0;
}

void loop() {
//return
    //showcountdown();
    //showstarfield();
    //showjabber();

//    diagnosticBlink();
    diagnosticLedOn();
    readSerialData();
    diagnosticLedOff();

    switch (msgType) {
        case MSGTYPE_FLASHY: {
            showcountdown(currentBuffer); //TODO extract color //TODO center
            showstarfield();
            break;
        }
        case MSGTYPE_ONE_BY_ONE: {
            showCharsOneByOneOnBothPanels(currentBuffer, textColor, textDelay);
            break;
        }
        case MSGTYPE_CHONKY_SLIDE: {
            showallyourbasestyleOnBothPanels(currentBuffer, 4);  //TODO extract color //TODO center
            break;
        }
        case MSGTYPE_ICON: {
            showInvaders();
            break;
        }
        case MSGTYPE_UTILITY: {
            showInvaders();
            break;
        }
        case MSGTYPE_KEYBOARD: {
            showAsInputStyle(currentBuffer, strlen(currentBuffer) - 1 - 1, keyboardInputMode);
            break;
        }
        case MSGTYPE_CHOOSER: {
            showAsChooser(currentBuffer, extraDataBuffer);
            break;
        }
        case MSGTYPE_DEFAULT:
        default: {
            showstarfieldcustom(100);
            marquee();
            break;
        }
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
                showInvaders();
            }
        }
    }
    */

    // TODO: Actually sample the state of the pullup on unused pins and OR it into the mask so we maintain the state.
    // Must do AFTER the cli().
    // TODO: Add offBits also to maintain the pullup state of unused pins.

    loopcount++;
//    return;
}



