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
// a line the emulator consumes instead of putting it on the wire:
//   @<ms>\n     put this reply's first bit on the wire <ms> milliseconds of
//               simulated time after its request, instead of --link-latency-ms
//   @none\n     this request gets no reply
// A directive is only recognised where a reply would start.
#include <stdlib.h>
#include <string.h>

#include "emu.h"

static struct {
    int directives;
    uint8_t data[REPLY_MAX];
    size_t n;
    int blank;                        // data holds only whitespace so far
    int depth, in_string, escape;     // JSON state inside data
    int in_directive;
    char directive[32];
    size_t directive_n;
    int has_delay;
    double delay_ms;
} g;

void reply_init(int directives)
{
    memset(&g, 0, sizeof(g));
    g.directives = directives;
    g.blank = 1;
}

static void emit(reply_cb_t cb, void *param, int complete)
{
    cb(g.data, g.n, g.has_delay, g.delay_ms, complete, param);
    g.n = 0;
    g.blank = 1;
    g.has_delay = 0;
    g.delay_ms = 0;
}

static void end_directive(reply_cb_t cb, void *param)
{
    g.directive[g.directive_n] = 0;
    g.in_directive = 0;
    g.directive_n = 0;
    if (!strcmp(g.directive, "none")) {
        g.has_delay = 0;
        g.delay_ms = 0;
        cb(g.data, 0, 0, 0, 1, param);   // a reply of no bytes; waiting whitespace stays
        return;
    }
    char *end;
    double ms = strtod(g.directive, &end);
    if (end != g.directive && !*end && ms >= 0 && ms < 1e9) {
        g.has_delay = 1;
        g.delay_ms = ms;
    }                                 // anything else is ignored
}

void reply_feed(const uint8_t *buf, size_t len, reply_cb_t cb, void *param)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (g.in_directive) {
            if (c == '\n')
                end_directive(cb, param);
            else if (c != '\r' && g.directive_n < sizeof(g.directive) - 1)
                g.directive[g.directive_n++] = (char) c;
            continue;
        }
        if (g.directives && c == '@' && g.blank && g.depth == 0 && !g.in_string) {
            g.in_directive = 1;
            continue;
        }
        if (g.n == REPLY_MAX) {       // absurdly long: pass it on in pieces
            int has_delay = g.has_delay;
            double delay_ms = g.delay_ms;
            emit(cb, param, 0);
            g.has_delay = has_delay;
            g.delay_ms = delay_ms;
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
}

// Passes on what has arrived of a reply that is not complete. Whitespace
// alone stays, and so does a directive that is waiting for its reply.
void reply_flush(reply_cb_t cb, void *param)
{
    if (g.n && !g.blank)
        emit(cb, param, 0);
}
