// sign-emu: runs the sign's real firmware image on a simulated ATmega328P
// (simavr) and shows what the LEDs would show. See ../README.md.
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "emu.h"
#include "avr_ioport.h"

// ---- options ---------------------------------------------------------------
static struct {
    const char *hex;
    int port;                 // 0 = no link
    int headless;
    int band;
    double duration_s;        // 0 = run until stopped
    long until_requests;      // 0 = off
    double speed;             // 0 = as fast as possible
    int speed_set;
    const char *gif, *png_dir, *events;
    int scale, tick_cs;
    double bpm;
    double reset_us;
    double gamma;
    double latency_ms;
    int wait_ms;
    int wait_client_s;
    const char *request_chars;
    int verbose;
} opt = {
    .port = 14400, .scale = 6, .tick_cs = 2, .reset_us = 50, .gamma = 2.2,
    .latency_ms = 5, .wait_ms = 1000, .request_chars = "~?",
};

static void usage(FILE *f)
{
    fputs(
"usage: sign-emu [options] <firmware.hex>\n"
"\n"
"Runs the sign firmware on a simulated ATmega328P and shows both 60 x 7 sides.\n"
"\n"
"  run length\n"
"    --duration <s>         stop after this much simulated time\n"
"    --until-requests <n>   stop when the sign asks for its n-th message\n"
"                           (it has then finished drawing the first n-1)\n"
"    --speed <x>            pace to x times real time (live default 1, 0 = flat out)\n"
"  views\n"
"    --headless             no terminal view (default when stdout is not a terminal)\n"
"    --band                 draw one 120 x 7 band instead of two stacked sides\n"
"    --record <out.gif>     write an animated GIF\n"
"    --png-dir <dir>        write a PNG for every change, plus index.tsv\n"
"    --frames <out.jsonl>   write every latched frame and serial byte as JSON lines\n"
"    --scale <n>            screen pixels per LED in GIF/PNG (default 6)\n"
"    --tick-cs <n>          GIF/PNG sampling tick in 1/100 s (default 2)\n"
"    --gamma <g>            LED value to screen mapping, see README (default 2.2)\n"
"  serial link (the UART to the app)\n"
"    --port <n>             TCP port on 127.0.0.1 (default 14400, 0 = off)\n"
"    --wait-client <s>      before booting, wait up to s seconds for an app to connect\n"
"    --link-latency-ms <n>  simulated time between a request and the reply's first\n"
"                           bit, when the app answers in time (default 5)\n"
"    --link-wait-ms <n>     real time to hold the simulation for that answer\n"
"                           (default 1000, 0 = never hold: replies land when they land)\n"
"    --request-chars <s>    bytes that count as a request (default \"~?\")\n"
"  inputs\n"
"    --bpm <n>              pulse the beat input (A0) n times a minute\n"
"  model\n"
"    --reset-us <n>         WS2812 latch time: a low gap longer than this shows the\n"
"                           frame (default 50)\n"
"  -v                       log serial bytes and frame timing to stderr\n",
        f);
}

// ---- state -----------------------------------------------------------------
static avr_t *avr;
static volatile sig_atomic_t g_stop;
static uint64_t g_frames, g_requests, g_rx_bytes, g_tx_bytes, g_marginal, g_partial, g_framing_errors;
static uint32_t g_gap_max, g_t0h_min = UINT32_MAX, g_t0h_max, g_t1h_min = UINT32_MAX, g_t1h_max;
static uint32_t g_bits_min = UINT32_MAX, g_bits_max;
static char g_last_tx[80] = "(nothing yet)", g_last_rx[80] = "(nothing yet)";
static int g_beat_on;
static double g_wall_blocked;     // real seconds spent holding for the app

static double wall_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_signal(int sig)
{
    g_stop = 1;
}

static void tail_append(char *tail, size_t cap, uint8_t byte)
{
    char piece[8];
    if (byte >= 0x20 && byte < 0x7f)
        snprintf(piece, sizeof(piece), "%c", byte);
    else
        snprintf(piece, sizeof(piece), "\\x%02x", byte);
    if (tail[0] == '(')
        tail[0] = 0;
    size_t len = strlen(tail), add = strlen(piece);
    if (len + add >= cap - 1) {
        size_t drop = len + add - (cap - 2);
        memmove(tail, tail + drop, len - drop + 1);
    }
    strcat(tail, piece);
}

