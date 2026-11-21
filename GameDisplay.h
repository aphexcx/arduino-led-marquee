//
// The jump game on the sign (MSGTYPE_GAME, 'G'). The app runs the game. The sign draws the picture
// of the frame it was given (GameFrame.h) on both faces and asks for the next one at once, with the
// same '~' as ever; it keeps nothing of the game. The game goes on for as long as the app answers
// with game frames, and ends with the first answer of another kind, which loop() then shows.
//
// A frame is the whole picture: all 120 columns are sent, so nothing is cleared before it and
// nothing of the picture before it stays.
//
// A frame is read as every message is: through the chooser's reader (ChooserReader, in
// ChooserDisplay.h), so that ArduinoJson's parser is built once in the image. So the bytes that are
// nobody's (outside a text every byte that is neither white space nor 0x20 to 0x7E, inside a text
// a byte above 0x7F that is no part of a UTF-8 character: what noise on the line is received as)
// are dropped before the frame is read, up to CHOOSER_MAX_NOISE of them in one frame, and a frame
// that is good without them is drawn. From the next one on they are read as they come, as in every
// message. And as any byte does, a byte that is dropped gives the next one GAME_MISS_MS again: a line
// that carries only noise is a miss GAME_MISS_MS behind its last such byte (and never later than
// GAME_LEAVE_MS allows).
//
// WHEN AN ANSWER IS NOT A GOOD FRAME (it does not come, it stops half way, it never ends, it is no
// JSON, it is an object without a type or with an empty one, it has more than GAME_MAX_KEYS keys,
// it is a game frame the sign cannot read) there is one rule for all of it:
//   - nothing is drawn and nothing is said: the last good picture stays on the LEDs, no error text
//     goes to the LEDs or to the app
//   - the sign waits until the line has been silent for GAME_QUIET_MS and asks again. (Asking in
//     the middle of what is still arriving would read its rest as a second bad answer, and then
//     the answer to that request too. The wait is longer than the app takes to answer, so that an
//     answer that was on its way goes by in it: after an answer that came too late, the app and
//     the sign are in step again from the next bad answer on.)
//   - when no good frame has come for GAME_LEAVE_MS the game is over for the sign: loop() asks the
//     ordinary way again, with the ordinary wait, the stars and the error text. That is the one
//     bound on everything the sign waits for in a game: no byte is read from the line after it
//     (AppSerial::read() in the sketch), whatever is still arriving. So an answer that never
//     ends (bytes that keep coming and never make an object) ends there, like one that stops.
//
// This file is part of the sketch: LED_Marquee.ino includes it after everything it uses (the GAME_*
// settings at the top of the sketch, MSGTYPE_GAME, the Dreamstate icon, sendColumnRGB, show,
// startSerial, stopSerial, softSerial and its AppSerial, json, ChooserReader).
//

#ifndef LED_MARQUEE_GAMEDISPLAY_H
#define LED_MARQUEE_GAMEDISPLAY_H

#include "GameFrame.h"

static_assert(GAME_COLS == COLUMNS_PER_PANEL, "GameFrame.h: the width of a face");
static_assert(GAME_MISS_MS >= 100 && GAME_MISS_MS <= 500, "GAME_MISS_MS must be 100 to 500");
static_assert(GAME_QUIET_MS >= 10 && GAME_QUIET_MS <= 100, "GAME_QUIET_MS must be 10 to 100");
static_assert(GAME_LEAVE_MS >= 2 * (GAME_MISS_MS + GAME_QUIET_MS) && GAME_LEAVE_MS <= 10000,
              "GAME_LEAVE_MS must be at least two missed answers and at most 10000");

// A frame has two keys. An answer with more than this many is not a good frame (the document has
// room for eight; a ninth with a short name would fit beside them in its room for the text).
#define GAME_MAX_KEYS 8

// The flash's picture lights nearly every LED of the sign: its colour is shifted right by this
#define GAME_FLASH_SHIFT 3

// The sparkle: this many stars, each one LED in a column the picture leaves dark, in a colour of
// its own (1 to 7). One random number places a star: a byte each for column, row and colour.
// (A column with a moving star is not dark: the sparkle leaves the moving stars as they are.)
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

// Sends the picture to both faces and latches it: in the colours of this moment, the score where
// the sign's clock has it on the wheel, the runner and the score as the last beat makes them.
static void gameDraw(const GamePicture& pic) {
    uint8_t colours[GAME_FRAME_COLOURS * 3];
    gameColours(pic, millis(), msSinceBeat(), colours);
    const uint shift = pic.effect == GAME_EFFECT_FLASH ? GAME_FLASH_SHIFT : 0;
    cli();
    for (uint face = NUM_PANELS; face; face--) {
        for (uint c = 0; c < GAME_COLS; c++) {
            const uint8_t* rgb = colours + pic.colour[c] * 3;
            sendColumnRGB(pic.rows[c], rgb[0] >> shift, rgb[1] >> shift, rgb[2] >> shift);
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
    bool whole = true; // `json` holds a whole answer: loop()'s read brought the first
    // When the last good frame was drawn (or the game was entered). Nothing is read from the line
    // once GAME_LEAVE_MS have gone by since then.
    softSerial.readSinceMs = millis();
    softSerial.readForMs = GAME_LEAVE_MS;

    for (;;) {
        const char* type = 0;
        if (whole) {
            type = json["type"];
        }
        if (type && !type[0]) {
            type = 0; // an empty type is no type
        }
        if (type && type[0] != MSGTYPE_GAME) {
            stopSerial();
            softSerial.readForMs = 0;
            return true;
        }

        GamePicture pic;
        const bool good = type && json.size() <= GAME_MAX_KEYS && gameRender(json["str"], DREAMSTATE.data, millis(), pic);
        if (!good) {
            // Whatever is still arriving goes by, until the line is silent. (Not for longer than
            // the game may go without a frame: a line that never falls silent ends the game.)
            char gone;
            softSerial.setTimeout(GAME_QUIET_MS);
            while (softSerial.readBytes(&gone, 1)) {
            }
        }
        stopSerial(); // nothing is heard while a frame goes out (interrupts are off for it)

        if (good) {
            if (pic.effect == GAME_EFFECT_SPARKLE) {
                gameSparkle(pic);
            }
            gameDraw(pic);
            softSerial.readSinceMs = millis();
        } else if (millis() - softSerial.readSinceMs >= GAME_LEAVE_MS) {
            softSerial.setTimeout(1000); // the Stream default, for the normal '~' read
            softSerial.readForMs = 0;
            return false;
        }

        startSerial(); // (this starts with an empty buffer)
        softSerial.setTimeout(GAME_MISS_MS);
        softSerial.print('~');
        ChooserReader reader = {false, false, false, false, 0, 0, -1}; // (no brace taken in front)
        whole = deserializeJson(json, reader) == DeserializationError::Ok;
        softSerial.setTimeout(1000);
    }
}

#endif //LED_MARQUEE_GAMEDISPLAY_H
