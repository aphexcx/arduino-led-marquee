// Host test for GameFrame.h: the text of a game frame, the picture it makes and its colours. The
// real gameRender() against a picture worked out here LED by LED, for every rise, every column a
// thing can be given (-99 to 99), every place of the moving stars and mixes of items in any order;
// one table for the score (its font as the contract draws it, its columns, beside the rest, the
// frames with a score that are no frame); one for the treat (its looks, its columns cut at the
// edges, beside the runner, an obstacle and the digits, in the flash, the frames with a treat that
// are no frame); one for the beat and the wheel (the colours gameColours() gives a column of the
// runner, of the score, of the rest, a beat 0 to 250 ms ago and at places on the wheel); and a table
// of texts that are no frame.
//
//   clang++ -std=c++11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
//       -o /tmp/game_frame_test test/game_frame_test.cpp && /tmp/game_frame_test
//
// The sanitizers are part of the test: a column off the face must be cut, not written somewhere.
// Not covered here: the sparkle, the LEDs, the serial link, the sign's clock (the emulator's
// tests/game_test.py runs those on the real image).

#undef NDEBUG

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define uint uint8_t // the sketch's uint is 8 bits
#define PROGMEM
#define pgm_read_byte(address) (*(address))
#define pgm_read_byte_near(address) (*(address))

#include "../Icons.h"
#include "../GameFrame.h"

// The game's contract (artifacts/led-sign/game-contract.md), which the app decides collisions by:
// the rows each column of the runner lights on the ground, row 1 the bottom row, left to right ...
static const char* const NOTE_RUNNER[GAME_RUNNER_COLS] = {
        "1", "12", "123", "134", "124", "234", "134", "145", "1256", "2367", "347", "47", "4"};
// ... and how many rows each column of the obstacles a, b, c is high
static const std::vector<int> NOTE_HEIGHTS[3] = {{1, 2, 1}, {3, 3}, {1, 2, 3, 2, 1}};
// ... and the moving stars at 0: the column and the row of each
static const int NOTE_STARS[8][2] = {{2, 7}, {9, 5}, {19, 6}, {25, 4}, {33, 7}, {40, 5}, {47, 4}, {55, 6}};
// ... and the score's font, each digit's rows from the top row (row 7) down, '#' lit. A digit's
// right column is 3 columns and a dark one left of the one after it; the last one's is column 59.
static const char* const NOTE_DIGITS[10][4] = {
        {"###", "#.#", "#.#", "###"}, {".#.", "##.", ".#.", "###"}, {"##.", "..#", ".#.", "###"},
        {"###", ".##", "..#", "###"}, {"#.#", "#.#", "###", "..#"}, {"###", "##.", "..#", "##."},
        {"#..", "###", "#.#", "###"}, {"###", "..#", ".#.", ".#."}, {"###", "#.#", "###", "###"},
        {"###", "#.#", "###", "..#"}};
static const int NOTE_DIGIT_ROWS = sizeof NOTE_DIGITS[0] / sizeof NOTE_DIGITS[0][0];
// ... and the treat: 3 columns from its own, rows 5 to 7 only. Each of its looks, a column at a time
// from the left: its rows from row 7 down to row 5, '#' lit, then its colour, P purple or G the glint.
// A look lasts NOTE_TWINKLE_MS of the sign's clock; after the last the first comes again.
static const int NOTE_TWINKLE_MS = 100;
static const char* const NOTE_TREAT[][3] = {
        {".#.P", "###P", ".#.P"}, {".#.P", "###G", ".#.P"}, {"#.#G", ".#.G", "#.#G"},
        {".#.P", "###G", ".#.P"}, {".#.P", "###P", ".#.P"}, {".#.P", "###P", ".#.P"}};
static const int NOTE_LOOKS = sizeof NOTE_TREAT / sizeof NOTE_TREAT[0];
// ... and the colours as red, green, blue: the digits 0 to 7, the treat's purple and its glint,
// the moving stars'
static const int NOTE_PALETTE[][3] = {{0, 0, 0},     {127, 127, 127}, {255, 0, 0}, {255, 31, 0},
                                      {255, 127, 0}, {0, 255, 0},     {0, 0, 255}, {255, 0, 127},
                                      {127, 0, 255}, {255, 127, 255}, {15, 15, 15}};
// ... and the score's colour: the marquee's wheel (three parts of 256 steps through the gamma
// table), a step every 18 ms of the sign's clock; and the flash on the beat: white, back to the
// colour over 250 ms in a straight line, rounded down
static const unsigned long NOTE_WHEEL_STEP_MS = 18;

static void noteWheel(unsigned long now, int rgb[3]) {
    const int k = (int) (now / NOTE_WHEEL_STEP_MS % 768), step = k % 256;
    const int up = gamma[step], down = gamma[255 - step];
    const int sectors[3][3] = {{up, down, 0}, {down, 0, up}, {0, up, down}};
    memcpy(rgb, sectors[k / 256], sizeof sectors[0]);
}

