// Headless outputs: animated GIF, PNG sequence, and the JSON-lines frames dump.
//
// Time. GIF and PNG timing comes from the simulated clock, in whole numbers.
// A GIF cannot time frames finer than 1/100 s (and most viewers slow down
// anything under 2/100 s), so the clip is cut into ticks of tick_cs/100 s
// (default 2), which is exactly 160000 * tick_cs CPU cycles:
//   - during tick k the clip shows the last frame latched in tick k or
//     earlier (dark before the first)
//   - a picture that does not change is written once, with the whole time it
//     stays; a stay too long for one GIF delay field (655.35 s) is split over
//     several frames whose delays add up to it
//   - the clip lasts ceil(end / tick) ticks, and at least to the end of the
//     tick in which the last frame latched
// So the clip is as long as the simulated time it covers, to within one tick,
// however long that is. The frames dump is not sampled: it has every frame.
//
// Failures. Every allocation, write and close here is checked. The first
// failure is kept (rec_error), nothing more is written to that output, and
// the caller is expected to stop and exit with an error. A PNG is listed in
// index.tsv only after it has been written and closed whole.
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "emu.h"

#define CYCLES_PER_CS 160000u      // CPU cycles in 1/100 s at 16 MHz
#define GIF_MAX_DELAY 65535u       // the GIF delay field: 16 bits, in 1/100 s
#define MAX_SCALE 64               // keeps the canvas far inside GIF's 16-bit sizes
#define MAX_TICK_CS 6000

// ---- failures ---------------------------------------------------------------
static char g_error[1600];

static void fail(const char *path, const char *what)
{
    int e = errno;
    if (g_error[0])
        return;                    // keep the first
    if (e)
        snprintf(g_error, sizeof(g_error), "%s: %s: %s", path, what, strerror(e));
    else
        snprintf(g_error, sizeof(g_error), "%s: %s", path, what);
}

int rec_failed(void)
{
    return g_error[0] != 0;
}

const char *rec_error(void)
{
    return g_error;
}

// True if everything written to f so far has been accepted.
static int wrote(FILE *f, const char *path)
{
    if (!ferror(f))
        return 1;
    fail(path, "write failed");
    return 0;
}

// Closes *f and reports whether its last buffered bytes made it out.
static int finish(FILE **f, const char *path)
{
    if (!*f)
        return 1;
    int bad = ferror(*f), write_errno = errno;
    FILE *closing = *f;
    *f = NULL;
    if (fclose(closing) != 0 && !bad) {
        fail(path, "close failed");          // the last buffered bytes did not go out
        return 0;
    }
    if (bad) {
        errno = write_errno;
        fail(path, "write failed");
        return 0;
    }
    return 1;
}

static char *copy_string(const char *s)
{
    char *c = malloc(strlen(s) + 1);
    if (c)
        strcpy(c, s);
    return c;
}

// ---- canvas ----------------------------------------------------------------
static int g_scale = 6, g_band;
static int g_w, g_h;
static uint8_t *g_canvas;          // RGB
static const uint8_t BG[3] = { 0, 0, 0 };

static unsigned g_tick_cs = 2;

// Layout, pixels per LED and tick, for every output. Called once, before any
// output is opened; the frames dump records these even when it is the only
// output. Values outside the supported ranges are brought inside them
// (main() refuses them before they get here).
void rec_configure(int band, int scale, int tick_cs)
{
    g_band = band;
    g_scale = scale < 2 ? 2 : scale > MAX_SCALE ? MAX_SCALE : scale;
    g_tick_cs = tick_cs < 1 ? 1u : tick_cs > MAX_TICK_CS ? (unsigned) MAX_TICK_CS : (unsigned) tick_cs;
}

static int canvas_setup(void)
{
    if (g_canvas)
        return 0;
    int margin = g_scale;
    if (g_band) {
        g_w = COLS * g_scale + 2 * margin;
        g_h = ROWS * g_scale + 2 * margin;
    } else {
        g_w = PANEL_COLS * g_scale + 2 * margin;
        g_h = 2 * ROWS * g_scale + 3 * margin;      // side A, gap, side B
    }
    g_canvas = malloc((size_t) g_w * (size_t) g_h * 3);
    if (!g_canvas) {
        fail("picture buffer", "out of memory");
        return -1;
    }
    return 0;
}

