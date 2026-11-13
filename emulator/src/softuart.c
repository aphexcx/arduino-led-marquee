// The serial link, at the pins.
//
// The firmware bit-bangs a UART with SoftwareSerial: it wiggles PB3 (Arduino
// pin 11) to transmit and samples PB2 (pin 10) from a pin-change interrupt to
// receive. This file is the wire and the UART at the other end of it:
//   - TX: watches PB3, finds the start bit, samples each bit in the middle,
//     hands whole bytes to on_tx
//   - RX: drives PB2 with correctly timed 8N1 bits in simulated time, so the
//     firmware's own receiver reads them. Bytes are put on the wire whether or
//     not the firmware is listening, as on the real sign.
#include <string.h>

#include "emu.h"
#include "avr_ioport.h"

#define TX_BIT 3   // PB3, Arduino pin 11
#define RX_BIT 2   // PB2, Arduino pin 10

static avr_t *g_avr;
static double g_bit_cycles;
static uart_tx_cb_t g_on_tx;
static uart_rx_cb_t g_on_rx;

// ---- TX decode ------------------------------------------------------------
static int g_tx_level = 1, g_tx_busy, g_tx_nbit;
static uint8_t g_tx_byte;
static uint64_t g_tx_start;

static avr_cycle_count_t tx_sample(avr_t *avr, avr_cycle_count_t when, void *param)
{
    if (g_tx_nbit < 8) {
        if (g_tx_level)
            g_tx_byte |= 1 << g_tx_nbit;
        g_tx_nbit++;
        return g_tx_start + (uint64_t) ((g_tx_nbit + 1.5) * g_bit_cycles);
    }
    // Middle of the stop bit.
    g_tx_busy = 0;
    if (g_on_tx)
        g_on_tx(g_tx_byte, g_tx_level, g_tx_start,
                g_tx_start + (uint64_t) (10 * g_bit_cycles));
    return 0;
}

static void tx_notify(struct avr_irq_t *irq, uint32_t value, void *param)
{
    int level = value & 1;
    if (!g_tx_busy && g_tx_level && !level) {
        g_tx_busy = 1;
        g_tx_start = g_avr->cycle;
        g_tx_nbit = 0;
        g_tx_byte = 0;
        avr_cycle_timer_register(g_avr, (avr_cycle_count_t) (1.5 * g_bit_cycles),
                                 tx_sample, NULL);
    }
    g_tx_level = level;
}

// ---- RX drive -------------------------------------------------------------
// A queue of replies. Each keeps the cycle before which its first bit must
// not start, so a reply that is due later does not ride out on the tail of an
// earlier one, and a reply that is due while another is still going out waits
// its turn, as bytes do in a real UART's transmit buffer.
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
static uint8_t g_rx_byte;
static uint64_t g_rx_start;

static void rx_set(int level)
{
    // The "external" value is what simavr re-applies to this input pin every
    // time the firmware writes PORTB, so the level survives those writes.
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
        if (g_on_rx)
            g_on_rx(g_rx_byte, g_rx_start);
        g_rx_phase = 0;
        return rx_step(avr, when, param);            // next byte, or wait for the next reply
    }
    g_rx_phase++;
    return g_rx_start + (uint64_t) (g_rx_phase * g_bit_cycles + 0.5);
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

void uart_init(avr_t *avr, double baud, uart_tx_cb_t on_tx, uart_rx_cb_t on_rx)
{
    g_avr = avr;
    g_bit_cycles = F_CPU_HZ / baud;
    g_on_tx = on_tx;
    g_on_rx = on_rx;
    avr_irq_register_notify(
        avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), TX_BIT), tx_notify, NULL);
    rx_set(1);                                       // line idles high
}