static int noteFade(int value, unsigned long sinceBeat) {
    return sinceBeat >= 250 ? value : (int) ((255 * (250 - sinceBeat) + value * sinceBeat) / 250);
}

struct Thing {
    char kind; // a b c
    int col;
};

struct Case {
    int rise, pose, runnerCol, runnerColour, obstacleColour, effect;
    std::vector<Thing> obstacles;
    int stars;         // how far the moving stars are moved; none below 0
    std::string score; // the score's digits as written; no score when empty
    bool hasTreat;     // the treat, and its column
    int treat;
    unsigned long now; // the sign's clock when the frame is read: the treat's look

    Case() : Case(0, 0, 0, 0, 0, 0, {}, -1, "") {}
    Case(int rise, int pose, int runnerCol, int runnerColour, int obstacleColour, int effect,
         std::vector<Thing> obstacles, int stars, std::string score, bool hasTreat = false, int treat = 0)
        : rise(rise), pose(pose), runnerCol(runnerCol), runnerColour(runnerColour), obstacleColour(obstacleColour),
          effect(effect), obstacles(obstacles), stars(stars), score(score), hasTreat(hasTreat), treat(treat), now(0) {}
};

static int checks = 0;

// The picture of a case, LED by LED. The colours are GameFrame.h's names for them: the digits', the
// treat's two, the stars', and the runner's and the score's of the moment (GAME_RUNNER_LIT,
// GAME_SCORE_LIT, whose values the table of the beat and the wheel holds to the contract)
static void expected(const Case& k, uint8_t rows[GAME_COLS], uint8_t colour[GAME_COLS]) {
    const int look = (int) (k.now / NOTE_TWINKLE_MS % NOTE_LOOKS);
    for (int c = 0; c < GAME_COLS; c++) {
        rows[c] = 0;
        colour[c] = 0;
        // From the weakest colour to the strongest: the runner, the score, the treat, the obstacles
        const int i = c - k.runnerCol;
        if (i >= 0 && i < GAME_RUNNER_COLS) {
            for (const char* r = NOTE_RUNNER[i]; *r; r++) {
                if (*r - '0' + k.rise <= 7) {
                    rows[c] |= 1 << (*r - '0' + k.rise);
                    colour[c] = k.runnerColour ? GAME_RUNNER_LIT : 0; // (colour 0: not drawn, not flashed)
                }
            }
        }
        const int digits = (int) k.score.size();
        for (int d = 0; d < digits; d++) {
            const int j = c - (GAME_COLS - 3 - 4 * (digits - 1 - d));
            for (int row = 0; j >= 0 && j < 3 && row < NOTE_DIGIT_ROWS; row++) {
                if (NOTE_DIGITS[k.score[d] - '0'][row][j] == '#') {
                    rows[c] |= 1 << (7 - row);
                    colour[c] = GAME_SCORE_LIT;
                }
            }
        }
        const int j = c - k.treat;
        if (k.hasTreat && j >= 0 && j < 3) {
            const char* column = NOTE_TREAT[look][j];
            for (int row = 0; row < 3; row++) {
                if (column[row] == '#') {
                    rows[c] |= 1 << (7 - row);
                    colour[c] = column[3] == 'G' ? GAME_TREAT_GLINT : GAME_TREAT_COLOUR;
                }
            }
        }
        for (const Thing& t : k.obstacles) {
            const std::vector<int>& heights = NOTE_HEIGHTS[t.kind - 'a'];
            const int o = c - t.col;
            if (o >= 0 && o < (int) heights.size()) {
                for (int row = 1; row <= heights[o]; row++) {
                    rows[c] |= 1 << row;
                }
                colour[c] = k.obstacleColour;
            }
        }
        if (k.effect == GAME_EFFECT_FLASH) {
            if (!colour[c]) rows[c] = 0; // colour 0 draws nothing: the negative has no hole there
            rows[c] = ~rows[c] & 0xFE;
            colour[c] = k.obstacleColour;
        }
    }
    // A star only where the play has no row (whatever its colour), and none in a flash
    for (const int* star : NOTE_STARS) {
        const int c = ((star[0] - k.stars) % GAME_COLS + GAME_COLS) % GAME_COLS;
        if (k.stars >= 0 && k.effect != GAME_EFFECT_FLASH && !rows[c]) {
            rows[c] = 1 << star[1];
            colour[c] = GAME_STAR_COLOUR;
        }
    }
}

