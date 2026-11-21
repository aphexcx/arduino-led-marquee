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
//     s<number>              the moving stars, 0 to 59 columns to the left of where they are at 0
//                            (none without it); at most once
//     n<digits>              the score: one to three digits, drawn as written at the top right
//                            (none without it); at most once
//     t<col>                 the treat: a small purple thing to eat, in rows 5 to 7, that
//                            twinkles by itself (none without it); at most once
//   <col> is -99 to 99: an optional '-' and one or two digits. Column 0 is a face's left edge; what
//   is left of it or right of column 59 is cut off.
// Anything else makes the whole frame a bad one: nothing of it is drawn.
//
// One colour per column is the display's rule. From the weakest: a column only the runner lights
// is in the runner's colour; one a digit of the score lights is in the score's, the runner's rows
// in it too; one the treat lights is in the treat's, the digits' and the runner's rows in it too;
// one an obstacle lights is in the obstacles' colour, everything's rows in it too.
// The runner and the score flash to the beat as a marquee message does, and the score's own colour
// moves through the marquee's wheel (MarqueeColour.h): nothing else flashes, so nothing in a
// column of the treat or of an obstacle does. gameColours() gives those colours for a moment.
//
// Pure: no hardware and no Arduino functions, only PROGMEM and pgm_read_byte_near for the tables.
// The same file is compiled into the sketch and into the host test (test/game_frame_test.cpp).
//

#ifndef LED_MARQUEE_GAMEFRAME_H
#define LED_MARQUEE_GAMEFRAME_H

#include <stdint.h>
#include <string.h>

#include "MarqueeColour.h"

#define GAME_COLS 60          // the columns of a face
#define GAME_RUNNER_COLS 13   // the runner's width: the Dreamstate icon of Icons.h
#define GAME_RUNNER_COL 2     // its left column when the frame does not say
#define GAME_RUNNER_COLOUR 6      // the colours when the frame does not say: blue,
#define GAME_OBSTACLE_COLOUR 2    // red
#define GAME_MAX_OBSTACLES 4
#define GAME_COLOURS 8        // colour digits 0 to 7

#define GAME_EFFECT_SPARKLE 1 // a few stars in dark columns, new in every frame (GameDisplay.h adds them)
#define GAME_EFFECT_FLASH 2   // the picture in negative, in the obstacles' colour, dimmed

// The colours a picture's columns name. After the digits' 0 to 7 come the treat's two and the
// moving stars', whose values are in the palette below; then two that are worked out for the
// moment the frame is drawn (gameColours): the runner's colour and the score's, as the beat and
// the wheel make them.
#define GAME_TREAT_COLOUR GAME_COLOURS         // the treat's purple
#define GAME_TREAT_GLINT (GAME_COLOURS + 1)    // the treat's bright tint, for its twinkle
#define GAME_STAR_COLOUR (GAME_COLOURS + 2)    // the moving stars'
#define GAME_PALETTE (GAME_COLOURS + 3)        // the colours of the palette
#define GAME_RUNNER_LIT GAME_PALETTE           // the runner's colour digit, flashing to the beat
#define GAME_SCORE_LIT (GAME_PALETTE + 1)      // the wheel's colour, flashing to the beat
#define GAME_FRAME_COLOURS (GAME_PALETTE + 2)  // all of them: gameColours() gives a value to each

// The palette, as red, green, blue: the digits' colours dark, white, red, orange, yellow, green,
// blue, pink; the treat's purple and its glint; the moving stars' white at an eighth of colour 1.
// Every value is a run of one-bits from bit 0 up, and stays one when it is shifted right, so it
// reaches the LEDs as it is written here whatever the carry flag holds when sendBitx8 begins (see
// AdDisplay.h), at every brightness shift.
static const uint8_t gamePalette[GAME_PALETTE * 3] PROGMEM = {
        0x00, 0x00, 0x00,
        0x7F, 0x7F, 0x7F,
        0xFF, 0x00, 0x00,
        0xFF, 0x1F, 0x00,
        0xFF, 0x7F, 0x00,
        0x00, 0xFF, 0x00,
        0x00, 0x00, 0xFF,
        0xFF, 0x00, 0x7F,
        0x7F, 0x00, 0xFF,
        0xFF, 0x7F, 0xFF,
        0x0F, 0x0F, 0x0F,
};

