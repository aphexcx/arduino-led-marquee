#!/usr/bin/env python3
"""Decodes every frame of a GIF written by the emulator and checks it against
the frames dump of the same run: every pixel, and when each picture is shown.

This file is an independent statement of what the recording must be. It does
not replay the recorder's steps; it works from the rules in the README
("Recording"), in exact integer arithmetic:

  change   "The picture" is what is drawn. Frames whose LED values differ but
           map to the same screen colours are one picture, written once.
  time     The clip is cut into ticks of tick_cs/100 s, which is exactly
           160000 * tick_cs CPU cycles. During tick k the clip shows the last
           frame latched in tick k or earlier (dark before the first). The
           clip lasts ceil(end / tick) ticks, and at least to the end of the
           tick in which the last frame latched. So its length is within one
           tick of the simulated time the run covered, however long that is.
  picture  Margin of `scale` pixels all round; each LED is a square of
           scale - 1 pixels with one background pixel to its right and below;
           stacked layout: side A, a gap of `scale`, side B.
  colour   An LED that is off is (14, 14, 16). Otherwise each channel v is
           shown as round(255 * (v / 255) ** (1 / gamma)), computed here with
           exact fractions.

The LZW decoder is strict: it follows the GIF89a rules for when the code width
grows, and anything a conforming decoder could not accept is an error.

usage: gif_check.py <clip.gif> <frames.jsonl>
Python 3 standard library only.
"""
import json
import os
import struct
import sys
import zlib
from fractions import Fraction

COLS, PANEL, ROWS = 120, 60, 7
CYCLES_PER_CS = 160000           # 16 MHz: CPU cycles in 1/100 s
OFF_COLOUR = (14, 14, 16)
BACKGROUND = (0, 0, 0)
GIF_MAX_DELAY = 65535            # the delay field is 16 bits, in 1/100 s


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
    frames, delay = [], None

    def sub_blocks(p):
        chunks = []
        while data[p]:
            chunks.append(data[p + 1:p + 1 + data[p]])
            p += data[p] + 1
        return b"".join(chunks), p + 1

    try:
        while True:
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
                if delay is None:
                    raise GifError("frame %d has no delay" % len(frames))
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
                delay = None
            else:
                raise GifError("unknown block 0x%02x at byte %d" % (block, pos))
    except IndexError:
        raise GifError("the file ends in the middle of a block (no trailer)") from None
    if pos != len(data) - 1:
        raise GifError("%d bytes after the trailer" % (len(data) - 1 - pos))
    if not frames:
        raise GifError("no frames")
    return width, height, frames


# ---- what the pictures must be, from the README's rules -------------------------
def screen_table(gamma):
    """table[v] = round(255 * (v / 255) ** (1 / gamma)) for v in 0..255, exactly.

    s is the answer when ((s - 1/2) / 255) ** gamma <= v / 255 < ((s + 1/2) / 255) ** gamma.
    With gamma = p / q that is a comparison of whole numbers:
    (2s - 1) ** p * 255 ** q <= v ** q * 510 ** p.
    """
    g = Fraction(gamma).limit_denominator(1000)
    if g <= 0:
        raise GifError("gamma %r" % gamma)
    p, q = g.numerator, g.denominator
    table, s = [], 0
    for v in range(256):
        while s < 255 and (2 * s + 1) ** p * 255 ** q <= v ** q * 510 ** p:
            s += 1
        table.append(s)
    return table