// The case as a frame's text. `order` shuffles the items; without `spelled`, what is as the sign
// assumes it (runner's column 2, colours 62, effect 0) is left out.
static std::string text(const Case& k, unsigned order, bool spelled) {
    std::vector<std::string> items;
    for (const Thing& t : k.obstacles) {
        items.push_back(std::string(1, t.kind) + std::to_string(t.col));
    }
    if (spelled || k.runnerCol != GAME_RUNNER_COL) items.push_back("r" + std::to_string(k.runnerCol));
    if (spelled || k.runnerColour != GAME_RUNNER_COLOUR || k.obstacleColour != GAME_OBSTACLE_COLOUR) {
        items.push_back("p" + std::to_string(k.runnerColour) + std::to_string(k.obstacleColour));
    }
    if (spelled || k.effect) items.push_back("e" + std::to_string(k.effect));
    if (k.stars >= 0) items.push_back("s" + std::to_string(k.stars));
    if (!k.score.empty()) items.push_back("n" + k.score);
    if (k.hasTreat) items.push_back("t" + std::to_string(k.treat));
    std::string out = std::to_string(k.rise) + std::to_string(k.pose);
    while (!items.empty()) {
        const size_t pick = order % items.size();
        order = order / items.size() + 7;
        out += items[pick];
        items.erase(items.begin() + pick);
    }
    return out;
}

// The frame `s`, read at the sign's clock k.now, is drawn as the picture of `k` worked out here
static void drawnAs(const Case& k, const std::string& s) {
    uint8_t rows[GAME_COLS], colour[GAME_COLS];
    expected(k, rows, colour);
    GamePicture pic;
    memset(&pic, 0xA5, sizeof pic);
    const bool good = gameRender(s.c_str(), DREAMSTATE.data, k.now, pic);
    checks++;
    if (!good || memcmp(pic.rows, rows, GAME_COLS) || memcmp(pic.colour, colour, GAME_COLS) || pic.effect != k.effect
        || pic.runnerColour != k.runnerColour) {
        printf("FAILED: \"%s\" at %lu ms %s\n", s.c_str(), k.now, good ? "is not the picture worked out here" : "is refused");
        for (int c = 0; good && c < GAME_COLS; c++) {
            if (pic.rows[c] != rows[c] || pic.colour[c] != colour[c]) {
                printf("  column %d: rows %02x colour %d, expected rows %02x colour %d\n", c, pic.rows[c],
                       pic.colour[c], rows[c], colour[c]);
            }
        }
        exit(1);
    }
}

static void drawnAsWorkedOut(const Case& k, unsigned order = 0, bool spelled = false) {
    drawnAs(k, text(k, order, spelled));
}

static bool refused(const char* frame) {
    GamePicture pic;
    checks++;
    return !gameRender(frame, DREAMSTATE.data, 0, pic);
}