// ---- callbacks -------------------------------------------------------------
static void on_frame(const image_t *img, uint64_t latch_cycle, const ws_frame_info_t *info)
{
    g_frames++;
    g_marginal += info->marginal;
    if (info->partial_bits)
        g_partial++;
    if (info->gap_max > g_gap_max) g_gap_max = info->gap_max;
    if (info->t0h_max) {
        if (info->t0h_min < g_t0h_min) g_t0h_min = info->t0h_min;
        if (info->t0h_max > g_t0h_max) g_t0h_max = info->t0h_max;
    }
    if (info->t1h_max) {
        if (info->t1h_min < g_t1h_min) g_t1h_min = info->t1h_min;
        if (info->t1h_max > g_t1h_max) g_t1h_max = info->t1h_max;
    }
    if (info->bits < g_bits_min) g_bits_min = info->bits;
    if (info->bits > g_bits_max) g_bits_max = info->bits;
    rec_frame(img, latch_cycle, info);
    if (opt.verbose > 1)
        fprintf(stderr, "[%10.6f] frame %llu: %u bits/strip, longest gap %.1f us\n",
                cycles_to_s(latch_cycle), (unsigned long long) g_frames, info->bits,
                info->gap_max / 16.0);
}

static void json_byte(char *out, size_t cap, uint8_t byte)
{
    if (byte >= 0x20 && byte < 0x7f && byte != '"' && byte != '\\')
        snprintf(out, cap, "\"byte\":%u,\"char\":\"%c\"", byte, byte);
    else
        snprintf(out, cap, "\"byte\":%u", byte);
}

static void on_tx(uint8_t byte, int framing_ok, uint64_t start_cycle, uint64_t end_cycle)
{
    char extra[64];
    g_tx_bytes++;
    if (!framing_ok)
        g_framing_errors++;
    json_byte(extra, sizeof(extra), byte);
    rec_event("tx", start_cycle, extra);
    tail_append(g_last_tx, sizeof(g_last_tx), byte);
    if (opt.verbose)
        fprintf(stderr, "[%10.6f] sign -> app: 0x%02x %c%s\n", cycles_to_s(start_cycle), byte,
                byte >= 0x20 && byte < 0x7f ? byte : '.', framing_ok ? "" : "  (framing error)");
    int is_request = strchr(opt.request_chars, byte) != NULL && byte != 0;
    if (is_request) {
        g_requests++;
        g_last_rx[0] = 0;
        if (opt.until_requests && (long) g_requests >= opt.until_requests) {
            g_stop = 1;
            return;
        }
    }
    link_send(byte);
    if (is_request && opt.wait_ms > 0 && link_connected()) {
        // Hold the simulated clock while the app works out its answer, then
        // put the answer on the wire a fixed simulated time after the request.
        // The app's thinking time on this Mac therefore does not leak into
        // the simulation, and runs repeat exactly.
        uint8_t buf[4096];
        double t0 = wall_now();
        size_t n = link_read(buf, sizeof(buf), opt.wait_ms);
        g_wall_blocked += wall_now() - t0;
        if (n)
            uart_rx_enqueue(buf, n, end_cycle + (uint64_t) (opt.latency_ms * F_CPU_HZ / 1000.0));
    }
}

static void on_rx(uint8_t byte, uint64_t start_cycle)
{
    char extra[64];
    g_rx_bytes++;
    json_byte(extra, sizeof(extra), byte);
    rec_event("rx", start_cycle, extra);
    tail_append(g_last_rx, sizeof(g_last_rx), byte);
    if (opt.verbose > 1)
        fprintf(stderr, "[%10.6f] app -> sign: 0x%02x %c\n", cycles_to_s(start_cycle), byte,
                byte >= 0x20 && byte < 0x7f ? byte : '.');
}

// ---- beat input on A0 (PC0) -------------------------------------------------
#define BEAT_HIGH_MS 30.0
static uint64_t g_beat_period;

static void beat_set(int level)
{
    avr_ioport_external_t e = { .name = 'C', .mask = 1, .value = level ? 1 : 0 };
    avr_ioctl(avr, AVR_IOCTL_IOPORT_SET_EXTERNAL('C'), &e);
    avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('C'), 0), level ? 1 : 0);
    g_beat_on = level;
    if (level)
        rec_event("beat", avr->cycle, NULL);
}

