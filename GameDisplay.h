//
// The jump game on the sign (MSGTYPE_GAME, 'G'). The app runs the game. The sign draws the picture
// of the frame it was given (GameFrame.h) on both faces and asks for the next one at once, with the
// same '~' as ever; it keeps nothing of the game. The game goes on for as long as the app answers
// with game frames, and ends with the first answer of another kind, which loop() then shows.
//
// A frame is the whole picture: all 120 columns are sent, so nothing is cleared before it and
// nothing of the picture before it stays.
//
// WHEN AN ANSWER IS NOT A GOOD FRAME (it does not come, it stops half way, it is no JSON, it is an
// object without a type, it is a game frame the sign cannot read) there is one rule for all of it:
//   - nothing is drawn and nothing is said: the last good picture stays on the LEDs, no error text
//     goes to the LEDs or to the app
//   - the sign waits until the line has been silent for GAME_QUIET_MS and asks again. (Asking in
//     the middle of what is still arriving would read its rest as a second bad answer, and then
//     the answer to that request too. The wait is longer than the app takes to answer, so that an
//     answer that was on its way goes by in it: after an answer that came too late, the app and
//     the sign are in step again from the next bad answer on.)
//   - when no good frame has come for GAME_LEAVE_MS the game is over for the sign: loop() asks the
//     ordinary way again, with the ordinary wait, the stars and the error text.
//
// This file is part of the sketch: LED_Marquee.ino includes it after everything it uses (the GAME_*
// settings at the top of the sketch, MSGTYPE_GAME, the Dreamstate icon, sendColumnRGB, show,
// startSerial, stopSerial, softSerial, json).
//

#ifndef LED_MARQUEE_GAMEDISPLAY_H
#define LED_MARQUEE_GAMEDISPLAY_H

#include "GameFrame.h"

static_assert(GAME_COLS == COLUMNS_PER_PANEL, "GameFrame.h: the width of a face");
static_assert(GAME_MISS_MS >= 100 && GAME_MISS_MS <= 500, "GAME_MISS_MS must be 100 to 500");
static_assert(GAME_QUIET_MS >= 10 && GAME_QUIET_MS <= 100, "GAME_QUIET_MS must be 10 to 100");
static_assert(GAME_LEAVE_MS >= 2 * (GAME_MISS_MS + GAME_QUIET_MS) && GAME_LEAVE_MS <= 10000,
              "GAME_LEAVE_MS must be at least two missed answers and at most 10000");

// The colours of the digits 0 to 7, as red, green, blue: dark, white, red, orange, yellow, green,
// blue, pink. Every value is a run of one-bits from bit 0 up, and stays one when it is shifted
// right, so it reaches the LEDs as it is written here whatever the carry flag holds when
// sendBitx8 begins (see AdDisplay.h), at every brightness shift.
static const uint8_t gamePalette[GAME_COLOURS * 3] PROGMEM = {
        0x00, 0x00, 0x00,
        0x7F, 0x7F, 0x7F,
        0xFF, 0x00, 0x00,
        0xFF, 0x1F, 0x00,
        0xFF, 0x7F, 0x00,
        0x00, 0xFF, 0x00,
        0x00, 0x00, 0xFF,
        0xFF, 0x00, 0x7F,
};

// The flash's picture lights nearly every LED of the sign: its colour is shifted right by this
#define GAME_FLASH_SHIFT 3

// The sparkle: this many stars, each one LED in a column the picture leaves dark, in a colour of
// its own (1 to 7). One random number places a star: a byte each for column, row and colour.
#define GAME_SPARKLE_STARS 4

static void gameSparkle(GamePicture& pic) {
    for (uint n = GAME_SPARKLE_STARS; n; n--) {
        const unsigned long r = random();
        const uint col = (uint) (((r & 0xFF) * GAME_COLS) >> 8);
        if (!pic.rows[col]) {
            pic.rows[col] = 2 << (uint) ((((r >> 8) & 0xFF) * 7) >> 8);
            pic.colour[col] = 1 + (uint) ((((r >> 16) & 0xFF) * (GAME_COLOURS - 1)) >> 8);
        }
    }
}

// Sends the picture to both faces and latches it.
static void gameDraw(const GamePicture& pic) {
    const uint shift = pic.effect == GAME_EFFECT_FLASH ? GAME_FLASH_SHIFT : 0;
    cli();
    for (uint face = NUM_PANELS; face; face--) {
        for (uint c = 0; c < GAME_COLS; c++) {
            const uint8_t* rgb = gamePalette + pic.colour[c] * 3;
            sendColumnRGB(pic.rows[c], pgm_read_byte_near(rgb) >> shift, pgm_read_byte_near(rgb + 1) >> shift,
                          pgm_read_byte_near(rgb + 2) >> shift);
        }
    }
    sei();
    show();
}

// Shows the game frame in `json` and stays here, asking the app for the next frame after each
// one, for as long as the game is on. Returns true when `json` holds a message of another kind
// for loop() to show, false when no good frame came for GAME_LEAVE_MS (loop() then asks the
// ordinary way).
bool showAsGame() {
    unsigned long goodMs = millis(); // when the last good frame was drawn (or the game was entered)
    bool whole = true;               // `json` holds a whole answer: loop()'s read brought the first

    for (;;) {
        const char* type = 0;
        if (whole) {
            type = json["type"];
        }
        if (type && type[0] != MSGTYPE_GAME) {
            stopSerial();
            return true;
        }

        GamePicture pic;
        const bool good = type && gameRender(json["str"], DREAMSTATE.data, pic);
        if (!good) {
            // Whatever is still arriving goes by, until the line is silent. (Not for longer than
            // the game may go without a frame: a line that never falls silent ends the game.)
            char gone;
            softSerial.setTimeout(GAME_QUIET_MS);
            while (softSerial.readBytes(&gone, 1) && millis() - goodMs < GAME_LEAVE_MS) {
            }
        }
        stopSerial(); // nothing is heard while a frame goes out (interrupts are off for it)

        if (good) {
            if (pic.effect == GAME_EFFECT_SPARKLE) {
                gameSparkle(pic);
            }
            gameDraw(pic);
            goodMs = millis();
        } else if (millis() - goodMs >= GAME_LEAVE_MS) {
            softSerial.setTimeout(1000); // the Stream default, for the normal '~' read
            return false;
        }

        startSerial(); // (this starts with an empty buffer)
        softSerial.setTimeout(GAME_MISS_MS);
        softSerial.print('~');
        whole = deserializeJson(json, softSerial) == DeserializationError::Ok;
        softSerial.setTimeout(1000);
    }
}

#endif //LED_MARQUEE_GAMEDISPLAY_H