// THE SCORE (item n): one table. A row is a frame's text and the case it is, whose picture
// expected() works out from the contract's font and columns; a row that is not good is a bad frame.
struct ItemRow {
    const char* frame;
    bool good;
    Case k; // rise, pose, runner's column, runner's colour, obstacles' colour, effect, obstacles, stars, score,
            // treat or not, its column
};
static const ItemRow SCORE[] = {
        // each digit's picture alone at columns 57 to 59 (a rise of 7: nothing of the runner on the face)
        {"70n0", true, {7, 0, 2, 6, 2, 0, {}, -1, "0"}},
        {"70n1", true, {7, 0, 2, 6, 2, 0, {}, -1, "1"}},
        {"70n2", true, {7, 0, 2, 6, 2, 0, {}, -1, "2"}},
        {"70n3", true, {7, 0, 2, 6, 2, 0, {}, -1, "3"}},
        {"70n4", true, {7, 0, 2, 6, 2, 0, {}, -1, "4"}},
        {"70n5", true, {7, 0, 2, 6, 2, 0, {}, -1, "5"}},
        {"70n6", true, {7, 0, 2, 6, 2, 0, {}, -1, "6"}},
        {"70n7", true, {7, 0, 2, 6, 2, 0, {}, -1, "7"}},
        {"70n8", true, {7, 0, 2, 6, 2, 0, {}, -1, "8"}},
        {"70n9", true, {7, 0, 2, 6, 2, 0, {}, -1, "9"}},
        // two digits at 53 to 55 and 57 to 59, three from 49; as written (007 is three digits)
        {"70n42", true, {7, 0, 2, 6, 2, 0, {}, -1, "42"}},
        {"70n10", true, {7, 0, 2, 6, 2, 0, {}, -1, "10"}},
        {"70n999", true, {7, 0, 2, 6, 2, 0, {}, -1, "999"}},
        {"70n100", true, {7, 0, 2, 6, 2, 0, {}, -1, "100"}},
        {"70n007", true, {7, 0, 2, 6, 2, 0, {}, -1, "007"}},
        {"70n00", true, {7, 0, 2, 6, 2, 0, {}, -1, "00"}},
        // anywhere after the first two characters, beside the play
        {"00n5a20", true, {0, 0, 2, 6, 2, 0, {{'a', 20}}, -1, "5"}},
        {"00a20n5b40", true, {0, 0, 2, 6, 2, 0, {{'a', 20}, {'b', 40}}, -1, "5"}},
        {"11b30c45e0r5p34n678", true, {1, 1, 5, 3, 4, 0, {{'b', 30}, {'c', 45}}, -1, "678"}},
        // an obstacle under a digit: its rows and the digit's in one column, in the obstacles' colour
        {"00a57n7", true, {0, 0, 2, 6, 2, 0, {{'a', 57}}, -1, "7"}},
        {"00p34b56n85", true, {0, 0, 2, 3, 4, 0, {{'b', 56}}, -1, "85"}},
        {"00c48n123", true, {0, 0, 2, 6, 2, 0, {{'c', 48}}, -1, "123"}},
        // the runner under the digits: its rows stay lit, in the score's colour in a digit's column
        {"30r47n123", true, {3, 0, 47, 6, 2, 0, {}, -1, "123"}},
        {"20r46p51n88", true, {2, 0, 46, 5, 1, 0, {}, -1, "88"}},
        // the moving stars: none in a digit's column (at s3 one would be at 59), one between two digits (52)
        {"70n999s3", true, {7, 0, 2, 6, 2, 0, {}, 3, "999"}},
        {"70n42s0", true, {7, 0, 2, 6, 2, 0, {}, 0, "42"}},
        // the obstacles' colour 0 does not darken the digits (theirs is the wheel's), but a digit over
        // an obstacle in colour 0 is dark
        {"70p20n58s3", true, {7, 0, 2, 2, 0, 0, {}, 3, "58"}},
        {"00p60n9a20", true, {0, 0, 2, 6, 0, 0, {{'a', 20}}, -1, "9"}},
        {"00p60n9a56", true, {0, 0, 2, 6, 0, 0, {{'a', 56}}, -1, "9"}},
        // the flash: the digits are part of the play, dark in the negative
        {"00e2n36", true, {0, 0, 2, 6, 2, 2, {}, -1, "36"}},
        {"00e2p35n9a57", true, {0, 0, 2, 3, 5, 2, {{'a', 57}}, -1, "9"}},
        {"30e2p02n147s12", true, {3, 0, 2, 0, 2, 2, {}, 12, "147"}},
        {"00e2p30n5", true, {0, 0, 2, 3, 0, 2, {}, -1, "5"}},
        // the sparkle's stars are GameDisplay.h's: here its frame is the picture without them
        {"00e1n7", true, {0, 0, 2, 6, 2, 1, {}, -1, "7"}},
        // the longest frame
        {"71a-99b-99c-99a-99r-99p77e2s59n999", true,
         {7, 1, -99, 7, 7, 2, {{'a', -99}, {'b', -99}, {'c', -99}, {'a', -99}}, 59, "999"}},
        // frames with a score that are no frame
        {"00n", false, {}},          {"00n-1", false, {}},     {"00n1234", false, {}},  {"00n0000", false, {}},
        {"00n99999", false, {}},     {"00n12n3", false, {}},   {"00n1n1", false, {}},   {"00n5n5", false, {}},
        {"00na5", false, {}},        {"00n+5", false, {}},     {"00n 5", false, {}},    {"00N5", false, {}},
        {"00a5n", false, {}},        {"30a17b45s12n", false, {}}, {"00n1.5", false, {}}, {"00n5,", false, {}},
        {"00n5\"", false, {}},       {"n500", false, {}},      {"0n5", false, {}},      {"00s5n1s5", false, {}},
};

