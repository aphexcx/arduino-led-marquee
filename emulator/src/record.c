// Headless outputs: animated GIF, PNG sequence, and the JSON-lines event dump.
//
// GIF and PNG timing comes from the simulated clock. A GIF cannot hold frames
// shorter than 1/100 s (and most viewers slow down anything under 2/100 s), so
// the picture is sampled on a fixed tick (default 2/100 s = 50 per second):
// each tick shows the last frame the LEDs latched before it, and a picture
// that does not change is written once with a long delay. The clip therefore
// runs exactly as long as the simulated time it covers. The JSON-lines dump is
// not sampled: it has every latched frame.
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "emu.h"

// ---- canvas ----------------------------------------------------------------
static int g_scale = 6, g_band;
static int g_w, g_h;
static uint8_t *g_canvas;          // RGB
static const uint8_t BG[3] = { 0, 0, 0 };

void rec_set_layout(int band)
{
    g_band = band;
}

static void canvas_setup(int scale)
{
    if (g_canvas)
        return;
    g_scale = scale < 2 ? 2 : scale;
    int margin = g_scale;
    if (g_band) {
        g_w = COLS * g_scale + 2 * margin;
        g_h = ROWS * g_scale + 2 * margin;
    } else {
        g_w = PANEL_COLS * g_scale + 2 * margin;
        g_h = 2 * ROWS * g_scale + 3 * margin;      // side A, gap, side B
    }
    g_canvas = malloc((size_t) g_w * g_h * 3);
}