// img holds screen colours (see rec_frame), not LED values.
static void canvas_render(const image_t *img)
{
    int margin = g_scale;
    for (int i = 0; i < g_w * g_h; i++)
        memcpy(g_canvas + i * 3, BG, 3);
    for (int row = 0; row < ROWS; row++) {
        for (int col = 0; col < COLS; col++) {
            const uint8_t *c = img->px[row][col];
            int x0, y0;
            if (g_band) {
                x0 = margin + col * g_scale;
                y0 = margin + row * g_scale;
            } else {
                int side = col / PANEL_COLS;
                x0 = margin + (col % PANEL_COLS) * g_scale;
                y0 = margin + side * (ROWS * g_scale + margin) + row * g_scale;
            }
            // Each LED is a square with a one-pixel dark gap to its neighbours.
            for (int y = y0; y < y0 + g_scale - 1; y++)
                for (int x = x0; x < x0 + g_scale - 1; x++)
                    memcpy(g_canvas + (y * g_w + x) * 3, c, 3);
        }
    }
}

// ---- GIF -------------------------------------------------------------------
static FILE *g_gif;
static char *g_gif_path;
static uint8_t *g_index;           // palette indices for the canvas
static uint16_t (*g_tree)[256];    // LZW dictionary

static struct { uint8_t buf[256]; int n; uint32_t acc; int nacc; } g_bits;

static void bits_flush_block(void)
{
    if (g_bits.n) {
        fputc(g_bits.n, g_gif);
        fwrite(g_bits.buf, 1, (size_t) g_bits.n, g_gif);
        g_bits.n = 0;
    }
}

static void bits_put(uint32_t code, int size)
{
    g_bits.acc |= code << g_bits.nacc;
    g_bits.nacc += size;
    while (g_bits.nacc >= 8) {
        g_bits.buf[g_bits.n++] = g_bits.acc & 0xff;
        g_bits.acc >>= 8;
        g_bits.nacc -= 8;
        if (g_bits.n == 255)
            bits_flush_block();
    }
}

static void bits_end(void)
{
    if (g_bits.nacc > 0) {
        g_bits.buf[g_bits.n++] = g_bits.acc & 0xff;
        if (g_bits.n == 255)
            bits_flush_block();
    }
    g_bits.acc = 0;
    g_bits.nacc = 0;
    bits_flush_block();
    fputc(0, g_gif);               // block terminator
}

static void put16(FILE *f, unsigned v)
{
    fputc((int) (v & 0xff), f);
    fputc((int) ((v >> 8) & 0xff), f);
}

// Build a palette for the canvas. Returns the number of colours (<= 256).
static int build_palette(uint8_t pal[256][3])
{
    for (int drop = 0; drop < 8; drop++) {
        uint8_t mask = (uint8_t) (0xff << drop);
        int n = 0, ok = 1;
        static int32_t keys[4096];
        static uint16_t vals[4096];
        memset(keys, 0xff, sizeof(keys));
        for (int i = 0; i < g_w * g_h && ok; i++) {
            const uint8_t *p = g_canvas + i * 3;
            int32_t key = ((p[0] & mask) << 16) | ((p[1] & mask) << 8) | (p[2] & mask);
            uint32_t h = ((uint32_t) key * 2654435761u) >> 20;
            while (keys[h] != -1 && keys[h] != key)
                h = (h + 1) & 4095;
            if (keys[h] == -1) {
                if (n == 256) {
                    ok = 0;
                    break;
                }
                keys[h] = key;
                vals[h] = (uint16_t) n;
                pal[n][0] = p[0] & mask;
                pal[n][1] = p[1] & mask;
                pal[n][2] = p[2] & mask;
                n++;
            }
            g_index[i] = (uint8_t) vals[h];
        }
        if (ok)
            return n;
    }
    return 0;                      // not reached: with one bit a channel there are 8 colours
}

