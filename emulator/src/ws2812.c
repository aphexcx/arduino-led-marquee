// WS2812 decoder: turns what the firmware does to PORTD into pictures.
//
// Nothing here knows what the firmware "means" to draw. It watches the port
// pins, times every high pulse in CPU cycles, and behaves like 7 strips of
// 120 WS2812 pixels:
//   - a high pulse is one bit; its width says 0 or 1
//   - bits fill pixels in order from the strip's input, 24 per pixel, G R B,
//     most significant bit first
//   - bits past the 120th pixel fall off the end of the strip
//   - pixels that were not addressed keep their colour
//   - a low gap longer than the reset time latches what was received
#include <string.h>

#include "emu.h"
#include "avr_ioport.h"

// A 0 bit is high for 5 cycles (312 ns), a 1 bit for 13 (812 ns) in this
// firmware. Real WS2812B parts read anything up to about 500 ns as 0 and
// anything from about 550 ns as 1.
#define ONE_MIN_CYCLES 9            // >= 562 ns reads as 1
#define CLEAN_ZERO_MAX_CYCLES 7     // <= 437 ns is a clean 0
#define CLEAN_ONE_MIN_CYCLES 10     // >= 625 ns is a clean 1

#define STRIP_MASK 0xFE             // PD1..PD7

typedef struct {
    uint64_t rise, fall;
    uint64_t nbits;
    uint8_t shadow[COLS * 3];       // bytes as received: G, R, B per pixel
} strip_t;

static avr_t *g_avr;
static uint32_t g_reset_cycles;
static ws_frame_cb_t g_cb;
static strip_t g_strip[8];          // indexed by port bit
static uint8_t g_last;              // last pin levels (masked)
static image_t g_image;
static ws_frame_info_t g_info;
static uint64_t g_any_fall;         // last falling edge on any strip
static uint32_t g_latch_gap_min = UINT32_MAX;

static void info_reset(void)
{
    memset(&g_info, 0, sizeof(g_info));
    g_info.t0h_min = g_info.t1h_min = UINT32_MAX;
}

// Copy what strip `bit` received into the picture. Returns 1 if it had data.
static int latch_strip(int bit)
{
    strip_t *s = &g_strip[bit];
    if (!s->nbits)
        return 0;
    uint32_t bits = s->nbits > COLS * 24 ? COLS * 24 : (uint32_t) s->nbits;   // at most 2880
    uint32_t pixels = bits / 24;
    int row = 7 - bit;
    for (uint32_t i = 0; i < pixels; i++) {
        g_image.px[row][i][0] = s->shadow[i * 3 + 1];   // R
        g_image.px[row][i][1] = s->shadow[i * 3 + 0];   // G
        g_image.px[row][i][2] = s->shadow[i * 3 + 2];   // B
    }
    if (s->nbits > g_info.bits)
        g_info.bits = s->nbits;
    if (bits % 24 > g_info.partial_bits)
        g_info.partial_bits = bits % 24;
    s->nbits = 0;
    return 1;
}

// A frame is the whole picture at one moment: every strip that has gone quiet
// for the latch time is taken into it together, and its stamp is the moment
// the last of them latched. Stamps never go backwards.
static void emit(uint64_t latch_cycle)
{
    static uint64_t newest;
    if (latch_cycle < newest)
        latch_cycle = newest;
    newest = latch_cycle;
    if (g_info.t0h_min == UINT32_MAX)
        g_info.t0h_min = 0;
    if (g_info.t1h_min == UINT32_MAX)
        g_info.t1h_min = 0;
    if (g_cb)
        g_cb(&g_image, latch_cycle, &g_info);
    info_reset();
}

// Latches every strip that holds data and has been low for longer than the
// latch time. Returns the latest of their latch moments, 0 if there was none.
static uint64_t latch_quiet_strips(uint64_t now)
{
    uint64_t latched_at = 0;
    for (int bit = 1; bit < 8; bit++) {
        strip_t *s = &g_strip[bit];
        // A strip that rose at this very cycle still counts as low until now.
        int high = (g_last & (1 << bit)) && s->rise != now;
        if (!s->nbits || high || now - s->fall <= g_reset_cycles)
            continue;
        uint64_t at = s->fall + g_reset_cycles;
        if (latch_strip(bit) && at > latched_at)
            latched_at = at;
    }
    return latched_at;
}