// THE TREAT (item t): one table, as the score's. Every good row is checked at every look of the
// treat (the sign's clock at each of them) and with its items in other orders.
static const ItemRow TREAT[] = {
        // alone (a rise of 7: nothing of the runner on the face), in the middle, at the edges, and at
        // 44, the last column the app sends it at (its columns 44 to 46)
        {"70t20", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, 20}},
        {"70t0", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, 0}},
        {"70t57", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, 57}},
        {"70t44", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, 44}},
        {"70t05", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, 5}},
        // cut at the edges: what falls off the face is not drawn
        {"70t-1", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, -1}},
        {"70t-2", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, -2}},
        {"70t-3", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, -3}},
        {"70t58", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, 58}},
        {"70t59", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, 59}},
        {"70t60", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, 60}},
        {"70t-99", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, -99}},
        {"70t99", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, 99}},
        {"70t-0", true, {7, 0, 2, 6, 2, 0, {}, -1, "", true, 0}},
        // in the runner's columns: its rows stay lit, in the treat's colour; also a runner in colour 0
        {"00t8", true, {0, 0, 2, 6, 2, 0, {}, -1, "", true, 8}},
        {"30t10", true, {3, 0, 2, 6, 2, 0, {}, -1, "", true, 10}},
        {"00t1", true, {0, 0, 2, 6, 2, 0, {}, -1, "", true, 1}},
        {"71r40t42", true, {7, 1, 40, 6, 2, 0, {}, -1, "", true, 42}},
        {"00p02t8", true, {0, 0, 2, 0, 2, 0, {}, -1, "", true, 8}},
        // over an obstacle's column: the obstacles' colour wins, both their rows lit (colour 0: dark)
        {"00t20a21", true, {0, 0, 2, 6, 2, 0, {{'a', 21}}, -1, "", true, 20}},
        {"00b19t20", true, {0, 0, 2, 6, 2, 0, {{'b', 19}}, -1, "", true, 20}},
        {"00c18t20", true, {0, 0, 2, 6, 2, 0, {{'c', 18}}, -1, "", true, 20}},
        {"00p35t20a20", true, {0, 0, 2, 3, 5, 0, {{'a', 20}}, -1, "", true, 20}},
        {"00p60t20a22", true, {0, 0, 2, 6, 0, 0, {{'a', 22}}, -1, "", true, 20}},
        {"00t9b9", true, {0, 0, 2, 6, 2, 0, {{'b', 9}}, -1, "", true, 9}},
        // over the digits (the app never sends that): the treat's colour, both their rows lit
        {"70t56n9", true, {7, 0, 2, 6, 2, 0, {}, -1, "9", true, 56}},
        {"70n123t48", true, {7, 0, 2, 6, 2, 0, {}, -1, "123", true, 48}},
        // the flash: the treat is part of the play, dark in the negative
        {"00e2t20", true, {0, 0, 2, 6, 2, 2, {}, -1, "", true, 20}},
        {"00e2t8", true, {0, 0, 2, 6, 2, 2, {}, -1, "", true, 8}},
        {"00e2p02t8", true, {0, 0, 2, 0, 2, 2, {}, -1, "", true, 8}},
        {"00e2t20a21", true, {0, 0, 2, 6, 2, 2, {{'a', 21}}, -1, "", true, 20}},
        {"30e2p35t-1s12", true, {3, 0, 2, 3, 5, 2, {}, 12, "", true, -1}},
        // the moving stars: none in the treat's columns (at s0 one would be at 2, at s2 one at 0)
        {"70t0s0", true, {7, 0, 2, 6, 2, 0, {}, 0, "", true, 0}},
        {"70t17s0", true, {7, 0, 2, 6, 2, 0, {}, 0, "", true, 17}},
        {"70t-2s2", true, {7, 0, 2, 6, 2, 0, {}, 2, "", true, -2}},
        // the sparkle's stars are GameDisplay.h's: here its frame is the picture without them
        {"00e1t20", true, {0, 0, 2, 6, 2, 1, {}, -1, "", true, 20}},
        // anywhere after the first two characters; the longest frame there is
        {"11b30c45e0r5p34n678t20", true, {1, 1, 5, 3, 4, 0, {{'b', 30}, {'c', 45}}, -1, "678", true, 20}},
        {"00t20a40n5", true, {0, 0, 2, 6, 2, 0, {{'a', 40}}, -1, "5", true, 20}},
        {"71a-99b-99c-99a-99r-99p77e2s59n999t-99", true,
         {7, 1, -99, 7, 7, 2, {{'a', -99}, {'b', -99}, {'c', -99}, {'a', -99}}, 59, "999", true, -99}},
        // frames with a treat that are no frame
        {"00t", false, {}},     {"00t-", false, {}},    {"00t--1", false, {}},  {"00t+1", false, {}},
        {"00t100", false, {}},  {"00t-100", false, {}}, {"00t999", false, {}},  {"00t1t2", false, {}},
        {"00t5t5", false, {}},  {"00T5", false, {}},    {"00t 5", false, {}},   {"00ta", false, {}},
        {"00tt5", false, {}},   {"00t1.5", false, {}},  {"00t5,", false, {}},   {"t5", false, {}},
        {"0t5", false, {}},     {"00a5t", false, {}},   {"00t5n", false, {}},   {"00t-a", false, {}},
        {"00t5\"", false, {}},  {"00n5t5t5", false, {}},
};

static void itemTable(const ItemRow* rows, size_t n) {
    for (const ItemRow* row = rows; row < rows + n; row++) {
        if (!row->good) {
            if (!refused(row->frame)) {
                printf("FAILED: \"%s\" is taken for a frame\n", row->frame);
                exit(1);
            }
            continue;
        }
        for (int look = 0; look < NOTE_LOOKS; look++) {
            Case k = row->k;
            k.now = (unsigned long) look * NOTE_TWINKLE_MS + (unsigned long) look % 2 * (NOTE_TWINKLE_MS - 1);
            drawnAs(k, row->frame);
            for (unsigned order = 0; order < 24; order++) { // (the same with the items in other orders)
                drawnAsWorkedOut(k, order * 37, order % 2);
            }
        }
    }
}

