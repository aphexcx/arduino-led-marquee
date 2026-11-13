// The serial link, at the pins.
//
// The firmware bit-bangs a UART with SoftwareSerial: it wiggles PB3 (Arduino
// pin 11) to transmit and samples PB2 (pin 10) from a pin-change interrupt to
// receive. This file is the wire and the UART at the other end of it:
//   - a driver puts replies on PB2 as correctly timed 8N1 bits in simulated
//     time, so the firmware's own receiver reads them. Bytes are put on the
//     wire whether or not the firmware is listening, as on the real sign.
//   - a listener on each line (PB3, and PB2 itself) finds start bits, samples
//     each bit in its middle and reports whole bytes. What is reported as
//     "sent to the sign" is therefore what was seen on the wire, with the
//     cycle its start bit really fell, not what the driver meant to send.
//
// Bit times are whole-number arithmetic on the CPU clock and the baud rate.
// One bit at 14400 baud is 1111.11 cycles, so every edge is due at the
// nearest whole cycle to its exact time, counted from the start of the run
// of back-to-back bytes it belongs to: the rounding never adds up. simavr
// acts on a due edge at the first instruction boundary that is not before it,
// which is where the firmware could first see it; that is the cycle the
// listener reports.
#include <string.h>

#include "emu.h"
#include "avr_ioport.h"

#define TX_BIT 3   // PB3, Arduino pin 11
#define RX_BIT 2   // PB2, Arduino pin 10

#define F_CPU_INT 16000000ull

static avr_t *g_avr;
static uint64_t g_baud;
static uart_tx_cb_t g_on_tx;
static uart_rx_cb_t g_on_rx;

// Cycles in `halves` half-bit times, to the nearest cycle.
static uint64_t half_bits(uint64_t halves)
{
    return (halves * F_CPU_INT + g_baud) / (2 * g_baud);
}

// ---- listening to a line ---------------------------------------------------
typedef struct listener_t {
    int level, busy, nbit;
    uint8_t byte;
    uint64_t start;                     // cycle the start bit fell
    void (*done)(struct listener_t *l, int framing_ok);
} listener_t;

static avr_cycle_count_t listen_sample(avr_t *avr, avr_cycle_count_t when, void *param)
{
    listener_t *l = param;
    if (l->nbit < 8) {
        if (l->level)
            l->byte |= 1 << l->nbit;
        l->nbit++;
        return l->start + half_bits(2 * (uint64_t) l->nbit + 3);   // middle of the next bit
    }
    l->busy = 0;                        // middle of the stop bit
    l->done(l, l->level);
    return 0;
}

static void listen_edge(listener_t *l, int level)
{
    if (!l->busy && l->level && !level) {
        l->busy = 1;
        l->start = g_avr->cycle;
        l->nbit = 0;
        l->byte = 0;
        avr_cycle_timer_register(g_avr, half_bits(3), listen_sample, l);
    }
    l->level = level;
}

// ---- TX: what the sign sends -------------------------------------------------
static void tx_done(listener_t *l, int framing_ok)
{
    // The byte ends 10 bit times after its start bit began: the first whole
    // cycle that is not before that.
    if (g_on_tx)
        g_on_tx(l->byte, framing_ok, l->start, l->start + (10 * F_CPU_INT + g_baud - 1) / g_baud);
}

static listener_t g_tx_line = { .level = 1, .done = tx_done };

static void tx_notify(struct avr_irq_t *irq, uint32_t value, void *param)
{
    listen_edge(&g_tx_line, value & 1);
}

// ---- RX: what is sent to the sign --------------------------------------------
// A queue of replies. Each keeps the cycle before which its first bit must
// not start, so a reply that is due later does not ride out on the tail of an
// earlier one, and a reply that is due while another is still going out waits
// its turn, as bytes do in a real UART's transmit buffer.
//
// The queue is what is on its way to the wire. It does not belong to a TCP
// connection: a reply that was handed over whole is sent at its moment even if
// the client that sent it has gone.
#define RXQ_SIZE 65536
#define RX_REPLIES 1024
static uint8_t g_rxq[RXQ_SIZE];
static size_t g_rx_head, g_rx_tail;     // head = next byte to send
static struct {
    size_t left;                        // bytes of this reply still to send
    uint64_t not_before;
} g_reply[RX_REPLIES];
static size_t g_reply_head, g_reply_tail;
static size_t g_rx_dropped;
static int g_rx_active, g_rx_phase;
static uint8_t g_rx_byte;               // the byte going out
static uint64_t g_rx_start;             // the cycle its start bit is due
static uint64_t g_run_start, g_run_bits; // the run of back-to-back bytes it is part of

