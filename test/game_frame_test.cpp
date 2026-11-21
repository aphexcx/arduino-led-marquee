// Host test for GameFrame.h: the text of a game frame and the picture it makes. The real
// gameRender() against a picture worked out here LED by LED, for every rise, every column a thing
// can be given (-99 to 99), every place of the moving stars and mixes of items in any order; one
// table for the score (its font as the contract draws it, its columns, beside the rest, the frames
// with a score that are no frame); and a table of texts that are no frame.
//
//   clang++ -std=c++11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
//       -o /tmp/game_frame_test test/game_frame_test.cpp && /tmp/game_frame_test
//
// The sanitizers are part of the test: a column off the face must be cut, not written somewhere.
// Not covered here: the colours' values, the sparkle, the LEDs, the serial link (the emulator's
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
static const int STAR_COLOUR = 8;
// ... and the score's font, each digit's rows from the top row (row 7) down, '#' lit. A digit's
// right column is 3 columns and a dark one left of the one after it; the last one's is column 59.
static const char* const NOTE_DIGITS[10][4] = {
        {"###", "#.#", "#.#", "###"}, {".#.", "##.", ".#.", "###"}, {"##.", "..#", ".#.", "###"},
        {"###", ".##", "..#", "###"}, {"#.#", "#.#", "###", "..#"}, {"###", "##.", "..#", "##."},
        {"#..", "###", "#.#", "###"}, {"###", "..#", ".#.", ".#."}, {"###", "#.#", "###", "###"},
        {"###", "#.#", "###", "..#"}};
static const int NOTE_DIGIT_ROWS = sizeof NOTE_DIGITS[0] / sizeof NOTE_DIGITS[0][0];

struct Thing {
    char kind; // a b c
    int col;
};

struct Case {
    int rise, pose, runnerCol, runnerColour, obstacleColour, effect;
    std::vector<Thing> obstacles;
    int stars;         // how far the moving stars are moved; none below 0
    std::string score; // the score's digits as written; no score when empty
};

static int checks = 0;

