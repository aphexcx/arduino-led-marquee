// Intel HEX reader for the firmware image.
#include <stdio.h>
#include <string.h>

#include "emu.h"

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// One record: ":" then length, address (2), type, payload, checksum, in hex.
// Returns the number of bytes decoded into b, or -1 if the line is not a
// well-formed record (bad hex, wrong length, wrong checksum).
static int read_record(const char *line, uint8_t b[260])
{
    int n = 0;
    const char *p = line + 1;
    while (n < 260 && hexval(p[0]) >= 0 && hexval(p[1]) >= 0) {
        b[n++] = (uint8_t) (hexval(p[0]) << 4 | hexval(p[1]));
        p += 2;
    }
    if (*p && *p != '\r' && *p != '\n')
        return -1;                       // odd digit, or something that is not hex
    if (n < 5 || n != b[0] + 5)
        return -1;
    uint8_t sum = 0;
    for (int i = 0; i < n; i++)
        sum += b[i];
    return sum ? -1 : n;
}

// Data (00), end of file (01), extended segment address (02) and extended
// linear address (04) records. Start-address records (03, 05) carry nothing
// to load and are skipped. Anything that would land outside mem is an error,
// and so is a file without an end-of-file record: a file cut short must not
// load as a smaller firmware.
long load_ihex(const char *path, uint8_t *mem, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[600];
    uint64_t base = 0;
    long top = 0;
    int ok = 1, ended = 0;
    while (ok && !ended && fgets(line, sizeof(line), f)) {
        if (line[0] != ':') {
            // Blank lines are harmless. Anything else is not a HEX file, or
            // is one that has been damaged.
            if (line[strspn(line, " \t\r\n")])
                ok = 0;
            continue;
        }
        uint8_t b[260];
        if (read_record(line, b) < 0) {
            ok = 0;
            break;
        }
        size_t len = b[0];
        const uint8_t *payload = b + 4;
        if (b[3] == 0) {
            uint64_t addr = base + ((uint32_t) b[1] << 8 | b[2]);
            if (addr > cap || len > cap - addr) {
                ok = 0;
                break;
            }
            memcpy(mem + addr, payload, len);
            if ((long) (addr + len) > top)
                top = (long) (addr + len);
        } else if (b[3] == 1) {
            ended = 1;
        } else if (b[3] == 2 || b[3] == 4) {
            if (len != 2) {
                ok = 0;
                break;
            }
            base = (uint64_t) (payload[0] << 8 | payload[1]) << (b[3] == 2 ? 4 : 16);
        }
    }
    if (ferror(f) || !ended)
        ok = 0;                          // a read error, or a file cut short: no end record
    fclose(f);
    return ok ? top : -1;
}