static avr_cycle_count_t beat_timer(avr_t *a, avr_cycle_count_t when, void *param)
{
    uint64_t high = (uint64_t) (BEAT_HIGH_MS * F_CPU_HZ / 1000.0);
    if (!g_beat_on) {
        beat_set(1);
        return when + high;
    }
    beat_set(0);
    return when + g_beat_period - high;
}

// ---- firmware image ---------------------------------------------------------
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Minimal Intel HEX reader (data, EOF and extended-address records).
static long load_ihex(const char *path, uint8_t *mem, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[600];
    uint32_t base = 0;
    long top = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != ':')
            continue;
        uint8_t b[300];
        int n = 0;
        for (char *p = line + 1; hexval(p[0]) >= 0 && hexval(p[1]) >= 0 && n < 300; p += 2)
            b[n++] = (uint8_t) (hexval(p[0]) << 4 | hexval(p[1]));
        if (n < 5 || n != b[0] + 5) {
            fclose(f);
            return -1;
        }
        uint8_t sum = 0;
        for (int i = 0; i < n; i++)
            sum += b[i];
        if (sum) {
            fclose(f);
            return -1;
        }
        uint32_t addr = base + ((uint32_t) b[1] << 8 | b[2]);
        if (b[3] == 0) {
            if (addr + b[0] > cap) {
                fclose(f);
                return -1;
            }
            memcpy(mem + addr, b + 4, b[0]);
            if ((long) (addr + b[0]) > top)
                top = addr + b[0];
        } else if (b[3] == 1) {
            break;
        } else if (b[3] == 2) {
            base = ((uint32_t) b[4] << 8 | b[5]) << 4;
        } else if (b[3] == 4) {
            base = ((uint32_t) b[4] << 8 | b[5]) << 16;
        }
    }
    fclose(f);
    return top;
}

static void no_sleep(avr_t *a, avr_cycle_count_t how_long)
{
    // simavr would sleep in real time here; simulated time is all that counts.
}

// ---- main -------------------------------------------------------------------
static double arg_num(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "sign-emu: %s needs a value\n", argv[*i]);
        exit(2);
    }
    return atof(argv[++*i]);
}

static const char *arg_str(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "sign-emu: %s needs a value\n", argv[*i]);
        exit(2);
    }
    return argv[++*i];
}