static void canvas_render(const image_t *img)
{
    int margin = g_scale;
    for (int i = 0; i < g_w * g_h; i++)
        memcpy(g_canvas + i * 3, BG, 3);
    for (int row = 0; row < ROWS; row++) {
        for (int col = 0; col < COLS; col++) {
            uint8_t c[3];
            view_map(img->px[row][col], c);
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

static void put16(FILE *f, int v)
{
    fputc(v & 0xff, f);
    fputc((v >> 8) & 0xff, f);
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
    return 0;
}

static void gif_write_frame(int delay_cs)
{
    uint8_t pal[256][3];
    int ncol = build_palette(pal);
    int depth = 1;
    while ((1 << depth) < ncol)
        depth++;
    if (delay_cs > 65535)
        delay_cs = 65535;

    // Graphic control extension: delay, no transparency, leave in place.
    fputc(0x21, g_gif); fputc(0xf9, g_gif); fputc(4, g_gif);
    fputc(0x04, g_gif); put16(g_gif, delay_cs); fputc(0, g_gif); fputc(0, g_gif);
    // Image descriptor with a local colour table.
    fputc(0x2c, g_gif);
    put16(g_gif, 0); put16(g_gif, 0); put16(g_gif, g_w); put16(g_gif, g_h);
    fputc(0x80 | (depth - 1), g_gif);
    for (int i = 0; i < (1 << depth); i++)
        fwrite(i < ncol ? pal[i] : BG, 1, 3, g_gif);

    int min_code = depth < 2 ? 2 : depth;
    fputc(min_code, g_gif);
    const uint32_t clear = 1u << min_code;
    uint32_t max_code = clear + 1;
    int size = min_code + 1;
    size_t n = (size_t) g_w * g_h;
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
    bits_put(clear, size);
    bits_put(clear + 1, min_code + 1);
    bits_end();
}

// ---- PNG -------------------------------------------------------------------
static char g_png_dir[1024];
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

static void png_write(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    size_t stride = (size_t) g_w * 3 + 1;
    uint8_t *raw = malloc(stride * g_h);
    for (int y = 0; y < g_h; y++) {
        raw[y * stride] = 0;
        memcpy(raw + y * stride + 1, g_canvas + (size_t) y * g_w * 3, (size_t) g_w * 3);
    }
    uLongf zn = compressBound((uLong) (stride * g_h));
    uint8_t *z = malloc(zn);
    compress2(z, &zn, raw, (uLong) (stride * g_h), 9);
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = { (uint8_t) (g_w >> 24), (uint8_t) (g_w >> 16), (uint8_t) (g_w >> 8), (uint8_t) g_w,
                         (uint8_t) (g_h >> 24), (uint8_t) (g_h >> 16), (uint8_t) (g_h >> 8), (uint8_t) g_h,
                         8, 2, 0, 0, 0 };
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, zn);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

// ---- tick sampling shared by GIF and PNG -----------------------------------
static int g_tick_cs = 2;
static int g_sampling;             // GIF or PNG output is on
static image_t g_shown;            // picture on the LEDs at g_shown_tick
static int64_t g_shown_tick;

static void write_shown(int64_t until_tick)
{
    int64_t ticks = until_tick - g_shown_tick;
    if (ticks < 1)
        ticks = 1;
    canvas_render(&g_shown);
    if (g_gif)
        gif_write_frame((int) (ticks * g_tick_cs));
    if (g_png_dir[0]) {
        char path[1200], name[64];
        double t = g_shown_tick * g_tick_cs / 100.0;
        snprintf(name, sizeof(name), "t%09.3f.png", t);
        snprintf(path, sizeof(path), "%s/%s", g_png_dir, name);
        png_write(path);
        if (g_png_index)
            fprintf(g_png_index, "%s\t%.3f\t%.3f\n", name, t, ticks * g_tick_cs / 100.0);
    }
}

int rec_gif_open(const char *path, int scale, int tick_cs)
{
    canvas_setup(scale);
    g_tick_cs = tick_cs < 1 ? 1 : tick_cs;
    g_gif = fopen(path, "wb");
    if (!g_gif)
        return -1;
    g_index = malloc((size_t) g_w * g_h);
    g_tree = malloc(sizeof(uint16_t) * 4096 * 256);
    fwrite("GIF89a", 1, 6, g_gif);
    put16(g_gif, g_w); put16(g_gif, g_h);
    fputc(0x70, g_gif); fputc(0, g_gif); fputc(0, g_gif);     // no global colour table
    // Loop forever.
    fwrite("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 1, 19, g_gif);
    g_sampling = 1;
    return 0;
}

int rec_png_open(const char *dir, int scale, int tick_cs)
{
    canvas_setup(scale);
    g_tick_cs = tick_cs < 1 ? 1 : tick_cs;
    mkdir(dir, 0755);
    snprintf(g_png_dir, sizeof(g_png_dir), "%s", dir);
    char path[1200];
    snprintf(path, sizeof(path), "%s/index.tsv", dir);
    g_png_index = fopen(path, "w");
    if (!g_png_index)
        return -1;
    fprintf(g_png_index, "file\tstart_s\tshown_for_s\n");
    g_sampling = 1;
    return 0;
}

// ---- JSON-lines events -----------------------------------------------------
static FILE *g_events;

int rec_events_open(const char *path, const char *meta_json)
{
    g_events = fopen(path, "w");
    if (!g_events)
        return -1;
    fprintf(g_events, "{\"type\":\"meta\",%s}\n", meta_json);
    return 0;
}

void rec_event(const char *type, uint64_t cycle, const char *extra_json)
{
    if (!g_events)
        return;
    fprintf(g_events, "{\"type\":\"%s\",\"t\":%.7f,\"cycle\":%llu%s%s}\n", type,
            cycles_to_s(cycle), (unsigned long long) cycle,
            extra_json && extra_json[0] ? "," : "", extra_json ? extra_json : "");
}

void rec_frame(const image_t *img, uint64_t latch_cycle, const ws_frame_info_t *info)
{
    if (g_events) {
        static const char hex[] = "0123456789abcdef";
        static char line[ROWS * (COLS * 6 + 4) + 512];
        char *p = line;
        p += sprintf(p, "{\"type\":\"frame\",\"t\":%.7f,\"cycle\":%llu,\"bits\":%u,"
                        "\"partial_bits\":%u,\"marginal\":%u,\"gap_us\":%.3f,"
                        "\"t0h_cycles\":[%u,%u],\"t1h_cycles\":[%u,%u],\"rows\":[",
                     cycles_to_s(latch_cycle), (unsigned long long) latch_cycle, info->bits,
                     info->partial_bits, info->marginal, info->gap_max / (F_CPU_HZ / 1e6),
                     info->t0h_min, info->t0h_max, info->t1h_min, info->t1h_max);
        for (int row = 0; row < ROWS; row++) {
            *p++ = '"';
            for (int col = 0; col < COLS; col++)
                for (int c = 0; c < 3; c++) {
                    *p++ = hex[img->px[row][col][c] >> 4];
                    *p++ = hex[img->px[row][col][c] & 15];
                }
            *p++ = '"';
            if (row < ROWS - 1)
                *p++ = ',';
        }
        p += sprintf(p, "]}\n");
        fwrite(line, 1, (size_t) (p - line), g_events);
    }
    if (g_sampling) {
        int64_t tick = (int64_t) (cycles_to_s(latch_cycle) * 100.0 / g_tick_cs);
        if (tick > g_shown_tick && memcmp(img, &g_shown, sizeof(*img))) {
            write_shown(tick);
            g_shown_tick = tick;
        }
        g_shown = *img;
    }
}

void rec_close(uint64_t end_cycle)
{
    if (g_sampling) {
        int64_t tick = (int64_t) (cycles_to_s(end_cycle) * 100.0 / g_tick_cs + 0.999);
        write_shown(tick);
        g_sampling = 0;
    }
    if (g_gif) {
        fputc(0x3b, g_gif);
        fclose(g_gif);
        g_gif = NULL;
    }
    if (g_png_index) {
        fclose(g_png_index);
        g_png_index = NULL;
    }
    if (g_events) {
        fclose(g_events);
        g_events = NULL;
    }
}