// The score's place on the marquee's wheel moves on a step every this many ms of the sign's clock:
// a whole turn of MARQUEE_WHEEL_SECTORS * (JAB_STEPS + 1) steps is 13.8 s of it, which loses about
// 4 ms of every frame it sends (interrupts are off for it): about 15 s of play.
#define GAME_WHEEL_STEP_MS 18
#define GAME_WHEEL_STEPS (MARQUEE_WHEEL_SECTORS * (JAB_STEPS + 1))

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

// The moving stars: a fixed pattern as wide as a face, one LED each, drawn where the play leaves a
// column dark and moved to the left by the frame's number; what leaves on the left comes in on the
// right. A byte a star: its column at 0 in bits 0 to 5, and in bits 6 and 7 how many rows it is
// above row 4, so that no star is in the three rows the obstacles stand in.
//   columns 2 9 19 25 33 40 47 55, rows 7 5 6 4 7 5 4 6
#define GAME_STARS 8
static const uint8_t gameStars[GAME_STARS] PROGMEM = {0xC2, 0x49, 0x93, 0x19, 0xE1, 0x68, 0x2F, 0xB7};

// The score's font: GAME_DIGIT_COLS columns a digit, one byte a column like the obstacles' (bit 1
// is the digit's bottom row), GAME_DIGIT_ROWS rows high. It is drawn raised so that its top row is
// the top row of the sign. The font is this table and that number, and nothing else.
//   (the rows from the top)   0 ### #.# #.# ###   1 .#. ##. .#. ###   2 ##. ..# .#. ###
//   3 ### .## ..# ###   4 #.# #.# ### ..#   5 ### ##. ..# ##.   6 #.. ### #.# ###
//   7 ### ..# .#. .#.   8 ### #.# ### ###   9 ### #.# ### ..#
#define GAME_DIGIT_ROWS 4
#define GAME_DIGIT_COLS 3
#define GAME_SCORE_DIGITS 3 // the most a score has
static const uint8_t gameDigits[10 * GAME_DIGIT_COLS] PROGMEM = {
        0x1E, 0x12, 0x1E,
        0x0A, 0x1E, 0x02,
        0x12, 0x16, 0x0A,
        0x12, 0x1A, 0x1E,
        0x1C, 0x04, 0x1E,
        0x1A, 0x1A, 0x14,
        0x1E, 0x0A, 0x0E,
        0x10, 0x16, 0x18,
        0x1E, 0x16, 0x1E,
        0x1C, 0x14, 0x1E,
};

// The treat: GAME_TREAT_COLS columns wide, in rows 5 to 7 only. It twinkles by itself: its look
// changes every GAME_TWINKLE_MS of the sign's clock, through GAME_TREAT_LOOKS pictures, and comes
// round again. A picture is two bytes a column, left to right: the rows it lights there (bit 1 is
// the bottom row, as the obstacles'), and their colour: 0 the treat's purple, 1 its glint.
//   a gem, the glint going across it and a rest:  .#.  ###  .#.  (rows 7, 6, 5)
#define GAME_TREAT_COLS 3
#define GAME_TREAT_LOOKS 6
#define GAME_TWINKLE_MS 100
static const uint8_t gameTreat[GAME_TREAT_LOOKS * GAME_TREAT_COLS * 2] PROGMEM = {
        0x40, 1, 0xE0, 0, 0x40, 0,
        0x40, 0, 0xE0, 1, 0x40, 0,
        0x40, 0, 0xE0, 0, 0x40, 1,
        0x40, 0, 0xE0, 0, 0x40, 0,
        0x40, 0, 0xE0, 0, 0x40, 0,
        0x40, 0, 0xE0, 0, 0x40, 0,
};