static void rx_done(listener_t *l, int framing_ok)
{
    if (g_on_rx)
        g_on_rx(l->byte, framing_ok, l->start, g_rx_start, g_rx_byte);
}

static listener_t g_rx_line = { .level = 1, .done = rx_done };

static void rx_notify(struct avr_irq_t *irq, uint32_t value, void *param)
{
    listen_edge(&g_rx_line, value & 1);
}

static void rx_set(int level)
{
    // The "external" value is what simavr re-applies to this input pin every
    // time the firmware writes PORTB, so the level survives those writes.
    // uart_init() has checked that this port takes the request.
    avr_ioport_external_t e = {
        .name = 'B', .mask = 1 << RX_BIT, .value = (level ? 1 : 0) << RX_BIT };
    avr_ioctl(g_avr, AVR_IOCTL_IOPORT_SET_EXTERNAL('B'), &e);
    avr_raise_irq(avr_io_getirq(g_avr, AVR_IOCTL_IOPORT_GETIRQ('B'), RX_BIT), level ? 1 : 0);
}

size_t uart_rx_pending(void)
{
    return (g_rx_tail + RXQ_SIZE - g_rx_head) % RXQ_SIZE + (g_rx_active && g_rx_phase ? 1 : 0);
}

size_t uart_rx_dropped(void)
{
    return g_rx_dropped;
}

static avr_cycle_count_t rx_step(avr_t *avr, avr_cycle_count_t when, void *param)
{
    if (g_rx_phase == 0) {
        if (g_reply_head == g_reply_tail) {
            g_rx_active = 0;
            return 0;
        }
        if (when < g_reply[g_reply_head].not_before)
            return g_reply[g_reply_head].not_before;   // the line idles until then
        if (when != g_run_start + half_bits(2 * g_run_bits)) {
            g_run_start = when;                        // not back to back: a new run
            g_run_bits = 0;
        }
        g_rx_byte = g_rxq[g_rx_head];
        g_rx_head = (g_rx_head + 1) % RXQ_SIZE;
        if (--g_reply[g_reply_head].left == 0)
            g_reply_head = (g_reply_head + 1) % RX_REPLIES;
        g_rx_start = when;
        rx_set(0);                                   // start bit
    } else if (g_rx_phase <= 8) {
        rx_set((g_rx_byte >> (g_rx_phase - 1)) & 1); // data, LSB first
    } else if (g_rx_phase == 9) {
        rx_set(1);                                   // stop bit
    } else {
        g_rx_phase = 0;
        return rx_step(avr, when, param);            // next byte, or wait for the next reply
    }
    g_rx_phase++;
    g_run_bits++;
    return g_run_start + half_bits(2 * g_run_bits);
}

void uart_rx_enqueue(const uint8_t *buf, size_t n, uint64_t not_before_cycle)
{
    size_t stored = 0;
    size_t next_reply = (g_reply_tail + 1) % RX_REPLIES;
    if (next_reply == g_reply_head) {
        g_rx_dropped += n;                           // too many replies waiting
        return;
    }
    for (; stored < n; stored++) {
        size_t next = (g_rx_tail + 1) % RXQ_SIZE;
        if (next == g_rx_head)
            break;                                   // queue full: the rest is lost
        g_rxq[g_rx_tail] = buf[stored];
        g_rx_tail = next;
    }
    g_rx_dropped += n - stored;
    if (!stored)
        return;
    g_reply[g_reply_tail].left = stored;
    g_reply[g_reply_tail].not_before = not_before_cycle;
    g_reply_tail = next_reply;
    if (!g_rx_active) {
        g_rx_active = 1;
        g_rx_phase = 0;
        uint64_t now = g_avr->cycle;
        avr_cycle_timer_register(g_avr, not_before_cycle > now ? not_before_cycle - now : 1,
                                 rx_step, NULL);
    }
}

int uart_init(avr_t *avr, unsigned baud, uart_tx_cb_t on_tx, uart_rx_cb_t on_rx)
{
    g_avr = avr;
    g_baud = baud;
    g_on_tx = on_tx;
    g_on_rx = on_rx;
    // Without these the emulator would run and simply never see or send a
    // byte, so their absence is an error, not something to carry on from.
    avr_irq_t *tx = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), TX_BIT);
    avr_irq_t *rx = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), RX_BIT);
    avr_ioport_external_t idle = { .name = 'B', .mask = 1 << RX_BIT, .value = 1 << RX_BIT };
    if (!baud || !tx || !rx || avr_ioctl(avr, AVR_IOCTL_IOPORT_SET_EXTERNAL('B'), &idle) != 0)
        return -1;
    avr_irq_register_notify(tx, tx_notify, NULL);
    avr_irq_register_notify(rx, rx_notify, NULL);
    rx_set(1);                                       // line idles high
    return 0;
}