// One GIF frame of the canvas. delay_cs is 1..GIF_MAX_DELAY.
static void gif_write_frame(unsigned delay_cs)
{
    uint8_t pal[256][3];
    int ncol = build_palette(pal);
    int depth = 1;
    while ((1 << depth) < ncol)
        depth++;

    // Graphic control extension: delay, no transparency, leave in place.
    fputc(0x21, g_gif); fputc(0xf9, g_gif); fputc(4, g_gif);
    fputc(0x04, g_gif); put16(g_gif, delay_cs); fputc(0, g_gif); fputc(0, g_gif);
    // Image descriptor with a local colour table.
    fputc(0x2c, g_gif);
    put16(g_gif, 0); put16(g_gif, 0); put16(g_gif, (unsigned) g_w); put16(g_gif, (unsigned) g_h);
    fputc(0x80 | (depth - 1), g_gif);
    for (int i = 0; i < (1 << depth); i++)
        fwrite(i < ncol ? pal[i] : BG, 1, 3, g_gif);

    int min_code = depth < 2 ? 2 : depth;
    fputc(min_code, g_gif);
    const uint32_t clear = 1u << min_code;
    uint32_t max_code = clear + 1;
    int size = min_code + 1;
    size_t n = (size_t) g_w * (size_t) g_h;
    memset(g_tree, 0, sizeof(uint16_t) * 4096 * 256);
    bits_put(clear, size);
    uint32_t cur = g_index[0];
    for (size_t i = 1; i < n; i++) {
        uint8_t next = g_index[i];
        if (g_tree[cur][next]) {
            cur = g_tree[cur][next];
            continue;
        }
        bits_put(cur, size);
        g_tree[cur][next] = (uint16_t) ++max_code;
        if (max_code >= (1u << size))
            size++;
        if (max_code == 4095) {
            bits_put(clear, size);
            memset(g_tree, 0, sizeof(uint16_t) * 4096 * 256);
            size = min_code + 1;
            max_code = clear + 1;
        }
        cur = next;
    }
    bits_put(cur, size);
    // A decoder adds a dictionary entry for every code it reads after the
    // first one since a clear, so it adds one for that last code too, and its
    // code width grows if that entry is the last one the current width can
    // number. Nothing was added here for the last code, so follow the decoder
    // before writing the end code. (cur is the first code since a clear only
    // when max_code is still clear + 1, far from a width boundary.)
    if (max_code + 1 >= (1u << size) && size < 12)
        size++;
    bits_put(clear + 1, size);
    bits_end();
}

// The canvas, shown for cs hundredths of a second (cs >= 1, any size). One
// delay field holds at most GIF_MAX_DELAY, so a longer stay becomes several
// frames of the same picture, of nearly equal length, that add up to cs.
static void gif_write_hold(uint64_t cs)
{
    uint64_t pieces = (cs + GIF_MAX_DELAY - 1) / GIF_MAX_DELAY;
    uint64_t each = cs / pieces, longer = cs % pieces;
    for (uint64_t i = 0; i < pieces && wrote(g_gif, g_gif_path); i++)
        gif_write_frame((unsigned) (each + (i < longer ? 1 : 0)));
}

// ---- PNG -------------------------------------------------------------------
static char *g_png_dir, *g_png_index_path;
static FILE *g_png_index;

static void png_chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    uint8_t len[4] = { (uint8_t) (n >> 24), (uint8_t) (n >> 16), (uint8_t) (n >> 8), (uint8_t) n };
    fwrite(len, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n)
        fwrite(data, 1, n, f);
    uLong crc = crc32(0L, (const Bytef *) type, 4);
    if (n)
        crc = crc32(crc, data, (uInt) n);
    uint8_t c[4] = { (uint8_t) (crc >> 24), (uint8_t) (crc >> 16), (uint8_t) (crc >> 8), (uint8_t) crc };
    fwrite(c, 1, 4, f);
}