// THE BEAT AND THE WHEEL: one table. A row is a frame, the sign's clock when it is drawn, how many ms
// ago the last beat was, one of its columns and the colour gameColours() must give that column then,
// as the contract works it out.
struct ColourRow {
    const char* frame;
    unsigned long now, sinceBeat;
    int col;
    int rgb[3];
};
static const unsigned long LONG_AGO = 5000000; // (a beat long ago, or none since the sign started)
static const ColourRow COLOURS[] = {
        // the runner alone in a column (column 2 of "00"), blue: white at the beat, back to blue over 250 ms
        {"00", 0, 0, 2, {255, 255, 255}},
        {"00", 0, 1, 2, {253, 253, 255}},
        {"00", 0, 100, 2, {153, 153, 255}},
        {"00", 0, 249, 2, {1, 1, 255}},
        {"00", 0, 250, 2, {0, 0, 255}},
        {"00", 0, LONG_AGO, 2, {0, 0, 255}},
        // in other colours of p: orange, white, and 0, which is dark and stays dark at the beat
        {"00p34", 0, 100, 2, {255, 165, 153}},
        {"00p14", 0, 100, 2, {203, 203, 203}},
        {"00p02", 0, 0, 2, {0, 0, 0}},
        {"00p02", 0, 100, 2, {0, 0, 0}},
        // the score alone in a column (column 57 of "70n8"), at step 0 of the wheel: green
        {"70n8", 0, 0, 57, {255, 255, 255}},
        {"70n8", 0, 100, 57, {153, 255, 153}},
        {"70n8", 0, 250, 57, {0, 255, 0}},
        // the runner's rows in a digit's column are the score's colour, flashing with it
        {"30r47n123", 0, 0, 49, {255, 255, 255}},
        {"30r47n123", 0, LONG_AGO, 49, {0, 255, 0}},
        // the wheel, a step every 18 ms of the sign's clock: three parts of 256 steps, round in 13.824 s
        {"70n8", 17, LONG_AGO, 57, {0, 255, 0}},
        {"70n8", 18, LONG_AGO, 57, {0, 252, 0}},
        {"70n8", 2304, LONG_AGO, 57, {37, 36, 0}},
        {"70n8", 4590, LONG_AGO, 57, {255, 0, 0}},
        {"70n8", 4608, LONG_AGO, 57, {255, 0, 0}},
        {"70n8", 6912, LONG_AGO, 57, {36, 0, 37}},
        {"70n8", 9198, LONG_AGO, 57, {0, 0, 255}},
        {"70n8", 9216, LONG_AGO, 57, {0, 0, 255}},
        {"70n8", 11520, LONG_AGO, 57, {0, 37, 36}},
        {"70n8", 13806, LONG_AGO, 57, {0, 255, 0}},
        {"70n8", 13824, LONG_AGO, 57, {0, 255, 0}},
        {"70n8", 13824 + 2304, 100, 57, {167, 167, 153}},
        {"70n8", 4294967295UL, LONG_AGO, 57, {50, 26, 0}}, // (the sign's clock at its last ms)
        // an obstacle's column does not flash: under a digit, under the runner, in colour 0
        {"70a57n8", 0, 0, 57, {255, 0, 0}},
        {"70a57n8", 0, 0, 59, {255, 0, 0}},
        {"00a4", 0, 0, 4, {255, 0, 0}},
        {"00a4", 0, 0, 3, {255, 255, 255}},
        {"00p35a4", 0, 100, 5, {0, 255, 0}},
        {"70p60a57n8", 0, 0, 57, {0, 0, 0}},
        // the treat's columns do not flash, the runner's rows in them neither (at 0 ms all purple,
        // at 100 ms its centre the glint, at 200 ms the bright x)
        {"00t8", 0, 0, 8, {127, 0, 255}},
        {"00t8", 0, 0, 9, {127, 0, 255}},
        {"00t8", 0, 0, 7, {255, 255, 255}},
        {"00t8", 100, 0, 8, {127, 0, 255}},
        {"00t8", 100, 0, 9, {255, 127, 255}},
        {"00t8", 200, 0, 8, {255, 127, 255}},
        // nothing flashes in the flash e2: every column is in the obstacles' colour
        {"00e2n42", 0, 0, 2, {255, 0, 0}},
        {"00e2n42", 0, 0, 57, {255, 0, 0}},
        {"00e2p35t20", 0, 0, 30, {0, 255, 0}},
        // the moving stars do not flash
        {"70s0", 0, 0, 2, {15, 15, 15}},
};

