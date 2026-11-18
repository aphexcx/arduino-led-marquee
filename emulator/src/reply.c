// Cuts the byte stream from the app into replies.
//
// TCP delivers the app's bytes in pieces of any size, and the pieces say
// nothing about what the app meant. The link's timing model needs to know
// where one reply ends: a reply is one JSON object, so this follows the
// brackets and strings of the stream and reports each object as it closes.
// Whitespace between objects (the newline some programs send after a reply)
// travels in front of the next reply; it is never put on the wire by itself,
// because when it would go would depend on how TCP happened to cut the stream.
//
// With directives on (sign-emu --link-directives), a reply may be preceded by
// lines the emulator consumes instead of putting them on the wire:
//   @<ms>\n       put this reply's first bit on the wire <ms> milliseconds of
//                 simulated time after its request, instead of --link-latency-ms
//   @none\n       this request gets no reply
//   @break<ms>\n  hold the line to the sign LOW for <ms> milliseconds (a UART
//                 break). It is an item of its own, in its place in the stream:
//                 a "@<ms>" or "@after<ms>" line in front of it applies to it.
//   @after<ms>\n  the next item (a reply or a break) starts no sooner than <ms>
//                 milliseconds after the item before it ended
//   @batch\n      in front of a reply: the clock stays held for the request
//   @end\n        after that reply, until this line has arrived. Everything in
//                 between is then placed with the clock standing still, however
//                 TCP cut the bytes into pieces.
// A directive is only recognised where a reply would start, and only if it is
// exactly one of those six forms. Anything else that starts with @ is not a
// directive: its bytes go to the sign like any others, from the first byte
// that shows it. So a mistyped directive is seen on the wire, and never
// swallows what follows it.
//
// Lifetimes. All of this state belongs to one TCP connection: when the client
// changes, reply_reset() drops it, so what one client left unfinished is not
// the start of the next client's first reply. Within a connection, the JSON
// nesting state belongs to one exchange (a request and its reply):
// reply_new_exchange() ends it.
#include <stdlib.h>
#include <string.h>

#include "emu.h"

#define DIRECTIVE_MAX 20             // a word and a number of milliseconds

static struct {
    int directives;
    uint8_t data[REPLY_MAX];
    size_t n;
    int blank;                        // data holds only whitespace so far
    int depth, in_string, escape;     // JSON state inside data
    int in_directive;
    char directive[DIRECTIVE_MAX + 1];
    size_t directive_n;
    int has_delay;
    double delay_ms;
    int has_after;
    double after_ms;
} g;

void reply_init(int directives)
{
    memset(&g, 0, sizeof(g));
    g.directives = directives;
    g.blank = 1;
}

void reply_reset(void)
{
    reply_init(g.directives);
}

// Hands over what is in data (n bytes of it), or a break when break_ms is
// more than 0, with the delay and the gap that were waiting for the next item.
static void hand_over(reply_cb_t cb, void *param, size_t n, int complete, double break_ms)
{
    reply_t r = { .data = g.data, .n = n, .complete = complete,
                  .has_delay = g.has_delay, .delay_ms = g.delay_ms,
                  .has_after = g.has_after, .after_ms = g.after_ms, .break_ms = break_ms };
    g.has_delay = 0;
    g.delay_ms = 0;
    g.has_after = 0;
    g.after_ms = 0;
    cb(&r, param);
}

static void emit(reply_cb_t cb, void *param, int complete)
{
    hand_over(cb, param, g.n, complete, 0);
    g.n = 0;
    g.blank = 1;
}

// One byte of what goes to the sign.
static void data_byte(uint8_t c, reply_cb_t cb, void *param)
{
    if (g.n == REPLY_MAX) {           // absurdly long: pass it on in pieces
        int has_delay = g.has_delay;
        double delay_ms = g.delay_ms;
        emit(cb, param, 0);
        g.has_delay = has_delay;      // every piece is placed like the first;
        g.delay_ms = delay_ms;        // the gap in front belongs to the first only
    }
    g.data[g.n++] = c;
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        g.blank = 0;
    if (g.in_string) {
        if (g.escape)
            g.escape = 0;
        else if (c == '\\')
            g.escape = 1;
        else if (c == '"')
            g.in_string = 0;
    } else if (c == '"' && g.depth > 0) {
        g.in_string = 1;
    } else if (c == '{' || c == '[') {
        g.depth++;
    } else if ((c == '}' || c == ']') && g.depth > 0) {
        if (--g.depth == 0)
            emit(cb, param, 1);
    }
}

// What looked like a directive is not one: its bytes are data after all.
static void not_a_directive(reply_cb_t cb, void *param)
{
    size_t n = g.directive_n;
    g.in_directive = 0;
    g.directive_n = 0;
    data_byte('@', cb, param);
    for (size_t i = 0; i < n; i++)
        data_byte((uint8_t) g.directive[i], cb, param);
}

