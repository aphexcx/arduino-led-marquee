// Shared declarations for the sign emulator. See ../README.md.
#ifndef SIGN_EMU_H
#define SIGN_EMU_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "sim_avr.h"

// ---- The sign -------------------------------------------------------------
// 7 WS2812 strips, one per LED row, on PORTD bits 1..7. Fonts.h: "Bit #7 is
// top line of char, Bit #1 is bottom", so PD7 drives the top row.
// Each strip is 120 pixels long: panel A is pixels 0..59 (nearest the
// Arduino), panel B is pixels 60..119.
#define ROWS 7
#define COLS 120
#define PANEL_COLS 60
#define ROW_TO_PORT_BIT(row) (7 - (row))   // row 0 = top = PD7

#define F_CPU_HZ 16000000.0

typedef struct {
    uint8_t px[ROWS][COLS][3];   // [row][column] = {R, G, B} as the LED holds it
} image_t;

static inline double cycles_to_s(uint64_t c) { return (double) c / F_CPU_HZ; }

// ---- ihex.c ---------------------------------------------------------------
// Reads an Intel HEX file into mem[0..cap). Returns the address one past the
// highest byte loaded, or -1 if the file cannot be read, is malformed, or
// places data outside mem.
long load_ihex(const char *path, uint8_t *mem, size_t cap);

// ---- ws2812.c -------------------------------------------------------------
typedef struct {
    uint64_t bits;          // most bits clocked into any one strip since the last latch
    uint32_t partial_bits;  // bits left over that did not make a whole 24-bit pixel
    uint32_t t0h_min, t0h_max, t1h_min, t1h_max;   // high times seen, in CPU cycles
    uint64_t marginal;      // pulses whose width is between a clean 0 and a clean 1
    uint32_t gap_max;       // longest low gap between two bits of this frame, in cycles
} ws_frame_info_t;

typedef void (*ws_frame_cb_t)(const image_t *img, uint64_t latch_cycle,
                              const ws_frame_info_t *info);

int ws_init(avr_t *avr, uint32_t reset_cycles, ws_frame_cb_t cb);   // 0, or -1: no port D
void ws_poll(uint64_t now);          // latch on a quiet line; call regularly
const image_t *ws_image(void);       // what the LEDs show right now
uint32_t ws_shortest_latch_gap(void); // shortest low gap that latched, in cycles (0 = none)

// ---- softuart.c -----------------------------------------------------------
// The firmware's SoftwareSerial: TX on Arduino pin 11 (PB3), RX on pin 10 (PB2).
typedef void (*uart_tx_cb_t)(uint8_t byte, int framing_ok, uint64_t start_cycle,
                             uint64_t end_cycle);
// A byte seen on the line to the sign: what a listener on the pin decoded and
// the cycle its start bit fell, then the cycle the driver had it due and the
// byte the driver was given.
typedef void (*uart_rx_cb_t)(uint8_t byte, int framing_ok, uint64_t start_cycle,
                             uint64_t due_cycle, uint8_t sent);

// 0, or -1 if the simulated chip has no port B pins to attach to.
int uart_init(avr_t *avr, unsigned baud, uart_tx_cb_t on_tx, uart_rx_cb_t on_rx);
// Queue one reply for the firmware's RX pin. Replies go out in the order they
// were queued, as from one UART: each one's bytes back to back, 8N1, its first
// start bit no earlier than not_before_cycle and not before the reply ahead of
// it has finished.
void uart_rx_enqueue(const uint8_t *buf, size_t n, uint64_t not_before_cycle);
size_t uart_rx_pending(void);
size_t uart_rx_dropped(void);                 // bytes lost because the queue was full

// ---- reply.c --------------------------------------------------------------
// Cuts the app's byte stream into replies (one JSON object each).
#define REPLY_MAX 8192
// data[0..n) is one reply, or a piece of one when complete is 0. has_delay:
// a directive gave this reply its own delay. n can be 0 (directive "none").
typedef void (*reply_cb_t)(const uint8_t *data, size_t n, int has_delay, double delay_ms,
                           int complete, void *param);
void reply_init(int directives);
// Forgets everything unfinished: a directive in progress or waiting for its
// reply, a partly received reply, whitespace. For when the client changes.
void reply_reset(void);
// A new request, or the wait for the last one's answer ran out: the next
// reply starts from outside any JSON object. drop_delay also forgets a
// directive's delay that was waiting for a reply.
void reply_new_exchange(int drop_delay);
void reply_feed(const uint8_t *buf, size_t len, reply_cb_t cb, void *param);
void reply_flush(reply_cb_t cb, void *param);

// ---- link.c ---------------------------------------------------------------
int link_listen(int port);                    // 127.0.0.1:port; 0 on success
// cb is called whenever the client changes: it closed, a read or write on it
// failed, or a new one connected (connected = 1), replacing any old one.
typedef void (*link_change_cb_t)(int connected);
void link_on_change(link_change_cb_t cb);
int link_connected(void);
int link_wait_client(int timeout_ms);         // 1 if a client is connected
void link_send(uint8_t byte);
// Non-blocking (timeout_ms == 0) or blocking read of whatever the client sent.
// Returns the number of bytes placed in buf.
size_t link_read(uint8_t *buf, size_t cap, int timeout_ms);
void link_close(void);

// ---- view.c ---------------------------------------------------------------
typedef struct {
    double sim_s;
    double speed_now;        // simulated seconds per wall second, last interval
    double speed_target;     // 0 = unpaced
    uint64_t frames;
    uint64_t requests;
    uint64_t rx_bytes;
    int port;
    int client;
    const char *last_tx;     // short printable tail of what the sign sent
    const char *last_rx;     // short printable tail of what was sent to the sign
    double bpm;
    int beat_on;
    uint64_t marginal;
    uint64_t partial;
} view_status_t;

void view_set_gamma(double gamma);
double view_get_gamma(void);
void view_map(const uint8_t led[3], uint8_t screen[3]);   // LED value -> screen colour
void view_set_band(int band);
void view_open(void);
void view_draw(const image_t *img, const view_status_t *st);
void view_close(void);

// ---- record.c -------------------------------------------------------------
// Before any output is opened: layout (1 = one 120 x 7 band, 0 = two stacked
// sides), screen pixels per LED, and the GIF/PNG tick in 1/100 s.
void rec_configure(int band, int scale, int tick_cs);
// The three opens return 0, or -1 with the reason in rec_error().
int rec_gif_open(const char *path);
int rec_png_open(const char *dir);
int rec_events_open(const char *path, const char *firmware, const char *settings_json);
void rec_frame(const image_t *img, uint64_t latch_cycle, const ws_frame_info_t *info);
void rec_event(const char *type, uint64_t cycle, const char *extra_json);
int rec_close(uint64_t end_cycle);            // 0, or -1 if any output failed
int rec_failed(void);                         // an output has failed: stop the run
const char *rec_error(void);                  // the first failure: path and reason

#endif