static void colourTable() {
    for (const ColourRow& row : COLOURS) {
        GamePicture pic;
        uint8_t rgb[GAME_FRAME_COLOURS * 3];
        assert(gameRender(row.frame, DREAMSTATE.data, row.now, pic));
        memset(rgb, 0xA5, sizeof rgb);
        gameColours(pic, row.now, row.sinceBeat, rgb);
        const uint8_t* got = rgb + pic.colour[row.col] * 3;
        checks++;
        if (!pic.rows[row.col] || got[0] != row.rgb[0] || got[1] != row.rgb[1] || got[2] != row.rgb[2]) {
            printf("FAILED: \"%s\" at %lu ms, a beat %lu ms ago: column %d is %d %d %d (rows %02x), not %d %d %d\n",
                   row.frame, row.now, row.sinceBeat, row.col, got[0], got[1], got[2], pic.rows[row.col],
                   row.rgb[0], row.rgb[1], row.rgb[2]);
            exit(1);
        }
    }
    // ... and the same over the whole of them: the score at every step of a turn and the fade at
    // every ms of it, for the score and the runner in every colour of p
    for (unsigned long step = 0; step <= 2 * 768; step++) {
        for (unsigned long since : {0UL, 1UL, 99UL, 100UL, 125UL, 200UL, 249UL, 250UL, LONG_AGO}) {
            for (int runner = 0; runner < GAME_COLOURS; runner++) {
                GamePicture pic;
                uint8_t rgb[GAME_FRAME_COLOURS * 3];
                const std::string frame = "00p" + std::to_string(runner) + "2n8";
                const unsigned long now = step * NOTE_WHEEL_STEP_MS + step % NOTE_WHEEL_STEP_MS;
                assert(gameRender(frame.c_str(), DREAMSTATE.data, now, pic));
                gameColours(pic, now, since, rgb);
                int score[3];
                noteWheel(now, score);
                for (int i = 0; i < 3; i++) {
                    assert(rgb[pic.colour[57] * 3 + i] == noteFade(score[i], since));
                    assert(!runner || rgb[pic.colour[2] * 3 + i] == noteFade(NOTE_PALETTE[runner][i], since));
                    assert(runner || rgb[pic.colour[2] * 3 + i] == 0);
                }
                checks++;
            }
        }
    }
    // The fade itself (MarqueeColour.h, the marquee's too): every value at every ms of it, and after
    for (int value = 0; value < 256; value++) {
        for (unsigned long since = 0; since <= 260; since++) {
            assert(beatFade((uint8_t) value, since) == noteFade(value, since));
        }
        assert(beatFade((uint8_t) value, 0xFFFFFFFFUL) == value);
        checks++;
    }
    // The palette (the treat's colours and the stars' too) is the contract's, every value a run of
    // one-bits from bit 0 up (so that it reaches the LEDs as written)
    for (int c = 0; c < GAME_PALETTE; c++) {
        GamePicture pic;
        uint8_t rgb[GAME_FRAME_COLOURS * 3];
        assert(gameRender("70", DREAMSTATE.data, 0, pic));
        gameColours(pic, 0, LONG_AGO, rgb);
        for (int i = 0; i < 3; i++) {
            assert(rgb[c * 3 + i] == NOTE_PALETTE[c][i] && !(NOTE_PALETTE[c][i] & (NOTE_PALETTE[c][i] + 1)));
        }
        checks++;
    }
    // A whole turn of the wheel is 10 to 20 s of the sign's clock (about 15 s of play)
    assert(NOTE_WHEEL_STEP_MS * 768 >= 10000 && NOTE_WHEEL_STEP_MS * 768 <= 20000);
}

static unsigned seed = 12345;

static int pick(int from, int to) { // from..to
    seed = seed * 1103515245u + 12345u;
    return from + (int) ((seed >> 8) % (unsigned) (to - from + 1));
}

