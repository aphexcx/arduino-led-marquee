// Tests for the Intel HEX loader (src/ihex.c). Built with
// -fsanitize=address,undefined by the Makefile, so a write outside the flash
// buffer or a read of uninitialised record bytes stops the test.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu.h"

static int failures;
static const char *g_dir;

#define CAP 32768

static long load(const char *name, const char *text, uint8_t *mem)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.hex", g_dir, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
    memset(mem, 0xff, CAP);
    return load_ihex(path, mem, CAP);
}

static void expect(const char *name, const char *text, long want, const char *why)
{
    // The buffer is exactly CAP bytes on the heap: one byte past either end is
    // an AddressSanitizer error.
    uint8_t *mem = malloc(CAP);
    long got = load(name, text, mem);
    int ok = got == want;
    printf("  %s  %-24s returns %ld (want %ld): %s\n", ok ? "ok  " : "FAIL", name, got, want, why);
    if (!ok)
        failures++;
    free(mem);
}

int main(int argc, char **argv)
{
    g_dir = argc > 1 ? argv[1] : ".";
    uint8_t *mem = malloc(CAP);

    // A good file: four bytes at 0, two at 0x10.
    long top = load("good", ":0400000001020304F2\n:02001000AABB89\n:00000001FF\n", mem);
    int ok = top == 0x12 && mem[0] == 1 && mem[3] == 4 && mem[0x10] == 0xaa && mem[0x11] == 0xbb &&
             mem[4] == 0xff;
    printf("  %s  %-24s loads its bytes at their addresses (top %ld)\n", ok ? "ok  " : "FAIL",
           "good", top);
    if (!ok)
        failures++;
    free(mem);

    // The gate's sequence: an extended linear address of 0xFFFF, then one data
    // byte at 0xFFFF. The address is 0xFFFFFFFF; address + length wraps to 0.
    expect("address-wraps", ":02000004FFFFFC\n:01FFFF000001\n:00000001FF\n", -1,
           "data at 0xFFFFFFFF is outside the flash");
    // Address records with no payload: the two address bytes are not there.
    expect("type04-empty", ":00000004FC\n:0100000055AA\n:00000001FF\n", -1,
           "extended linear address record without its two bytes");
    expect("type02-empty", ":00000002FE\n:0100000055AA\n:00000001FF\n", -1,
           "extended segment address record without its two bytes");
    expect("type04-short", ":0100000400FB\n:00000001FF\n", -1,
           "extended linear address record with one byte");
    // The edges of the flash.
    expect("last-byte", ":017FFF005A27\n:00000001FF\n", 0x8000, "one byte at the last flash address");
    expect("one-past", ":018000005A25\n:00000001FF\n", -1, "one byte just past the flash");
    expect("straddles", ":027FFF0011115E\n:00000001FF\n", -1, "two bytes across the end of the flash");
    expect("segment-high", ":020000021000EC\n:0100000055AA\n:00000001FF\n", -1,
           "segment 0x1000 puts address 0 at 0x10000");
    expect("bad-checksum", ":0400000001020304F3\n:00000001FF\n", -1, "wrong checksum");
    expect("bad-length", ":0400000001020304\n:00000001FF\n", -1, "record shorter than its length byte says");

    if (failures) {
        printf("FAILED: %d ihex check(s)\n", failures);
        return 1;
    }
    printf("ihex: all checks passed\n");
    return 0;
}