struct GamePicture {
    uint8_t rows[GAME_COLS];   // the lit rows of each column
    uint8_t colour[GAME_COLS]; // the colour of each column: a digit, or one of the colours after them
    uint8_t effect;            // the frame's effect digit
    uint8_t runnerColour;      // the runner's colour digit (GAME_RUNNER_LIT flashes it to the beat)
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
// program memory; `now` is the sign's clock in ms, which says how the treat looks. Returns false
// for a bad frame (see the top of this file); `pic` is then not one.
static bool gameRender(const char* s, const uint8_t* runner, unsigned long now, GamePicture& pic) {
    if (!s || s[0] < '0' || s[0] > '7' || s[1] < '0' || s[1] > '1') {
        return false;
    }
    const uint8_t rise = s[0] - '0';
    runner += (uint8_t) (s[1] - '0') * GAME_RUNNER_COLS;
    s += 2;

    int8_t runnerCol = GAME_RUNNER_COL;
    uint8_t runnerColour = GAME_RUNNER_COLOUR;
    uint8_t obstacleColour = GAME_OBSTACLE_COLOUR;
    int8_t stars = -1;        // how far the moving stars are moved; none below 0
    const char* score = 0;    // the score's digits in the text, and how many (none: no score)
    uint8_t digits = 0;
    bool treat = false;       // the treat, and its left column
    int8_t treatCol = 0;
    uint8_t seen = 0;         // which of r, p, e, s, n, t have come: each at most once
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
        } else if (item == 's') {
            once = 8;
            if (!gameReadCol(s, stars) || stars < 0 || stars >= GAME_COLS) {
                return false;
            }
        } else if (item == 'n') {
            once = 16;
            score = s;
            for (digits = 0; digits < GAME_SCORE_DIGITS && *s >= '0' && *s <= '9'; digits++) {
                s++;
            }
            if (!digits) {
                return false;
            }
        } else if (item == 't') {
            once = 32;
            if (!gameReadCol(s, treatCol)) {
                return false;
            }
            treat = true;
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
    pic.runnerColour = runnerColour;
    // From the weakest colour to the strongest: what is drawn later takes the column.
    gamePut(pic, runner, GAME_RUNNER_COLS, runnerCol, rise, runnerColour ? GAME_RUNNER_LIT : 0);
    // The score as written, from its last digit, whose right column is the face's last; a dark
    // column between two digits.
    for (int8_t col = GAME_COLS - GAME_DIGIT_COLS; digits--; col -= GAME_DIGIT_COLS + 1) {
        gamePut(pic, gameDigits + (uint8_t) (score[digits] - '0') * GAME_DIGIT_COLS, GAME_DIGIT_COLS, col,
                7 - GAME_DIGIT_ROWS, GAME_SCORE_LIT);
    }
    // The treat as it looks now, a column at a time, each in its own colour
    if (treat) {
        const uint8_t* column = gameTreat + (uint8_t) (now / GAME_TWINKLE_MS % GAME_TREAT_LOOKS) * GAME_TREAT_COLS * 2;
        for (uint8_t i = 0; i < GAME_TREAT_COLS; i++, column += 2) {
            gamePut(pic, column, 1, (int8_t) (treatCol + i), 0, GAME_TREAT_COLOUR + pgm_read_byte_near(column + 1));
        }
    }
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
    } else if (stars >= 0) {
        for (uint8_t n = 0; n < GAME_STARS; n++) {
            const uint8_t star = pgm_read_byte_near(gameStars + n);
            int8_t col = (int8_t) ((star & 0x3F) - stars);
            if (col < 0) {
                col += GAME_COLS;
            }
            if (!pic.rows[col]) { // (a column of the play has no star, in colour 0 too)
                pic.rows[col] = (uint8_t) (0x10 << (star >> 6));
                pic.colour[col] = GAME_STAR_COLOUR;
            }
        }
    }
    return true;
}

// The value of every colour a picture's columns name, as red, green, blue, three bytes each, for
// the moment it is drawn: `now` is the sign's clock in ms, which says where the score is on the
// marquee's wheel, and `sinceBeat` how many ms ago the last beat was. The runner's colour and the
// score's flash to the beat as a marquee message's letters do (beatFade); the rest are the
// palette's.
static void gameColours(const GamePicture& pic, unsigned long now, unsigned long sinceBeat,
                        uint8_t rgb[GAME_FRAME_COLOURS * 3]) {
    for (uint8_t i = 0; i < GAME_PALETTE * 3; i++) {
        rgb[i] = pgm_read_byte_near(gamePalette + i);
    }
    const uint16_t wheel = (uint16_t) (now / GAME_WHEEL_STEP_MS % GAME_WHEEL_STEPS);
    const Color score = marqueeWheel((uint) (wheel / (JAB_STEPS + 1)), (uint) (wheel % (JAB_STEPS + 1)));
    const uint8_t own[6] = {rgb[pic.runnerColour * 3], rgb[pic.runnerColour * 3 + 1], rgb[pic.runnerColour * 3 + 2],
                            score.r, score.g, score.b};
    static_assert(GAME_SCORE_LIT == GAME_RUNNER_LIT + 1, "the runner's colour and then the score's");
    for (uint8_t i = 0; i < 6; i++) {
        rgb[GAME_RUNNER_LIT * 3 + i] = (uint8_t) beatFade(own[i], sinceBeat);
    }
}

#endif //LED_MARQUEE_GAMEFRAME_H