class Picture:
    """The canvas the README describes, drawn a scan line at a time."""

    def __init__(self, render):
        self.scale, self.band = render["scale"], render["band"]
        self.table = screen_table(render["gamma"])
        s = self.scale
        self.sides = [(0, COLS)] if self.band else [(0, PANEL), (PANEL, PANEL)]
        columns = self.sides[0][1]
        self.width = s + columns * s + s
        self.height = s + len(self.sides) * (ROWS * s + s)
        self.cache = {}

    def colour(self, hex6):
        led = (int(hex6[0:2], 16), int(hex6[2:4], 16), int(hex6[4:6], 16))
        return OFF_COLOUR if led == (0, 0, 0) else tuple(self.table[v] for v in led)

    def drawn(self, rows):
        """The screen colour of every LED: what tells one picture from another."""
        return tuple(tuple(self.colour(r[c * 6:c * 6 + 6]) for c in range(COLS)) for r in rows)

    def planes(self, rows, all_colours=False):
        """rows: 7 strings of 120 x RRGGBB. Returns the (r, g, b) planes of the canvas.

        all_colours: for a format with no limit on colours (PNG), nothing is dropped.
        """
        key = (tuple(rows), all_colours)
        if key in self.cache:
            return self.cache[key]
        s = self.scale
        leds = [[self.colour(r[c * 6:c * 6 + 6]) for c in range(COLS)] for r in rows]
        # More than 256 colours cannot go in one GIF frame: low bits are dropped,
        # as few as it takes.
        colours = {c for row in leds for c in row} | {BACKGROUND}
        drop = 0
        while not all_colours and len({tuple(v >> drop for v in c) for c in colours}) > 256:
            drop += 1
        result = []
        for channel in range(3):
            def shade(colour):
                return bytes([colour[channel] >> drop << drop])
            background = shade(BACKGROUND)
            empty_line = background * self.width
            lines = [empty_line] * s
            for first, count in self.sides:
                for row in range(ROWS):
                    led_line = background * s + b"".join(
                        shade(leds[row][c]) * (s - 1) + background
                        for c in range(first, first + count)) + background * s
                    lines += [led_line] * (s - 1) + [empty_line]
                lines += [empty_line] * s
            result.append(b"".join(lines))
        self.cache = {key: tuple(result)}
        return self.cache[key]


def read_dump(dump_path):
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
    return meta["render"], frames, end_cycle