// Writes the canvas as a PNG. Returns 1 only if the whole file was written
// and closed; otherwise the failure is recorded and no partial file is left.
static int png_write(const char *path)
{
    size_t stride = (size_t) g_w * 3 + 1, raw_size = stride * (size_t) g_h;
    uLongf zn = compressBound((uLong) raw_size);
    uint8_t *raw = malloc(raw_size), *z = malloc(zn);
    FILE *f = NULL;
    int ok = 0;
    if (!raw || !z) {
        fail(path, "out of memory");
        goto done;
    }
    for (int y = 0; y < g_h; y++) {
        raw[(size_t) y * stride] = 0;
        memcpy(raw + (size_t) y * stride + 1, g_canvas + (size_t) y * g_w * 3, (size_t) g_w * 3);
    }
    if (compress2(z, &zn, raw, (uLong) raw_size, 9) != Z_OK) {
        errno = 0;
        fail(path, "compression failed");
        goto done;
    }
    f = fopen(path, "wb");
    if (!f) {
        fail(path, "cannot create");
        goto done;
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = { (uint8_t) (g_w >> 24), (uint8_t) (g_w >> 16), (uint8_t) (g_w >> 8), (uint8_t) g_w,
                         (uint8_t) (g_h >> 24), (uint8_t) (g_h >> 16), (uint8_t) (g_h >> 8), (uint8_t) g_h,
                         8, 2, 0, 0, 0 };
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, zn);
    png_chunk(f, "IEND", NULL, 0);
    ok = finish(&f, path);
    if (!ok)
        remove(path);              // do not leave half a picture behind
done:
    free(raw);
    free(z);
    return ok;
}

// ---- sampling shared by GIF and PNG ----------------------------------------
// g_cur is the picture the clip shows from tick g_cur_tick on; it is written
// when the next different picture, or the end, says how long it stayed.
// g_last is the latest frame latched, in tick g_last_tick: it becomes the
// clip's picture for that tick only once the tick is over, because a later
// frame in the same tick replaces it.
// Both hold screen colours, the LED values after the LED-to-screen mapping:
// "the picture changed" means what is drawn changed. Two LED values that map
// to the same screen colour are the same picture.
static int g_sampling;             // GIF or PNG output is on
static image_t g_cur, g_last;
static uint64_t g_cur_tick, g_last_tick;
static int g_have_last;

static void to_screen(const image_t *leds, image_t *screen)
{
    for (int row = 0; row < ROWS; row++)
        for (int col = 0; col < COLS; col++)
            view_map(leds->px[row][col], screen->px[row][col]);
}

// The clip starts dark.
static void sampling_start(void)
{
    if (g_sampling)
        return;
    image_t dark;
    memset(&dark, 0, sizeof(dark));
    to_screen(&dark, &g_cur);
    g_sampling = 1;
}

static uint64_t tick_of(uint64_t cycle)
{
    return cycle / ((uint64_t) CYCLES_PER_CS * g_tick_cs);
}

// Writes g_cur as the picture for ticks g_cur_tick up to (not including) until.
static void write_cur(uint64_t until)
{
    if (until <= g_cur_tick)
        return;                    // nothing to show: cannot happen while time runs forward
    uint64_t start_cs = g_cur_tick * g_tick_cs, cs = (until - g_cur_tick) * g_tick_cs;
    canvas_render(&g_cur);
    if (g_gif && wrote(g_gif, g_gif_path)) {
        gif_write_hold(cs);
        wrote(g_gif, g_gif_path);
    }
    if (g_png_dir && g_png_index && !rec_failed()) {
        // t<seconds>.<milliseconds>.png, the moment the picture appears.
        char name[64];
        snprintf(name, sizeof(name), "t%05llu.%02llu0.png",
                 (unsigned long long) (start_cs / 100), (unsigned long long) (start_cs % 100));
        size_t path_size = strlen(g_png_dir) + strlen(name) + 2;
        char *path = malloc(path_size);
        if (!path) {
            fail(g_png_dir, "out of memory");
            return;
        }
        snprintf(path, path_size, "%s/%s", g_png_dir, name);
        if (png_write(path)) {
            fprintf(g_png_index, "%s\t%llu.%02llu0\t%llu.%02llu0\n", name,
                    (unsigned long long) (start_cs / 100), (unsigned long long) (start_cs % 100),
                    (unsigned long long) (cs / 100), (unsigned long long) (cs % 100));
            fflush(g_png_index);   // the index on disk always matches the pictures on disk
            wrote(g_png_index, g_png_index_path);
        }
        free(path);
    }
}

