// Drives the recorder (src/record.c) directly, without the simulator, so a
// test can put exact pictures through the GIF encoder. Reads commands from
// stdin, one per line:
//
//   open <out.gif> <out.jsonl> <scale> <tick_cs> <band 0|1> <gamma>
//   fill <cycle> <row> <first_col> <last_col> <rrggbb>   set LEDs, latch a frame
//   rand <cycle> <seed> <colours> <percent_lit>          random picture, latch a frame
//   close <cycle>
//
// The frames go to the GIF and to the frames dump, so tests/gif_check.py can
// decode the one and compare it with the other.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu.h"

static image_t img;
static uint32_t rng;

static uint32_t next_random(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

int main(void)
{
    char line[1024], gif[400], dump[400];
    ws_frame_info_t info;
    memset(&info, 0, sizeof(info));
    while (fgets(line, sizeof(line), stdin)) {
        unsigned long long cycle;
        int scale, tick, band, row, c0, c1, colours, percent;
        unsigned rgb, seed;
        double gamma;
        if (sscanf(line, "open %399s %399s %d %d %d %lf", gif, dump, &scale, &tick, &band, &gamma) == 6) {
            view_set_gamma(gamma);
            rec_set_layout(band);
            if (rec_gif_open(gif, scale, tick) || rec_events_open(dump, "gif_case", "\"rows\":7,\"cols\":120")) {
                perror("gif_case: open");
                return 2;
            }
        } else if (sscanf(line, "fill %llu %d %d %d %x", &cycle, &row, &c0, &c1, &rgb) == 5) {
            for (int c = c0; c <= c1 && c < COLS; c++) {
                img.px[row][c][0] = rgb >> 16;
                img.px[row][c][1] = rgb >> 8;
                img.px[row][c][2] = rgb;
            }
            info.bits = COLS * 24;
            rec_frame(&img, cycle, &info);
        } else if (sscanf(line, "rand %llu %u %d %d", &cycle, &seed, &colours, &percent) == 4) {
            rng = seed * 2654435761u + 1;
            uint8_t palette[1024][3];
            if (colours > 1024)
                colours = 1024;
            for (int i = 0; i < colours; i++)
                for (int k = 0; k < 3; k++)
                    palette[i][k] = (uint8_t) (next_random() >> 8);
            for (int r = 0; r < ROWS; r++)
                for (int c = 0; c < COLS; c++) {
                    int lit = (int) (next_random() % 100) < percent;
                    const uint8_t *p = palette[next_random() % (uint32_t) colours];
                    for (int k = 0; k < 3; k++)
                        img.px[r][c][k] = lit ? p[k] : 0;
                }
            info.bits = COLS * 24;
            rec_frame(&img, cycle, &info);
        } else if (sscanf(line, "close %llu", &cycle) == 1) {
            rec_close(cycle);
            return 0;
        } else if (line[0] != '\n' && line[0] != '#') {
            fprintf(stderr, "gif_case: bad command: %s", line);
            return 2;
        }
    }
    fprintf(stderr, "gif_case: no close command\n");
    return 2;
}
