#!/usr/bin/env python3
"""Decodes every frame of a GIF written by the emulator and compares it,
pixel for pixel and delay for delay, with the pictures redrawn from the frames
dump of the same run.

The LZW decoder here is strict: it follows the GIF89a rules for when the code
width grows, and any code a conforming decoder could not accept (a code beyond
the next free dictionary entry, data that runs out before the end code, bytes
left over after it, a pixel count that is not width x height) is an error.

usage: gif_check.py <clip.gif> <frames.jsonl>
Python 3 standard library only.
"""
import json
import math
import sys

COLS, PANEL, ROWS = 120, 60, 7
F_CPU = 16000000.0
OFF_COLOUR = (14, 14, 16)        # view.c: an LED that is off
BACKGROUND = (0, 0, 0)           # record.c: between and around the LEDs


class GifError(Exception):
    pass


def lzw_decode(min_code, data, pixels):
    """Strict GIF LZW. Returns the colour indices as bytes."""
    if not 2 <= min_code <= 8:
        raise GifError("minimum code size %d" % min_code)
    clear, end = 1 << min_code, (1 << min_code) + 1
    base = [bytes([i]) for i in range(clear)] + [None, None]
    table, size, prev = list(base), min_code + 1, None
    big, pos, total = int.from_bytes(data, "little"), 0, len(data) * 8
    out = bytearray()
    codes = 0
    while True:
        if pos + size > total:
            raise GifError("data ran out after %d codes, before an end code" % codes)
        code = (big >> pos) & ((1 << size) - 1)
        pos += size
        codes += 1
        if code == clear:
            table, size, prev = list(base), min_code + 1, None
            continue
        if code == end:
            break
        if prev is None:
            if code >= clear:
                raise GifError("code %d (number %d, %d bits) right after a clear code"
                               % (code, codes, size))
            entry = table[code]
        else:
            if code < len(table):
                entry = table[code]
            elif code == len(table) and len(table) < 4096:
                entry = prev + prev[:1]
            else:
                raise GifError("invalid code %d (number %d, read as %d bits): the dictionary "
                               "has %d entries" % (code, codes, size, len(table)))
            if len(table) < 4096:
                table.append(prev + entry[:1])
                if len(table) == (1 << size) and size < 12:
                    size += 1
        out += entry
        prev = entry
        if len(out) > pixels:
            raise GifError("more than %d pixels" % pixels)
    if total - pos >= 8:
        raise GifError("%d bytes left over after the end code" % ((total - pos) // 8))
    if len(out) != pixels:
        raise GifError("%d pixels, the image has %d" % (len(out), pixels))
    return bytes(out)


def read_gif(path):
    """Returns (width, height, [(delay_cs, (r_plane, g_plane, b_plane)), ...])."""
    data = open(path, "rb").read()
    if data[:6] != b"GIF89a":
        raise GifError("not a GIF89a file")
    width, height = data[6] | data[7] << 8, data[8] | data[9] << 8
    pos = 13
    global_palette = None
    if data[10] & 0x80:
        n = 3 << ((data[10] & 7) + 1)
        global_palette = data[pos:pos + n]
        pos += n
    frames, delay = [], 0

    def sub_blocks(p):
        chunks = []
        while data[p]:
            chunks.append(data[p + 1:p + 1 + data[p]])
            p += data[p] + 1
        return b"".join(chunks), p + 1

    while True:
        if pos >= len(data):
            raise GifError("no trailer")
        block = data[pos]
        if block == 0x3B:
            break
        if block == 0x21:
            if data[pos + 1] == 0xF9:
                delay = data[pos + 4] | data[pos + 5] << 8
            _, pos = sub_blocks(pos + 2)
        elif block == 0x2C:
            x, y = data[pos + 1] | data[pos + 2] << 8, data[pos + 3] | data[pos + 4] << 8
            w, h = data[pos + 5] | data[pos + 6] << 8, data[pos + 7] | data[pos + 8] << 8
            flags = data[pos + 9]
            pos += 10
            if (x, y, w, h) != (0, 0, width, height) or flags & 0x40:
                raise GifError("frame %d is not a full, non-interlaced image" % len(frames))
            palette = global_palette
            if flags & 0x80:
                n = 3 << ((flags & 7) + 1)
                palette = data[pos:pos + n]
                pos += n
            if palette is None:
                raise GifError("frame %d has no colour table" % len(frames))
            min_code = data[pos]
            packed, pos = sub_blocks(pos + 1)
            try:
                indices = lzw_decode(min_code, packed, width * height)
            except GifError as e:
                raise GifError("frame %d: %s" % (len(frames), e)) from None
            if max(indices) * 3 >= len(palette):
                raise GifError("frame %d uses a colour index outside its table" % len(frames))
            planes = tuple(indices.translate(bytes(palette[c::3]).ljust(256, b"\0"))
                           for c in range(3))
            frames.append((delay, planes))
            delay = 0
        else:
            raise GifError("unknown block 0x%02x at byte %d" % (block, pos))
    if pos != len(data) - 1:
        raise GifError("%d bytes after the trailer" % (len(data) - 1 - pos))
    return width, height, frames


# ---- the pictures the recorder should have drawn, from the frames dump --------
class Renderer:
    """record.c's canvas, redrawn from the settings in the dump's meta line."""

    def __init__(self, render):
        self.scale, self.tick_cs = render["scale"], render["tick_cs"]
        self.band = render["band"]
        gamma = render["gamma"]
        self.lut = [int(math.floor(255.0 * math.pow(v / 255.0, 1.0 / gamma) + 0.5))
                    for v in range(256)]
        s = self.scale
        if self.band:
            self.width, self.height = COLS * s + 2 * s, ROWS * s + 2 * s
        else:
            self.width, self.height = PANEL * s + 2 * s, 2 * ROWS * s + 3 * s

    def colour(self, hex6):
        led = (int(hex6[0:2], 16), int(hex6[2:4], 16), int(hex6[4:6], 16))
        if led == (0, 0, 0):
            return OFF_COLOUR
        return tuple(self.lut[v] for v in led)

    def planes(self, rows):
        """rows: 7 strings of 120 x RRGGBB. Returns (r, g, b) planes of the canvas."""
        s = self.scale
        grid = [[self.colour(r[c * 6:c * 6 + 6]) for c in range(COLS)] for r in rows]
        colours = {c for row in grid for c in row} | {BACKGROUND}
        # More than 256 colours: the recorder drops low bits until they fit.
        drop = 0
        while len({tuple(v & (0xff << drop) & 0xff for v in c) for c in colours}) > 256:
            drop += 1
        mask = (0xff << drop) & 0xff
        blank = bytes(self.width)
        out = []
        for plane in range(3):
            lines = []
            bg = bytes([BACKGROUND[plane] & mask])

            def led_line(row, first, count):
                cells = b"".join(bytes([grid[row][c][plane] & mask]) * (s - 1) + bg
                                 for c in range(first, first + count))
                return bg * s + cells + bg * s

            lines += [blank] * s
            if self.band:
                for row in range(ROWS):
                    lines += [led_line(row, 0, COLS)] * (s - 1) + [blank]
            else:
                for side in range(2):
                    for row in range(ROWS):
                        lines += [led_line(row, side * PANEL, PANEL)] * (s - 1) + [blank]
                    lines += [blank] * s
            if self.band:
                lines += [blank] * s
            out.append(b"".join(lines))
        return tuple(out)


def expected_frames(dump_path):
    """Replays record.c's sampling: [(delay_cs, rows), ...] and the renderer."""
    meta, frames, end_cycle = None, [], None
    for line in open(dump_path, encoding="utf-8"):
        d = json.loads(line)
        if d["type"] == "meta":
            meta = d
        elif d["type"] == "frame":
            frames.append((d["cycle"], d["rows"]))
        elif d["type"] == "end":
            end_cycle = d["cycle"]
    if meta is None or "render" not in meta or end_cycle is None:
        raise GifError("the dump has no meta line with render settings, or no end line")
    renderer = Renderer(meta["render"])
    tick_cs = renderer.tick_cs
    dark = ["000000" * COLS] * ROWS
    shown, shown_tick, out = dark, 0, []

    def emit(until_tick):
        out.append((min(max(until_tick - shown_tick, 1) * tick_cs, 65535), shown))

    for cycle, rows in frames:
        tick = int(cycle / F_CPU * 100.0 / tick_cs)
        if tick > shown_tick and rows != shown:
            emit(tick)
            shown_tick = tick
        shown = rows
    emit(int(end_cycle / F_CPU * 100.0 / tick_cs + 0.999))
    return renderer, out


def compare(gif_path, dump_path):
    """Returns (frame count, total seconds). Raises GifError on any difference."""
    width, height, frames = read_gif(gif_path)
    renderer, expected = expected_frames(dump_path)
    if (width, height) != (renderer.width, renderer.height):
        raise GifError("the GIF is %d x %d, the dump's settings give %d x %d"
                       % (width, height, renderer.width, renderer.height))
    if len(frames) != len(expected):
        raise GifError("the GIF has %d frames, the dump gives %d" % (len(frames), len(expected)))
    cache = {}
    for i, ((delay, planes), (want_delay, rows)) in enumerate(zip(frames, expected)):
        if delay != want_delay:
            raise GifError("frame %d is shown for %d/100 s, the dump gives %d/100 s"
                           % (i, delay, want_delay))
        key = tuple(rows)
        if key not in cache:
            cache.clear()
            cache[key] = renderer.planes(rows)
        if planes != cache[key]:
            want = cache[key]
            for p in range(width * height):
                if any(planes[c][p] != want[c][p] for c in range(3)):
                    raise GifError("frame %d differs at x=%d y=%d: GIF %s, dump %s"
                                   % (i, p % width, p // width,
                                      tuple(planes[c][p] for c in range(3)),
                                      tuple(want[c][p] for c in range(3))))
    return len(frames), sum(d for d, _ in frames) / 100.0


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        n, seconds = compare(sys.argv[1], sys.argv[2])
    except GifError as e:
        print("FAIL  %s: %s" % (sys.argv[1], e))
        sys.exit(1)
    print("ok    %s: %d frames, %.2f s, every pixel and delay as the frames dump has them"
          % (sys.argv[1], n, seconds))


if __name__ == "__main__":
    main()