int main(int argc, char **argv)
{
    opt.headless = !isatty(STDOUT_FILENO);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--duration")) opt.duration_s = arg_num(argc, argv, &i);
        else if (!strcmp(a, "--until-requests")) opt.until_requests = (long) arg_num(argc, argv, &i);
        else if (!strcmp(a, "--speed")) { opt.speed = arg_num(argc, argv, &i); opt.speed_set = 1; }
        else if (!strcmp(a, "--headless")) opt.headless = 1;
        else if (!strcmp(a, "--band")) opt.band = 1;
        else if (!strcmp(a, "--record")) opt.gif = arg_str(argc, argv, &i);
        else if (!strcmp(a, "--png-dir")) opt.png_dir = arg_str(argc, argv, &i);
        else if (!strcmp(a, "--frames")) opt.events = arg_str(argc, argv, &i);
        else if (!strcmp(a, "--scale")) opt.scale = (int) arg_num(argc, argv, &i);
        else if (!strcmp(a, "--tick-cs")) opt.tick_cs = (int) arg_num(argc, argv, &i);
        else if (!strcmp(a, "--gamma")) opt.gamma = arg_num(argc, argv, &i);
        else if (!strcmp(a, "--port")) opt.port = (int) arg_num(argc, argv, &i);
        else if (!strcmp(a, "--wait-client")) opt.wait_client_s = (int) arg_num(argc, argv, &i);
        else if (!strcmp(a, "--link-latency-ms")) opt.latency_ms = arg_num(argc, argv, &i);
        else if (!strcmp(a, "--link-wait-ms")) opt.wait_ms = (int) arg_num(argc, argv, &i);
        else if (!strcmp(a, "--request-chars")) opt.request_chars = arg_str(argc, argv, &i);
        else if (!strcmp(a, "--bpm")) opt.bpm = arg_num(argc, argv, &i);
        else if (!strcmp(a, "--reset-us")) opt.reset_us = arg_num(argc, argv, &i);
        else if (!strcmp(a, "-v")) opt.verbose++;
        else if (!strcmp(a, "-vv")) opt.verbose += 2;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "sign-emu: unknown option %s\n\n", a); usage(stderr); return 2; }
        else opt.hex = a;
    }
    if (!opt.hex) {
        usage(stderr);
        return 2;
    }
    if (!opt.speed_set)
        opt.speed = opt.headless ? 0 : 1;

    // The simulated chip.
    avr = avr_make_mcu_by_name("atmega328p");
    if (!avr) {
        fprintf(stderr, "sign-emu: simavr has no atmega328p core\n");
        return 1;
    }
    avr_init(avr);
    avr->frequency = (uint32_t) F_CPU_HZ;
    avr->log = LOG_ERROR;
    avr->sleep = no_sleep;
    static uint8_t flash[32768];
    memset(flash, 0xff, sizeof(flash));
    long size = load_ihex(opt.hex, flash, sizeof(flash));
    if (size <= 0) {
        fprintf(stderr, "sign-emu: cannot read Intel HEX file %s\n", opt.hex);
        return 1;
    }
    avr_loadcode(avr, flash, (uint32_t) size, 0);
    avr->codeend = (uint32_t) size;

    // The world around it.
    view_set_gamma(opt.gamma);
    rec_set_layout(opt.band);
    view_set_band(opt.band);
    ws_init(avr, (uint32_t) (opt.reset_us * F_CPU_HZ / 1e6), on_frame);
    uart_init(avr, 14400.0, on_tx, on_rx);
    if (opt.bpm > 0) {
        g_beat_period = (uint64_t) (60.0 / opt.bpm * F_CPU_HZ);
        beat_set(0);
        avr_cycle_timer_register(avr, g_beat_period, beat_timer, NULL);
    }

    if (opt.gif && rec_gif_open(opt.gif, opt.scale, opt.tick_cs)) {
        fprintf(stderr, "sign-emu: cannot write %s: %s\n", opt.gif, strerror(errno));
        return 1;
    }
    if (opt.png_dir && rec_png_open(opt.png_dir, opt.scale, opt.tick_cs)) {
        fprintf(stderr, "sign-emu: cannot write into %s: %s\n", opt.png_dir, strerror(errno));
        return 1;
    }
    if (opt.events) {
        char meta[512];
        snprintf(meta, sizeof(meta),
                 "\"firmware\":\"%s\",\"f_cpu\":16000000,\"rows\":%d,\"cols\":%d,"
                 "\"panel_cols\":%d,\"reset_us\":%g,\"baud\":14400,\"link_latency_ms\":%g,"
                 "\"bpm\":%g,\"row_order\":\"rows[0] is the top row\","
                 "\"pixel_format\":\"6 hex digits RRGGBB per column, raw LED values\"",
                 opt.hex, ROWS, COLS, PANEL_COLS, opt.reset_us, opt.latency_ms, opt.bpm);
        if (rec_events_open(opt.events, meta)) {
            fprintf(stderr, "sign-emu: cannot write %s: %s\n", opt.events, strerror(errno));
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (opt.port > 0) {
        if (link_listen(opt.port)) {
            fprintf(stderr, "sign-emu: cannot listen on 127.0.0.1:%d: %s\n", opt.port, strerror(errno));
            return 1;
        }
        if (opt.wait_client_s > 0) {
            fprintf(stderr, "sign-emu: waiting for an app on tcp 127.0.0.1:%d ...\n", opt.port);
            double until = wall_now() + opt.wait_client_s;
            while (!g_stop && !link_wait_client(100) && wall_now() < until)
                ;
            if (!link_connected()) {
                fprintf(stderr, "sign-emu: no app connected within %d s\n", opt.wait_client_s);
                return 1;
            }
        }
    }

    if (!opt.headless)
        view_open();

    // Run. One slice is 1 ms of simulated time.
    const uint64_t slice = (uint64_t) (F_CPU_HZ / 1000);
    const uint64_t end_cycle = opt.duration_s > 0 ? (uint64_t) (opt.duration_s * F_CPU_HZ) : UINT64_MAX;
    double wall_start = wall_now();
    double pace_wall = wall_start, pace_sim = 0;        // pacing reference
    double meas_wall = wall_start, meas_sim = 0, speed_now = 0, last_draw = 0;
    int state = cpu_Running;
    uint8_t buf[4096];

    while (!g_stop && avr->cycle < end_cycle) {
        uint64_t target = avr->cycle + slice;
        if (target > end_cycle)
            target = end_cycle;
        while (avr->cycle < target && !g_stop) {
            state = avr_run(avr);
            if (state == cpu_Done || state == cpu_Crashed)
                break;
        }
        if (state == cpu_Done || state == cpu_Crashed)
            break;
        ws_poll(avr->cycle);

        // Bytes the app sent on its own (not as a held answer) go on the wire now.
        if (opt.port > 0) {
            size_t n = link_read(buf, sizeof(buf), 0);
            if (n)
                uart_rx_enqueue(buf, n, avr->cycle);
        }

        double sim = cycles_to_s(avr->cycle);
        double now = wall_now();
        if (g_wall_blocked > 0) {       // time spent holding for the app is not ours
            pace_wall += g_wall_blocked;
            meas_wall += g_wall_blocked;
            wall_start += g_wall_blocked;
            g_wall_blocked = 0;
        }
        if (opt.speed > 0) {
            double due = pace_wall + (sim - pace_sim) / opt.speed;
            if (due > now) {
                struct timespec ts;
                double d = due - now;
                ts.tv_sec = (time_t) d;
                ts.tv_nsec = (long) ((d - (double) ts.tv_sec) * 1e9);
                nanosleep(&ts, NULL);
                now = wall_now();
            } else if (now - due > 0.25) {
                pace_wall = now;        // fell behind: do not sprint to catch up
                pace_sim = sim;
            }
        }
        if (now - meas_wall >= 0.5) {
            speed_now = (sim - meas_sim) / (now - meas_wall);
            meas_wall = now;
            meas_sim = sim;
        }
        if (!opt.headless && now - last_draw >= 1.0 / 60) {
            view_status_t st = {
                .sim_s = sim, .speed_now = speed_now, .speed_target = opt.speed,
                .frames = g_frames, .requests = g_requests, .rx_bytes = g_rx_bytes,
                .port = opt.port, .client = link_connected(),
                .last_tx = g_last_tx, .last_rx = g_last_rx,
                .bpm = opt.bpm, .beat_on = g_beat_on,
                .marginal = g_marginal, .partial = g_partial,
            };
            view_draw(ws_image(), &st);
            last_draw = now;
        }
    }

    double wall = wall_now() - wall_start;
    double sim = cycles_to_s(avr->cycle);
    if (!opt.headless)
        view_close();
    rec_close(avr->cycle);
    link_close();

    FILE *out = stderr;
    fprintf(out, "sign-emu: simulated %.3f s in %.3f s of real time: %.2fx real time%s\n", sim, wall,
            wall > 0 ? sim / wall : 0, opt.speed > 0 ? " (paced)" : " (unpaced)");
    fprintf(out, "sign-emu: frames latched %llu, bits per strip per frame %u..%u\n",
            (unsigned long long) g_frames, g_frames ? g_bits_min : 0, g_bits_max);
    fprintf(out, "sign-emu: WS2812 high times: 0-bit %u..%u cycles, 1-bit %u..%u cycles\n",
            g_t0h_max ? g_t0h_min : 0, g_t0h_max, g_t1h_max ? g_t1h_min : 0, g_t1h_max);
    fprintf(out, "sign-emu: WS2812 low gaps: longest inside a frame %.1f us, shortest between "
                 "frames %.1f us: these pictures hold for any latch time between the two "
                 "(used %.0f us)\n",
            g_gap_max / 16.0, ws_shortest_latch_gap() / 16.0, opt.reset_us);
    fprintf(out, "sign-emu: serial: sign sent %llu bytes (%llu requests), app sent %llu bytes\n",
            (unsigned long long) g_tx_bytes, (unsigned long long) g_requests,
            (unsigned long long) g_rx_bytes);
    if (g_marginal || g_partial || g_framing_errors)
        fprintf(out, "sign-emu: WARNING: %llu marginal WS2812 pulses, %llu frames with a partial "
                     "pixel, %llu serial framing errors\n",
                (unsigned long long) g_marginal, (unsigned long long) g_partial,
                (unsigned long long) g_framing_errors);
    if (state == cpu_Crashed) {
        fprintf(out, "sign-emu: the firmware CRASHED at pc=0x%04x (simavr stopped the CPU)\n", avr->pc);
        return 3;
    }
    if (state == cpu_Done) {
        fprintf(out, "sign-emu: the firmware stopped (sleep with interrupts off) at pc=0x%04x\n", avr->pc);
        return 3;
    }
    return 0;
}