def timeline_from_dump(render, frames, end_cycle, picture):
    """[(first tick, rows), ...] for each change of picture, and the number of ticks.

    "Picture" is what is drawn: two frames whose LED values differ but map to
    the same screen colours are the same picture.
    """
    cycles_per_tick = CYCLES_PER_CS * render["tick_cs"]
    dark = ["000000" * COLS] * ROWS
    by_tick, previous = {}, 0                      # tick -> the last frame latched in it
    for cycle, rows in frames:
        if cycle < previous:
            raise GifError("the frames dump goes back in time at cycle %d" % cycle)
        previous = cycle
        by_tick[cycle // cycles_per_tick] = rows
    ticks = max(-(-end_cycle // cycles_per_tick), max(by_tick, default=0) + 1, 1)
    first = by_tick.get(0, dark)
    timeline, shown = [(0, first)], picture.drawn(first)
    for tick in sorted(by_tick):
        if tick and picture.drawn(by_tick[tick]) != shown:
            shown = picture.drawn(by_tick[tick])
            timeline.append((tick, by_tick[tick]))
    return timeline, ticks


def first_difference(planes, want, width):
    for p in range(len(want[0])):
        if any(planes[c][p] != want[c][p] for c in range(3)):
            return "x=%d y=%d: file %s, expected %s" % (
                p % width, p // width, tuple(planes[c][p] for c in range(3)),
                tuple(want[c][p] for c in range(3)))
    return "sizes differ"


def compare(gif_path, dump_path):
    """Returns (frame count, total seconds). Raises GifError on any difference."""
    width, height, gif = read_gif(gif_path)
    render, frames, end_cycle = read_dump(dump_path)
    tick_cs = render["tick_cs"]
    picture = Picture(render)
    if (width, height) != (picture.width, picture.height):
        raise GifError("the GIF is %d x %d; scale %d gives %d x %d"
                       % (width, height, picture.scale, picture.width, picture.height))

    # How long the clip is, against the run's closing cycle alone.
    total_cs = sum(delay for delay, _ in gif)
    end_cs = Fraction(end_cycle, CYCLES_PER_CS)
    if not end_cs <= total_cs <= end_cs + tick_cs:
        raise GifError("the GIF plays for %d/100 s but the run covered %.2f/100 s: the clip "
                       "must be at least that long and at most one tick (%d/100 s) longer"
                       % (total_cs, float(end_cs), tick_cs))

    # Each picture of the timeline, in order: its frames in the GIF must show
    # it, add up to the time it stays, and be as few as the delay field allows
    # (one, unless it stays longer than 655.35 s).
    timeline, ticks = timeline_from_dump(render, frames, end_cycle, picture)
    ends = [tick for tick, _ in timeline[1:]] + [ticks]
    i = 0
    for (tick, rows), end in zip(timeline, ends):
        want, stay = picture.planes(rows), (end - tick) * tick_cs
        shown_for, count = 0, 0
        while shown_for < stay:
            if i == len(gif):
                raise GifError("the GIF ends at %d/100 s; the picture from %d/100 s stays "
                               "until %d/100 s" % (tick * tick_cs + shown_for, tick * tick_cs,
                                                   end * tick_cs))
            delay, planes = gif[i]
            if delay < 1:
                raise GifError("frame %d has a delay of 0" % i)
            if planes != want:
                raise GifError("frame %d, at %d/100 s, does not show the picture the frames "
                               "dump has for that moment (%s)"
                               % (i, tick * tick_cs + shown_for,
                                  first_difference(planes, want, width)))
            shown_for += delay
            count += 1
            i += 1
        if shown_for != stay:
            raise GifError("the picture from %d/100 s is shown for %d/100 s; the frames dump "
                           "gives %d/100 s" % (tick * tick_cs, shown_for, stay))
        if count != -(-stay // GIF_MAX_DELAY):
            raise GifError("the picture from %d/100 s stays %d/100 s and is written as %d "
                           "frames; that fits %d" % (tick * tick_cs, stay, count,
                                                     -(-stay // GIF_MAX_DELAY)))
    if i != len(gif):
        raise GifError("the GIF has %d frames after the run's last picture" % (len(gif) - i))
    return len(gif), total_cs / 100.0


def read_png(path):
    """Returns (width, height, (r, g, b) planes) of an 8-bit RGB PNG with unfiltered lines."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise GifError("%s is not a PNG" % path)
    pos, header, packed, ended = 8, None, b"", False
    while pos + 12 <= len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if len(body) != n or len(data) < pos + 12 + n or \
                struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])[0] != zlib.crc32(kind + body):
            raise GifError("%s: damaged %s chunk" % (path, kind.decode("latin-1")))
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            packed += body
        elif kind == b"IEND":
            ended = True
        pos += 12 + n
    if header is None or not ended or pos != len(data):
        raise GifError("%s is not a whole PNG" % path)
    width, height = header[0], header[1]
    if header[2:] != (8, 2, 0, 0, 0):
        raise GifError("%s is not 8-bit RGB" % path)
    raw = zlib.decompress(packed)
    stride = width * 3 + 1
    if len(raw) != stride * height or any(raw[y * stride] for y in range(height)):
        raise GifError("%s: picture data of the wrong size or with filtered lines" % path)
    pixels = b"".join(raw[y * stride + 1:(y + 1) * stride] for y in range(height))
    return width, height, tuple(pixels[c::3] for c in range(3))


def compare_pngs(directory, dump_path):
    """index.tsv and its pictures against the frames dump. Returns the number of pictures."""
    render, frames, end_cycle = read_dump(dump_path)
    tick_cs = render["tick_cs"]
    picture = Picture(render)
    timeline, ticks = timeline_from_dump(render, frames, end_cycle, picture)
    lines = open(os.path.join(directory, "index.tsv"), encoding="utf-8").read().splitlines()
    if not lines or lines[0].split("\t") != ["file", "start_s", "shown_for_s"]:
        raise GifError("index.tsv has no header line")
    entries = [line.split("\t") for line in lines[1:]]
    if len(entries) != len(timeline):
        raise GifError("index.tsv lists %d pictures, the frames dump gives %d"
                       % (len(entries), len(timeline)))
    ends = [tick for tick, _ in timeline[1:]] + [ticks]
    for (name, start_s, shown_s), (tick, rows), end in zip(entries, timeline, ends):
        if Fraction(start_s) * 100 != tick * tick_cs or \
                Fraction(shown_s) * 100 != (end - tick) * tick_cs:
            raise GifError("%s is listed from %s s for %s s; the frames dump gives %d/100 s "
                           "for %d/100 s" % (name, start_s, shown_s, tick * tick_cs,
                                             (end - tick) * tick_cs))
        width, height, planes = read_png(os.path.join(directory, name))
        if (width, height) != (picture.width, picture.height):
            raise GifError("%s is %d x %d, expected %d x %d"
                           % (name, width, height, picture.width, picture.height))
        want = picture.planes(rows, all_colours=True)
        if planes != want:
            raise GifError("%s does not show the frame the dump has for its moment (%s)"
                           % (name, first_difference(planes, want, width)))
    return len(entries)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        n, seconds = compare(sys.argv[1], sys.argv[2])
    except GifError as e:
        print("FAIL  %s: %s" % (sys.argv[1], e))
        sys.exit(1)
    print("ok    %s: %d frames, %.2f s: every pixel as the frames dump has it, every picture "
          "at its moment, and the clip as long as the run" % (sys.argv[1], n, seconds))


if __name__ == "__main__":
    main()