static void port_notify(struct avr_irq_t *irq, uint32_t value, void *param)
{
    uint8_t v = value & STRIP_MASK;
    uint8_t changed = v ^ g_last;
    if (!changed)
        return;
    g_last = v;
    uint64_t now = g_avr->cycle;
    uint8_t rising = changed & v, falling = changed & ~v;

    if (rising) {
        // Shortest low gap that counted as a latch. If this is far above the
        // latch time, the pictures do not depend on which WS2812 generation
        // (50 us or 280 us reset) the strips are.
        if (g_any_fall && now - g_any_fall > g_reset_cycles && now - g_any_fall < g_latch_gap_min)
            g_latch_gap_min = (uint32_t) (now - g_any_fall);
        int latch = 0;
        for (int bit = 1; bit < 8; bit++) {
            if (!(rising & (1 << bit)))
                continue;
            strip_t *s = &g_strip[bit];
            if (s->nbits) {
                uint64_t gap = now - s->fall;
                if (gap > g_reset_cycles)
                    latch = 1;                       // new data for a strip that had latched
                else if (gap > g_info.gap_max)
                    g_info.gap_max = (uint32_t) gap; // at most g_reset_cycles, which is 32 bits
            }
            s->rise = now;
        }
        if (latch) {
            // The picture that was complete before this new data began: all
            // quiet strips, not only the ones that happen to rise now.
            uint64_t latched_at = latch_quiet_strips(now);
            if (latched_at)
                emit(latched_at);
        }
    }
    if (falling) {
        g_any_fall = now;
        for (int bit = 1; bit < 8; bit++) {
            if (!(falling & (1 << bit)))
                continue;
            strip_t *s = &g_strip[bit];
            // A pin can stay high for longer than 32 bits of cycles (4.5 minutes).
            uint64_t high = now - s->rise;
            uint32_t width = high > UINT32_MAX ? UINT32_MAX : (uint32_t) high;
            int one = width >= ONE_MIN_CYCLES;
            if (s->nbits < COLS * 24) {
                uint8_t m = 0x80 >> (s->nbits & 7);
                if (one)
                    s->shadow[s->nbits >> 3] |= m;
                else
                    s->shadow[s->nbits >> 3] &= ~m;
            }
            s->nbits++;
            s->fall = now;
            if (one) {
                if (width < g_info.t1h_min) g_info.t1h_min = width;
                if (width > g_info.t1h_max) g_info.t1h_max = width;
            } else {
                if (width < g_info.t0h_min) g_info.t0h_min = width;
                if (width > g_info.t0h_max) g_info.t0h_max = width;
            }
            if (width > CLEAN_ZERO_MAX_CYCLES && width < CLEAN_ONE_MIN_CYCLES)
                g_info.marginal++;
        }
    }
}

void ws_poll(uint64_t now)
{
    // Latch only once every strip that holds data has been quiet long enough,
    // so one frame never comes out as two half pictures.
    uint64_t latched_at = 0;
    int pending = 0;
    for (int bit = 1; bit < 8; bit++) {
        strip_t *s = &g_strip[bit];
        if (!s->nbits)
            continue;
        pending = 1;
        if ((g_last & (1 << bit)) || now - s->fall <= g_reset_cycles)
            return;
    }
    if (!pending)
        return;
    for (int bit = 1; bit < 8; bit++) {
        strip_t *s = &g_strip[bit];
        uint64_t at = s->fall + g_reset_cycles;
        if (latch_strip(bit) && at > latched_at)
            latched_at = at;
    }
    emit(latched_at);
}

uint32_t ws_shortest_latch_gap(void)
{
    return g_latch_gap_min == UINT32_MAX ? 0 : g_latch_gap_min;
}

const image_t *ws_image(void)
{
    return &g_image;
}

int ws_init(avr_t *avr, uint32_t reset_cycles, ws_frame_cb_t cb)
{
    // Without this the emulator would run and never see a frame.
    avr_irq_t *port = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), IOPORT_IRQ_PIN_ALL);
    if (!port)
        return -1;
    g_avr = avr;
    g_reset_cycles = reset_cycles;
    g_cb = cb;
    memset(g_strip, 0, sizeof(g_strip));
    memset(&g_image, 0, sizeof(g_image));
    info_reset();
    avr_irq_register_notify(port, port_notify, NULL);
    return 0;
}