// What a directive line says.
enum { D_DELAY, D_NONE, D_BREAK, D_AFTER, D_BATCH, D_END };

// "none", or digits with at most one point among them, with "break" or
// "after" in front or nothing. Returns 0 if the text is none of those.
static int directive_value(const char *text, int *kind, double *ms)
{
    *kind = D_DELAY;
    if (!strcmp(text, "none")) {
        *kind = D_NONE;
        return 1;
    }
    if (!strcmp(text, "batch") || !strcmp(text, "end")) {
        *kind = text[0] == 'b' ? D_BATCH : D_END;
        return 1;
    }
    if (!strncmp(text, "break", 5)) {
        *kind = D_BREAK;
        text += 5;
    } else if (!strncmp(text, "after", 5)) {
        *kind = D_AFTER;
        text += 5;
    }
    int digits = 0, points = 0;
    for (const char *p = text; *p; p++) {
        if (*p >= '0' && *p <= '9')
            digits++;
        else if (*p == '.')
            points++;
        else
            return 0;
    }
    if (!digits || points > 1)
        return 0;
    *ms = strtod(text, NULL);
    if (*kind == D_BREAK && !(*ms > 0))
        return 0;                     // a break of no length is not a break
    return *ms <= 1e9;                // 11 days: keeps the cycle count far inside 64 bits
}

static void end_directive(reply_cb_t cb, void *param)
{
    int kind;
    double ms = 0;
    char value[DIRECTIVE_MAX + 1];
    size_t n = g.directive_n;
    if (n && g.directive[n - 1] == '\r')
        n--;                          // the line ended \r\n
    memcpy(value, g.directive, n);
    value[n] = 0;
    if (!directive_value(value, &kind, &ms)) {
        not_a_directive(cb, param);
        data_byte('\n', cb, param);
        return;
    }
    g.in_directive = 0;
    g.directive_n = 0;
    if (kind == D_NONE) {
        g.has_delay = 0;
        g.delay_ms = 0;
        g.has_after = 0;
        g.after_ms = 0;
        hand_over(cb, param, 0, 1, 0);   // a reply of no bytes; waiting whitespace stays
    } else if (kind == D_BREAK) {
        // Not a reply: a request that is waiting for its answer goes on
        // waiting. Waiting whitespace stays for the reply that follows.
        hand_over(cb, param, 0, 0, ms);
    } else if (kind == D_AFTER) {
        g.has_after = 1;
        g.after_ms = ms;
    } else if (kind == D_BATCH || kind == D_END) {
        // A mark, not an item: a delay or a gap that is waiting stays for the
        // item that follows.
        reply_t r = { .data = g.data, .batch = kind == D_BATCH, .end = kind == D_END };
        cb(&r, param);
    } else {
        g.has_delay = 1;
        g.delay_ms = ms;
    }
}

void reply_feed(const uint8_t *buf, size_t len, reply_cb_t cb, void *param)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (g.in_directive) {
            if (c == '\n') {
                end_directive(cb, param);
            } else if (g.directive_n < DIRECTIVE_MAX &&
                       ((c >= '0' && c <= '9') || c == '.' || (c >= 'a' && c <= 'z') ||
                        c == '\r')) {
                // \r is kept: only the line end tells whether it was "\r\n".
                g.directive[g.directive_n++] = (char) c;
            } else {
                not_a_directive(cb, param);   // too long, or a byte no directive has
                data_byte(c, cb, param);
            }
            continue;
        }
        if (g.directives && c == '@' && g.blank && g.depth == 0 && !g.in_string) {
            g.in_directive = 1;
            continue;
        }
        data_byte(c, cb, param);
    }
}

// The sign has sent a new request, or has given up waiting for the answer to
// the last one. Whatever JSON object was open is over as far as finding the
// end of the next reply goes: without this, one reply cut short (inside a
// string, say) would leave every later reply looking unfinished. A delay
// given for a reply that then did not come in time is dropped with it.
void reply_new_exchange(int drop_delay)
{
    g.depth = 0;
    g.in_string = 0;
    g.escape = 0;
    if (drop_delay) {
        g.has_delay = 0;
        g.delay_ms = 0;
        g.has_after = 0;
        g.after_ms = 0;
    }
}

void reply_give_up(reply_cb_t cb, void *param)
{
    if (g.in_directive)
        not_a_directive(cb, param);   // it never got its newline: ordinary bytes
    if (g.n && !g.blank)
        emit(cb, param, 0);
    reply_new_exchange(1);
}

// Passes on what has arrived of a reply that is not complete. Whitespace
// alone stays, and so does a directive that is waiting for its reply or for
// the end of its own line.
void reply_flush(reply_cb_t cb, void *param)
{
    if (g.n && !g.blank)
        emit(cb, param, 0);
}