// The tick of g_last is over: it is that tick's picture.
static void settle_last(void)
{
    if (!g_have_last)
        return;
    g_have_last = 0;
    if (g_last_tick == g_cur_tick) {
        g_cur = g_last;            // still the tick the current picture starts in
    } else if (memcmp(&g_last, &g_cur, sizeof(g_cur))) {
        write_cur(g_last_tick);
        g_cur = g_last;
        g_cur_tick = g_last_tick;
    }
}

int rec_gif_open(const char *path)
{
    if (canvas_setup())
        return -1;
    g_gif_path = copy_string(path);
    g_index = malloc((size_t) g_w * (size_t) g_h);
    g_tree = malloc(sizeof(uint16_t) * 4096 * 256);
    if (!g_gif_path || !g_index || !g_tree) {
        errno = ENOMEM;
        fail(path, "out of memory");
        return -1;
    }
    g_gif = fopen(path, "wb");
    if (!g_gif) {
        fail(path, "cannot create");
        return -1;
    }
    fwrite("GIF89a", 1, 6, g_gif);
    put16(g_gif, (unsigned) g_w); put16(g_gif, (unsigned) g_h);
    fputc(0x70, g_gif); fputc(0, g_gif); fputc(0, g_gif);     // no global colour table
    // Loop forever.
    fwrite("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 1, 19, g_gif);
    if (!wrote(g_gif, g_gif_path))
        return -1;
    sampling_start();
    return 0;
}

int rec_png_open(const char *dir)
{
    if (canvas_setup())
        return -1;
    g_png_dir = copy_string(dir);
    size_t index_size = strlen(dir) + sizeof("/index.tsv");
    g_png_index_path = malloc(index_size);
    if (!g_png_dir || !g_png_index_path) {
        errno = ENOMEM;
        fail(dir, "out of memory");
        return -1;
    }
    snprintf(g_png_index_path, index_size, "%s/index.tsv", dir);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fail(dir, "cannot create the directory");
        return -1;
    }
    g_png_index = fopen(g_png_index_path, "w");
    if (!g_png_index) {
        fail(g_png_index_path, "cannot create");
        return -1;
    }
    fprintf(g_png_index, "file\tstart_s\tshown_for_s\n");
    if (!wrote(g_png_index, g_png_index_path))
        return -1;
    sampling_start();
    return 0;
}

// ---- JSON-lines frames dump ------------------------------------------------
static FILE *g_events;
static char *g_events_path;

// Writes s as a JSON string. Quotes, backslashes and control characters are
// escaped. Bytes above 0x7f are kept only if the whole string is valid UTF-8;
// otherwise each becomes U+FFFD, so the line always parses.
static int utf8_valid(const unsigned char *s)
{
    while (*s) {
        int extra;
        if (*s < 0x80) extra = 0;
        else if ((*s & 0xe0) == 0xc0 && *s >= 0xc2) extra = 1;
        else if ((*s & 0xf0) == 0xe0) extra = 2;
        else if ((*s & 0xf8) == 0xf0 && *s <= 0xf4) extra = 3;
        else return 0;
        unsigned char lead = *s++;
        // No overlong forms, no surrogates, nothing above U+10FFFF.
        if ((lead == 0xe0 && *s < 0xa0) || (lead == 0xed && *s > 0x9f) ||
            (lead == 0xf0 && *s < 0x90) || (lead == 0xf4 && *s > 0x8f))
            return 0;
        while (extra--) {
            if ((*s & 0xc0) != 0x80)
                return 0;
            s++;
        }
    }
    return 1;
}

static void json_string(FILE *f, const char *text)
{
    const unsigned char *s = (const unsigned char *) text;
    int utf8 = utf8_valid(s);
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fprintf(f, "\\%c", *s);
        else if (*s < 0x20 || *s == 0x7f)
            fprintf(f, "\\u%04x", *s);
        else if (*s >= 0x80 && !utf8)
            fputs("\\ufffd", f);
        else
            fputc(*s, f);
    }
    fputc('"', f);
}