int main() {
    // Icons.h is what the contract's table says: both of the runner's pictures
    assert(DREAMSTATE.width == GAME_RUNNER_COLS);
    for (int pose = 0; pose < 2; pose++) {
        for (int i = 0; i < GAME_RUNNER_COLS; i++) {
            uint8_t rows = 0;
            for (const char* r = NOTE_RUNNER[i]; *r; r++) rows |= 1 << (*r - '0');
            assert(DREAMSTATE.data[pose * GAME_RUNNER_COLS + i] == rows);
            checks++;
        }
    }

    // The runner: every rise, both pictures, every column it can be given
    for (int rise = 0; rise <= 7; rise++) {
        for (int pose = 0; pose <= 1; pose++) {
            for (int col = -99; col <= 99; col++) {
                drawnAsWorkedOut(Case{rise, pose, col, 6, 2, 0, {{'b', 30}}, -1, ""}); // (no 'p': blue, red)
            }
        }
    }
    // The obstacles: every shape at every column, plain and in the flash
    for (char kind = 'a'; kind <= 'c'; kind++) {
        for (int col = -99; col <= 99; col++) {
            for (int effect = 0; effect <= 2; effect++) {
                drawnAsWorkedOut(Case{3, 0, 2, 4, 6, effect, {{kind, col}}, -1, ""});
            }
        }
    }
    // The moving stars: at every place the picture at 0 moved that many columns to the left and
    // around, 8 LEDs in rows 4 to 7 when nothing of the play is on the face (a rise of 7)
    for (int stars = 0; stars < GAME_COLS; stars++) {
        GamePicture at0, moved;
        const std::string s = "70s" + std::to_string(stars);
        assert(gameRender("70s0", DREAMSTATE.data, 0, at0) && gameRender(s.c_str(), DREAMSTATE.data, 0, moved));
        int lit = 0;
        for (int c = 0; c < GAME_COLS; c++) {
            assert(moved.rows[c] == at0.rows[(c + stars) % GAME_COLS]);
            assert(moved.colour[c] == at0.colour[(c + stars) % GAME_COLS]);
            assert(!(moved.rows[c] & 0x0F) && (!moved.rows[c] || moved.colour[c] == GAME_STAR_COLOUR));
            lit += __builtin_popcount(moved.rows[c]);
        }
        assert(lit == 8);
        checks++;
        for (int effect = 0; effect <= 2; effect++) {
            drawnAsWorkedOut(Case{stars % 8, 0, 2, 6, 2, effect, {{'c', stars}, {'b', 59 - stars}}, stars, ""});
        }
    }
    // The score, the treat, and the colours of the beat and the wheel
    itemTable(SCORE, sizeof SCORE / sizeof SCORE[0]);
    itemTable(TREAT, sizeof TREAT / sizeof TREAT[0]);
    colourTable();

    // Mixes: 0 to 4 obstacles, any colours, a score or none, a treat or none, the items in any order,
    // said or left out, at any moment of the treat's twinkle
    for (int n = 0; n < 20000; n++) {
        Case k{pick(0, 7), pick(0, 1), pick(0, 3) ? 2 : pick(-99, 99), pick(0, 7), pick(0, 7), pick(0, 2), {}, -1, ""};
        for (int o = pick(0, GAME_MAX_OBSTACLES); o; o--) {
            k.obstacles.push_back(Thing{(char) pick('a', 'c'), pick(0, 2) ? pick(-8, 64) : pick(-99, 99)});
        }
        for (int d = pick(0, 3); d; d--) {
            k.score += (char) pick('0', '9'); // a score of none to three digits
        }
        k.hasTreat = pick(0, 1);
        k.treat = pick(0, 2) ? pick(-4, 62) : pick(-99, 99);
        k.now = (unsigned long) pick(0, 1 << 20) * (unsigned long) pick(1, 4000);
        const unsigned order = (unsigned) pick(0, 5000);
        const bool spelled = pick(0, 1);
        drawnAsWorkedOut(k, order, spelled);
        // ... and with the moving stars: every column the play has a row in is as without them
        GamePicture plain, starry;
        k.stars = -1;
        assert(gameRender(text(k, order, spelled).c_str(), DREAMSTATE.data, k.now, plain));
        k.stars = pick(0, GAME_COLS - 1);
        drawnAsWorkedOut(k, order, spelled);
        assert(gameRender(text(k, order, spelled).c_str(), DREAMSTATE.data, k.now, starry));
        for (int c = 0; c < GAME_COLS; c++) {
            assert(!plain.rows[c] || (starry.rows[c] == plain.rows[c] && starry.colour[c] == plain.colour[c]));
        }
    }
    // A column may be written with a leading zero or as -0
    {
        GamePicture a, b;
        assert(gameRender("00a05r-0t-0", DREAMSTATE.data, 0, a) && gameRender("00a5r0t0", DREAMSTATE.data, 0, b));
        assert(!memcmp(&a, &b, sizeof a));
        checks++;
    }

    // Picture 1 is the runner's second picture. (In Icons.h the two are the same today, so a
    // runner made up here shows it: its first picture is the bottom row, its second the top row.)
    {
        uint8_t two[2 * GAME_RUNNER_COLS];
        memset(two, 0x02, GAME_RUNNER_COLS);
        memset(two + GAME_RUNNER_COLS, 0x80, GAME_RUNNER_COLS);
        GamePicture a, b;
        assert(gameRender("00", two, 0, a) && gameRender("01", two, 0, b));
        for (int c = 0; c < GAME_COLS; c++) {
            const bool runner = c >= GAME_RUNNER_COL && c < GAME_RUNNER_COL + GAME_RUNNER_COLS;
            assert(a.rows[c] == (runner ? 0x02 : 0) && b.rows[c] == (runner ? 0x80 : 0));
        }
        checks++;
    }

    // Texts that are no frame: every one is refused
    static const char* const BAD[] = {
            "", "0", "3", "80", "90", "02", "09", "a0", "-00", " 00", "00 ", "00\n",   // rise and picture
            "00x", "00A1", "00d1", "00R2", "00 a1", "00a1 ", "00,a1", "00a1,b2",       // what is no item
            "00S5", "00u5", "30a17b45s12w3", "00d17a5",                                // (one the sign does not know)
            "00a", "00a-", "00a--1", "00a+1", "00ab1", "00a100", "00a-100", "00a1.5",  // an obstacle's column
            "00a1a2a3a4a5", "00a1b2c3a4b5", "00r1a1a2a3a4a5",                          // a fifth obstacle
            "00r", "00r-", "00r100", "00r1r2", "00r2r2",                               // the runner's column
            "00p", "00p1", "00p8", "00p18", "00p81", "00p123", "00p12p12", "00pab",    // the colours
            "00p89", "00p98", "00p99",                                                 // (8 and up are not p's)
            "00e", "00e3", "00e9", "00e-1", "00e10", "00e0e0", "00e1e2",               // the effect
            "00s", "00s60", "00s99", "00s-1", "00s100", "00s5s6", "00s5s5", "00sa1",   // the moving stars
            "30a17b45\"", "30a17b45}", "{\"str\":\"00\"}",
    };
    for (const char* bad : BAD) {
        if (!refused(bad)) {
            printf("FAILED: \"%s\" is taken for a frame\n", bad);
            return 1;
        }
    }
    {
        GamePicture pic;
        assert(!gameRender(nullptr, DREAMSTATE.data, 0, pic)); // a message without a text
        checks++;
    }

    printf("game_frame_test: %d checks passed\n", checks);
    return 0;
}