// The picture of a case, LED by LED
static void expected(const Case& k, uint8_t rows[GAME_COLS], uint8_t colour[GAME_COLS]) {
    for (int c = 0; c < GAME_COLS; c++) {
        rows[c] = 0;
        colour[c] = 0;
        const int i = c - k.runnerCol;
        if (i >= 0 && i < GAME_RUNNER_COLS) {
            for (const char* r = NOTE_RUNNER[i]; *r; r++) {
                if (*r - '0' + k.rise <= 7) {
                    rows[c] |= 1 << (*r - '0' + k.rise);
                    colour[c] = k.runnerColour;
                }
            }
        }
        for (const Thing& t : k.obstacles) {
            const std::vector<int>& heights = NOTE_HEIGHTS[t.kind - 'a'];
            const int j = c - t.col;
            if (j >= 0 && j < (int) heights.size()) {
                for (int row = 1; row <= heights[j]; row++) {
                    rows[c] |= 1 << row;
                }
                colour[c] = k.obstacleColour;
            }
        }
        // The score: a column a digit lights is in the obstacles' colour, as an obstacle's is
        const int digits = (int) k.score.size();
        for (int d = 0; d < digits; d++) {
            const int j = c - (GAME_COLS - 3 - 4 * (digits - 1 - d));
            for (int row = 0; j >= 0 && j < 3 && row < NOTE_DIGIT_ROWS; row++) {
                if (NOTE_DIGITS[k.score[d] - '0'][row][j] == '#') {
                    rows[c] |= 1 << (7 - row);
                    colour[c] = k.obstacleColour;
                }
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
            colour[c] = STAR_COLOUR;
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
    std::string out = std::to_string(k.rise) + std::to_string(k.pose);
    while (!items.empty()) {
        const size_t pick = order % items.size();
        order = order / items.size() + 7;
        out += items[pick];
        items.erase(items.begin() + pick);
    }
    return out;
}

// The frame `s` is drawn as the picture of `k` worked out here
static void drawnAs(const Case& k, const std::string& s) {
    uint8_t rows[GAME_COLS], colour[GAME_COLS];
    expected(k, rows, colour);
    GamePicture pic;
    memset(&pic, 0xA5, sizeof pic);
    const bool good = gameRender(s.c_str(), DREAMSTATE.data, pic);
    checks++;
    if (!good || memcmp(pic.rows, rows, GAME_COLS) || memcmp(pic.colour, colour, GAME_COLS) || pic.effect != k.effect) {
        printf("FAILED: \"%s\" %s\n", s.c_str(), good ? "is not the picture worked out here" : "is refused");
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

// THE SCORE (item n): one table. A row is a frame's text and the case it is, whose picture
// expected() works out from the contract's font and columns; a row that is not good is a bad frame.
struct ScoreRow {
    const char* frame;
    bool good;
    Case k; // rise, pose, runner's column, runner's colour, obstacles' colour, effect, obstacles, stars, score
};
static const ScoreRow SCORE[] = {
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
        // the runner under the digits: its rows stay lit, in the obstacles' colour in a digit's column
        {"30r47n123", true, {3, 0, 47, 6, 2, 0, {}, -1, "123"}},
        {"20r46p51n88", true, {2, 0, 46, 5, 1, 0, {}, -1, "88"}},
        // the moving stars: none in a digit's column (at s3 one would be at 59), one between two digits (52)
        {"70n999s3", true, {7, 0, 2, 6, 2, 0, {}, 3, "999"}},
        {"70n42s0", true, {7, 0, 2, 6, 2, 0, {}, 0, "42"}},
        // the obstacles' colour 0: the digits are dark, and their columns have no star all the same
        {"70p20n58s3", true, {7, 0, 2, 2, 0, 0, {}, 3, "58"}},
        {"00p60n9a20", true, {0, 0, 2, 6, 0, 0, {{'a', 20}}, -1, "9"}},
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

static void scoreTable() {
    for (int d = 0; d < 10; d++) { // (every column of every digit has a lit row: a digit lights all three)
        for (int j = 0; j < 3; j++) {
            bool lit = false;
            for (int row = 0; row < NOTE_DIGIT_ROWS; row++) lit = lit || NOTE_DIGITS[d][row][j] == '#';
            assert(lit);
        }
    }
    for (const ScoreRow& row : SCORE) {
        if (!row.good) {
            GamePicture pic;
            if (gameRender(row.frame, DREAMSTATE.data, pic)) {
                printf("FAILED: \"%s\" is taken for a frame\n", row.frame);
                exit(1);
            }
            checks++;
            continue;
        }
        drawnAs(row.k, row.frame);
        for (unsigned order = 0; order < 24; order++) { // (the same with the items in other orders)
            drawnAsWorkedOut(row.k, order * 37, order % 2);
        }
    }
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
        assert(gameRender("70s0", DREAMSTATE.data, at0) && gameRender(s.c_str(), DREAMSTATE.data, moved));
        int lit = 0;
        for (int c = 0; c < GAME_COLS; c++) {
            assert(moved.rows[c] == at0.rows[(c + stars) % GAME_COLS]);
            assert(moved.colour[c] == at0.colour[(c + stars) % GAME_COLS]);
            assert(!(moved.rows[c] & 0x0F) && (!moved.rows[c] || moved.colour[c] == STAR_COLOUR));
            lit += __builtin_popcount(moved.rows[c]);
        }
        assert(lit == 8);
        checks++;
        for (int effect = 0; effect <= 2; effect++) {
            drawnAsWorkedOut(Case{stars % 8, 0, 2, 6, 2, effect, {{'c', stars}, {'b', 59 - stars}}, stars, ""});
        }
    }
    // The score
    scoreTable();

    // Mixes: 0 to 4 obstacles, any colours, a score or none, the items in any order, said or left out
    for (int n = 0; n < 20000; n++) {
        Case k{pick(0, 7), pick(0, 1), pick(0, 3) ? 2 : pick(-99, 99), pick(0, 7), pick(0, 7), pick(0, 2), {}, -1, ""};
        for (int o = pick(0, GAME_MAX_OBSTACLES); o; o--) {
            k.obstacles.push_back(Thing{(char) pick('a', 'c'), pick(0, 2) ? pick(-8, 64) : pick(-99, 99)});
        }
        for (int d = pick(0, 3); d; d--) {
            k.score += (char) pick('0', '9'); // a score of none to three digits
        }
        const unsigned order = (unsigned) pick(0, 5000);
        const bool spelled = pick(0, 1);
        drawnAsWorkedOut(k, order, spelled);
        // ... and with the moving stars: every column the play has a row in is as without them
        GamePicture plain, starry;
        k.stars = -1;
        assert(gameRender(text(k, order, spelled).c_str(), DREAMSTATE.data, plain));
        k.stars = pick(0, GAME_COLS - 1);
        drawnAsWorkedOut(k, order, spelled);
        assert(gameRender(text(k, order, spelled).c_str(), DREAMSTATE.data, starry));
        for (int c = 0; c < GAME_COLS; c++) {
            assert(!plain.rows[c] || (starry.rows[c] == plain.rows[c] && starry.colour[c] == plain.colour[c]));
        }
    }
    // A column may be written with a leading zero or as -0
    {
        GamePicture a, b;
        assert(gameRender("00a05r-0", DREAMSTATE.data, a) && gameRender("00a5r0", DREAMSTATE.data, b));
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
        assert(gameRender("00", two, a) && gameRender("01", two, b));
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
            "00S5", "00t5", "30a17b45s12w3", "00d17a5",                                // (one the sign does not know)
            "00a", "00a-", "00a--1", "00a+1", "00ab1", "00a100", "00a-100", "00a1.5",  // an obstacle's column
            "00a1a2a3a4a5", "00a1b2c3a4b5", "00r1a1a2a3a4a5",                          // a fifth obstacle
            "00r", "00r-", "00r100", "00r1r2", "00r2r2",                               // the runner's column
            "00p", "00p1", "00p8", "00p18", "00p81", "00p123", "00p12p12", "00pab",    // the colours
            "00e", "00e3", "00e9", "00e-1", "00e10", "00e0e0", "00e1e2",               // the effect
            "00s", "00s60", "00s99", "00s-1", "00s100", "00s5s6", "00s5s5", "00sa1",   // the moving stars
            "30a17b45\"", "30a17b45}", "{\"str\":\"00\"}",
    };
    for (const char* bad : BAD) {
        GamePicture pic;
        if (gameRender(bad, DREAMSTATE.data, pic)) {
            printf("FAILED: \"%s\" is taken for a frame\n", bad);
            return 1;
        }
        checks++;
    }
    {
        GamePicture pic;
        assert(!gameRender(nullptr, DREAMSTATE.data, pic)); // a message without a text
        checks++;
    }

    printf("game_frame_test: %d checks passed\n", checks);
    return 0;
}