// The first line of the dump. firmware is a file name and may hold any bytes;
// settings_json is a comma-separated list of members built from numbers only.
// The "render" member is what a reader needs to redraw the GIF and PNG
// pictures from the frames (tests/gif_check.py does).
int rec_events_open(const char *path, const char *firmware, const char *settings_json)
{
    g_events_path = copy_string(path);
    if (!g_events_path) {
        errno = ENOMEM;
        fail(path, "out of memory");
        return -1;
    }
    g_events = fopen(path, "w");
    if (!g_events) {
        fail(path, "cannot create");
        return -1;
    }
    fputs("{\"type\":\"meta\",\"firmware\":", g_events);
    json_string(g_events, firmware);
    fprintf(g_events, ",%s,\"render\":{\"scale\":%d,\"tick_cs\":%u,\"band\":%s,\"gamma\":%.17g}}\n",
            settings_json, g_scale, g_tick_cs, g_band ? "true" : "false", view_get_gamma());
    return wrote(g_events, g_events_path) ? 0 : -1;
}

void rec_event(const char *type, uint64_t cycle, const char *extra_json)
{
    if (!g_events || rec_failed())
        return;
    fprintf(g_events, "{\"type\":\"%s\",\"t\":%.7f,\"cycle\":%llu%s%s}\n", type,
            cycles_to_s(cycle), (unsigned long long) cycle,
            extra_json && extra_json[0] ? "," : "", extra_json ? extra_json : "");
    wrote(g_events, g_events_path);
}

void rec_frame(const image_t *img, uint64_t latch_cycle, const ws_frame_info_t *info)
{
    // Frames are recorded in the order they are given, and time does not run
    // backwards in a recording: a frame stamped earlier than the one before it
    // is recorded at that one's moment. (The decoder never produces one.)
    static uint64_t newest;
    if (latch_cycle < newest)
        latch_cycle = newest;
    newest = latch_cycle;
    if (rec_failed())
        return;
    if (g_events) {
        static const char hex[] = "0123456789abcdef";
        fprintf(g_events, "{\"type\":\"frame\",\"t\":%.7f,\"cycle\":%llu,\"bits\":%llu,"
                          "\"partial_bits\":%u,\"marginal\":%llu,\"gap_us\":%.3f,"
                          "\"t0h_cycles\":[%u,%u],\"t1h_cycles\":[%u,%u],\"rows\":[",
                cycles_to_s(latch_cycle), (unsigned long long) latch_cycle,
                (unsigned long long) info->bits, info->partial_bits,
                (unsigned long long) info->marginal,
                info->gap_max / (F_CPU_HZ / 1e6),
                info->t0h_min, info->t0h_max, info->t1h_min, info->t1h_max);
        char row_text[COLS * 6 + 4];
        for (int row = 0; row < ROWS; row++) {
            char *p = row_text;
            *p++ = '"';
            for (int col = 0; col < COLS; col++)
                for (int c = 0; c < 3; c++) {
                    *p++ = hex[img->px[row][col][c] >> 4];
                    *p++ = hex[img->px[row][col][c] & 15];
                }
            *p++ = '"';
            if (row < ROWS - 1)
                *p++ = ',';
            fwrite(row_text, 1, (size_t) (p - row_text), g_events);
        }
        fputs("]}\n", g_events);
        wrote(g_events, g_events_path);
    }
    if (g_sampling) {
        uint64_t tick = tick_of(latch_cycle);
        if (g_have_last && tick > g_last_tick)
            settle_last();
        to_screen(img, &g_last);
        g_last_tick = tick;
        g_have_last = 1;
    }
}

int rec_close(uint64_t end_cycle)
{
    if (g_sampling && !rec_failed()) {
        uint64_t per_tick = (uint64_t) CYCLES_PER_CS * g_tick_cs;
        uint64_t ticks = end_cycle / per_tick + (end_cycle % per_tick ? 1 : 0);
        if (g_have_last && g_last_tick + 1 > ticks)
            ticks = g_last_tick + 1;
        if (ticks < 1)
            ticks = 1;
        settle_last();
        write_cur(ticks);
    }
    g_sampling = 0;
    if (g_gif) {
        if (!rec_failed())
            fputc(0x3b, g_gif);
        finish(&g_gif, g_gif_path);
    }
    finish(&g_png_index, g_png_index_path);
    if (g_events) {
        rec_event("end", end_cycle, NULL);
        finish(&g_events, g_events_path);
    }
    return rec_failed() ? -1 : 0;
}
