//
// The game frame ('G'): the one picture a message of the jump game describes. The app runs the game
// (the jump, the obstacles, the speed, the collisions, the score); the sign draws the picture a
// frame gives it and asks for the next one (GameDisplay.h). Both faces show the same 60 columns.
//
// The frame is the message's text:    rise picture { item }
//   rise      one digit, 0 to 7: how many rows the runner is above the ground. Rows above the top
//             edge are cut off; at 7 nothing of it is left on the sign
//   picture   one digit, 0 or 1: which of the runner's two pictures
//   items, in any order:
//     a<col> b<col> c<col>   an obstacle of that shape with its left column there; at most 4
//     r<col>                 the runner's left column (2 without it); at most once
//     p<runner><obstacles>   two colour digits, 0 to 7 (p62 without it: blue, red); at most once
//     e<digit>               0 nothing, 1 sparkle, 2 flash; at most once
//   <col> is -99 to 99: an optional '-' and one or two digits. Column 0 is a face's left edge; what
//   is left of it or right of column 59 is cut off.
// Anything else makes the whole frame a bad one: nothing of it is drawn.
//
// One colour per column is the display's rule: a column an obstacle lights is in the obstacles'
// colour, the runner's rows in it too.
//
// Pure: no hardware and no Arduino functions, only PROGMEM and pgm_read_byte_near for the tables.
// The same file is compiled into the sketch and into the host test (test/game_frame_test.cpp).
//

#ifndef LED_MARQUEE_GAMEFRAME_H
#define LED_MARQUEE_GAMEFRAME_H

#include <stdint.h>
#include <string.h>

#define GAME_COLS 60          // the columns of a face
#define GAME_RUNNER_COLS 13   // the runner's width: the Dreamstate icon of Icons.h
#define GAME_RUNNER_COL 2     // its left column when the frame does not say
#define GAME_RUNNER_COLOUR 6      // the colours when the frame does not say: blue,
#define GAME_OBSTACLE_COLOUR 2    // red
#define GAME_MAX_OBSTACLES 4
#define GAME_COLOURS 8        // colour digits 0 to 7 (the values are in GameDisplay.h)

#define GAME_EFFECT_SPARKLE 1 // a few stars in dark columns, new in every frame (GameDisplay.h adds them)
#define GAME_EFFECT_FLASH 2   // the picture in negative, in the obstacles' colour, dimmed

// The obstacles, one byte a column like the fonts: bit 1 is the bottom row, bit 7 the top.
//   a  step   3 columns, 1 2 1 rows high
//   b  post   2 columns, 3 rows high
//   c  ridge  5 columns, 1 2 3 2 1 rows high
static const uint8_t gameShapes[] PROGMEM = {
        0x02, 0x06, 0x02,
        0x0E, 0x0E,
        0x02, 0x06, 0x0E, 0x06, 0x02,
};
static const uint8_t gameShapeAt[] PROGMEM = {0, 3, 5, 10}; // where each shape begins, and the end

struct GamePicture {
    uint8_t rows[GAME_COLS];   // the lit rows of each column
    uint8_t colour[GAME_COLS]; // the colour digit of each column
    uint8_t effect;            // the frame's effect digit
};

// Reads a column number: an optional '-', then one or two digits. False when no digit is there.
static bool gameReadCol(const char*& s, int8_t& col) {
    const bool negative = *s == '-';
    if (negative) {
        s++;
    }
    if (*s < '0' || *s > '9') {
        return false;
    }
    int8_t value = *s++ - '0';
    if (*s >= '0' && *s <= '9') {
        value = (int8_t) (value * 10 + (*s++ - '0'));
    }
    col = negative ? (int8_t) -value : value;
    return true;
}

// Puts `width` columns of a shape into the picture with its left column at `col`, `rise` rows up.
// A column that gets a lit row takes the colour. What is off the face or above the top row is cut.
static void gamePut(GamePicture& pic, const uint8_t* shape, uint8_t width, int8_t col, uint8_t rise, uint8_t colour) {
    for (; width; width--, col++, shape++) {
        if (col < 0 || col >= GAME_COLS) {
            continue;
        }
        const uint8_t rows = (uint8_t) (pgm_read_byte_near(shape) << rise);
        if (rows) {
            pic.rows[col] |= rows;
            pic.colour[col] = colour;
        }
    }
}

// The picture of a frame. `runner` is the runner's two pictures, GAME_RUNNER_COLS bytes each, in
// program memory. Returns false for a bad frame (see the top of this file); `pic` is then not one.
static bool gameRender(const char* s, const uint8_t* runner, GamePicture& pic) {
    if (!s || s[0] < '0' || s[0] > '7' || s[1] < '0' || s[1] > '1') {
        return false;
    }
    const uint8_t rise = s[0] - '0';
    runner += (uint8_t) (s[1] - '0') * GAME_RUNNER_COLS;
    s += 2;

    int8_t runnerCol = GAME_RUNNER_COL;
    uint8_t runnerColour = GAME_RUNNER_COLOUR;
    uint8_t obstacleColour = GAME_OBSTACLE_COLOUR;
    uint8_t seen = 0;         // which of r, p, e have come: each at most once
    uint8_t obstacles = 0;
    uint8_t shape[GAME_MAX_OBSTACLES];
    int8_t at[GAME_MAX_OBSTACLES];
    pic.effect = 0;

    while (*s) {
        const char item = *s++;
        uint8_t once = 0;
        if (item >= 'a' && item <= 'c') {
            if (obstacles == GAME_MAX_OBSTACLES || !gameReadCol(s, at[obstacles])) {
                return false;
            }
            shape[obstacles++] = item - 'a';
        } else if (item == 'r') {
            once = 1;
            if (!gameReadCol(s, runnerCol)) {
                return false;
            }
        } else if (item == 'p') {
            once = 2;
            if (s[0] < '0' || s[0] > '7' || s[1] < '0' || s[1] > '7') {
                return false;
            }
            runnerColour = s[0] - '0';
            obstacleColour = s[1] - '0';
            s += 2;
        } else if (item == 'e') {
            once = 4;
            if (s[0] < '0' || s[0] > '2') {
                return false;
            }
            pic.effect = *s++ - '0';
        } else {
            return false;
        }
        if (seen & once) {
            return false;
        }
        seen |= once;
    }

    memset(pic.rows, 0, GAME_COLS);
    memset(pic.colour, 0, GAME_COLS);
    gamePut(pic, runner, GAME_RUNNER_COLS, runnerCol, rise, runnerColour);
    while (obstacles--) {
        const uint8_t from = pgm_read_byte_near(gameShapeAt + shape[obstacles]);
        const uint8_t to = pgm_read_byte_near(gameShapeAt + shape[obstacles] + 1);
        gamePut(pic, gameShapes + from, to - from, at[obstacles], 0, obstacleColour);
    }
    if (pic.effect == GAME_EFFECT_FLASH) {
        for (uint8_t c = 0; c < GAME_COLS; c++) {
            // The colours first, then the negative: what colour 0 leaves dark is not in the
            // picture, and is lit in its negative like every other dark LED.
            const uint8_t lit = pic.colour[c] ? pic.rows[c] : 0;
            pic.rows[c] = (uint8_t) ~lit & 0xFE; // (bit 0 is no row)
            pic.colour[c] = obstacleColour;
        }
    }
    return true;
}

#endif //LED_MARQUEE_GAMEFRAME_H
