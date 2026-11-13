// Live terminal view (ANSI truecolor), and the LED-value-to-screen mapping
// every output shares.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "emu.h"

// ---- LED value -> screen colour --------------------------------------------
// An LED's light output is proportional to the 8-bit value it is given. A
// screen's light output is proportional to (code/255)^2.2. So an LED value v
// is shown as 255 * (v/255)^(1/2.2): the same relative luminance, with white
// 255 on the LED shown as white 255 on the screen. --gamma changes the 2.2
// (1 = raw values). An LED that is off is drawn as a faint dot, not black, so
// the panel outline stays visible.
static uint8_t g_lut[256];
static const uint8_t OFF_COLOUR[3] = { 14, 14, 16 };

static double g_gamma;             // 0 until view_set_gamma has built the table

double view_get_gamma(void)
{
    if (g_gamma == 0)
        view_set_gamma(2.2);
    return g_gamma;
}

void view_set_gamma(double gamma)
{
    if (!(gamma >= 0.1 && gamma <= 10))
        gamma = 2.2;               // main() refuses such values; this keeps the table in 0..255
    g_gamma = gamma;
    for (int v = 0; v < 256; v++)
        g_lut[v] = (uint8_t) lround(255.0 * pow(v / 255.0, 1.0 / gamma));
}

void view_map(const uint8_t led[3], uint8_t screen[3])
{
    if (g_gamma == 0)
        view_set_gamma(2.2);
    if (!led[0] && !led[1] && !led[2]) {
        memcpy(screen, OFF_COLOUR, 3);
        return;
    }
    for (int i = 0; i < 3; i++)
        screen[i] = g_lut[led[i]];
}

// ---- terminal --------------------------------------------------------------
// The view is a display, not a record: if the terminal has gone and a write
// to it fails, the run carries on, and the recordings are what count.
static int g_open, g_band;

void view_set_band(int band)
{
    g_band = band;
}

void view_open(void)
{
    // Alternate screen, hide cursor, clear.
    fputs("\x1b[?1049h\x1b[?25l\x1b[2J", stdout);
    fflush(stdout);
    g_open = 1;
}

void view_close(void)
{
    if (!g_open)
        return;
    fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    g_open = 0;
}

// One screenful is built here and written at once. A full redraw is about
// 20 000 bytes; put() never writes past the end whatever it is given.
static char g_screen[1 << 16];
static size_t g_len;

static void put(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_screen + g_len, sizeof(g_screen) - g_len, fmt, ap);
    va_end(ap);
    if (n > 0)
        g_len += (size_t) n < sizeof(g_screen) - g_len ? (size_t) n : sizeof(g_screen) - g_len - 1;
}

// Two characters per LED for one side (close to square LEDs), one per LED for
// the 120-column band so it fits a 124-column terminal.
static void put_panel(const image_t *img, int first_col, int ncols, int chars)
{
    for (int row = 0; row < ROWS; row++) {
        uint8_t last[3] = { 0, 0, 0 };
        int have = 0;
        put("  ");
        for (int col = first_col; col < first_col + ncols; col++) {
            uint8_t c[3];
            view_map(img->px[row][col], c);
            if (!have || memcmp(c, last, 3)) {
                put("\x1b[48;2;%d;%d;%dm", c[0], c[1], c[2]);
                memcpy(last, c, 3);
                have = 1;
            }
            put(chars == 2 ? "  " : " ");
        }
        put("\x1b[0m\x1b[K\r\n");
    }
}

void view_draw(const image_t *img, const view_status_t *st)
{
    g_len = 0;
    put("\x1b[H\x1b[0m");
    put("  \x1b[1mENERGY sign emulator\x1b[0m   real firmware on simavr   "
        "sim time %9.3f s\x1b[K\r\n", st->sim_s);

    put("  speed %.2fx simulated/real", st->speed_now);
    if (st->speed_target > 0)
        put(" (paced to %.2fx)", st->speed_target);
    else
        put(" (unpaced)");
    if (st->speed_target > 0 && st->speed_now < st->speed_target * 0.97)
        put("   \x1b[1;33mSLOWER THAN ASKED: this Mac cannot keep up\x1b[0m");
    put("\x1b[K\r\n\x1b[K\r\n");

    if (g_band) {
        put("  Whole strip, columns 0-119: side A is the left half, side B the "
            "right half\x1b[K\r\n");
        put_panel(img, 0, COLS, 1);
    } else {
        put("  Side A   columns 0-59 (nearest the Arduino)\x1b[K\r\n");
        put_panel(img, 0, PANEL_COLS, 2);
        put("\x1b[K\r\n  Side B   columns 60-119 (mounted back to back with A)\x1b[K\r\n");
        put_panel(img, PANEL_COLS, PANEL_COLS, 2);
    }
    put("\x1b[K\r\n");

    put("  frames latched %llu   requests from sign %llu   bytes to sign %llu\x1b[K\r\n",
        (unsigned long long) st->frames, (unsigned long long) st->requests,
        (unsigned long long) st->rx_bytes);
    if (st->port > 0)
        put("  serial link: tcp 127.0.0.1:%d  %s\x1b[K\r\n", st->port,
            st->client ? "\x1b[32mapp connected\x1b[0m"
                       : "\x1b[33mno app connected (run fake_app.py)\x1b[0m");
    else
        put("  serial link: off\x1b[K\r\n");
    put("  sign -> app: %s\x1b[K\r\n", st->last_tx[0] ? st->last_tx : "(nothing yet)");
    put("  app -> sign: %s\x1b[K\r\n", st->last_rx[0] ? st->last_rx : "(nothing yet)");
    if (st->bpm > 0)
        put("  beat input A0: %.0f bpm %s\x1b[K\r\n", st->bpm,
            st->beat_on ? "\x1b[1;37m*\x1b[0m" : " ");
    if (st->marginal || st->partial)
        put("  \x1b[1;31mWS2812 timing problems: %llu marginal pulses, "
            "%llu frames with a partial pixel\x1b[0m\x1b[K\r\n",
            (unsigned long long) st->marginal, (unsigned long long) st->partial);
    put("  Ctrl-C to quit\x1b[K\r\n\x1b[J");

    fwrite(g_screen, 1, g_len, stdout);
    fflush(stdout);
}
