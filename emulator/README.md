# Sign emulator

Runs the sign's firmware on this computer and shows what the LEDs would show,
so a firmware change can be watched and checked without the hardware.

It runs the **real firmware image**: the same `LED_Marquee.hex` that gets
flashed, executed instruction by instruction on a simulated ATmega328P
([simavr](https://github.com/buserror/simavr)). Nothing in the emulator knows
what the sketch means to draw. It watches the pins: it decodes the WS2812 bit
stream the firmware bit-bangs on PORTD into pictures, and it is the other end
of the firmware's software serial port.

![the sign's startup, a chooser message, a chonky message and a marquee](docs/demo.gif)

## Quick start

Needs the Arduino IDE 1.8.x in `/Applications/Arduino.app` (its avr-gcc 7.3.0
builds the firmware), plus `cmake`, `make`, the Xcode command-line compiler and
`python3`. Nothing is installed: everything lands in `emulator/build/` and
`emulator/third_party/`, both gitignored.

```sh
make -C emulator          # firmware image + emulator (first run clones simavr v1.8)
make -C emulator test     # all tests, about three minutes, no human needed
```

Watch it live, in two terminals:

```sh
emulator/build/sign-emu emulator/build/firmware/LED_Marquee.hex     # the sign
emulator/fake_app.py emulator/playlists/all_types.jsonl             # the app
```

The first terminal shows both sides of the sign, the simulated time and the
speed. The second answers the sign's requests from a playlist. Without the
second, the sign behaves as it does with the app unplugged: it asks, waits a
second, and shows stars.

Type messages yourself:

```sh
emulator/fake_app.py -i            # plain text scrolls; ":F HELLO" sends type F; "{...}" is sent as is
```

## What you see

Each of the 7 LED rows is one 120-pixel strip that goes around the sign like a
belt. Columns 0-59 are side A (nearest the Arduino), columns 60-119 are side B,
mounted back to back with A. Both sides read left to right in strip order;
nothing is mirrored.

- Default view: two labelled 60 x 7 grids, side A above side B.
- `--band`: one 120 x 7 band, side A on the left. This is how a marquee
  travels: it enters at the right edge of side B and leaves at the left edge of
  side A.

Row and column order were worked out from the font data, not assumed:
`Fonts.h` says bit 7 of a column byte is the top row, so PD7 drives the top row
and PD1 the bottom, and the first pixel sent is column 0. The smoke test
proves it: the startup words and a marquee read correctly against `Fonts.h`.

### Brightness on a screen

The firmware gamma-corrects for LEDs (`Gamma.h`), so most of the values it
sends are small. An LED's light output is proportional to the value it is
given; a screen's is proportional to (code/255)^2.2. To show the same relative
brightness, an LED value `v` is drawn as `255 * (v/255)^(1/2.2)`. So LED 255 is
screen 255, LED 64 is screen 136, LED 8 is screen 53. `--gamma 1` shows raw
values; `--gamma <g>` uses another exponent. An LED that is off is drawn as a
faint dot so the panel outline stays visible. The frames dump (`--frames`)
always holds the raw LED values.

## Recording, for a person or an agent

All of this runs headless (no terminal needed; headless is the default when
stdout is not a terminal).

```sh
# terminal 1: boot, wait for the app, stop when the sign asks for its 4th message
emulator/build/sign-emu --headless --wait-client 20 --until-requests 4 \
    --record out.gif --frames out.jsonl emulator/build/firmware/LED_Marquee.hex
# terminal 2
emulator/fake_app.py emulator/playlists/demo.jsonl
```

- `--record out.gif`: animated GIF, timed by the simulated clock.
- `--png-dir dir`: one PNG per change of picture, plus `index.tsv` with each
  file's start time and how long it was shown. A picture is listed only once
  its file has been written and closed whole.
- `--frames out.jsonl`: every latched frame, not sampled, for tests.
- `--scale n`: screen pixels per LED (2 to 64, default 6). `--band`: the
  120 x 7 layout.

Without the app, `--duration <seconds>` stops a run after that much simulated
time; the startup animation takes the first 5.5 s.

**When each picture is shown.** A GIF cannot time frames finer than 1/100 s,
and viewers slow down anything under 2/100 s. So the clip is cut into ticks of
2/100 s (`--tick-cs`, 1 to 6000), each exactly 320000 CPU cycles:

- During tick k the clip shows the last frame latched in tick k or earlier,
  and dark before the first. The sign's fastest animations latch a frame every
  3 to 6 ms; the clip shows the last one of each tick.
- A picture that does not change is written once, with the whole time it
  stays. "The picture" is what is drawn: two frames whose LED values differ
  but are drawn in the same screen colours (254 and 255 are, at the default
  mapping) are one picture. One GIF frame can hold at most 655.35 s, so a longer stay is written
  as several frames of the same picture whose times add up to it.
- The clip lasts the run's simulated time rounded up to a whole tick, and at
  least to the end of the tick in which the last frame latched. It is never
  shorter than the run and at most one tick longer, however long the run is.

**What a picture looks like.** A margin of `scale` pixels all round. Each LED
is a square of `scale - 1` pixels with one background pixel to its right and
below. In the default layout side A is above side B with a gap of `scale`
pixels. Colours are as in "Brightness on a screen"; a GIF frame holds 256
colours, so a picture with more loses low bits, as few as it takes (PNGs keep
all of them).

**If a recording cannot be written** (the path cannot be created, the disk is
full, a file size limit is reached), the run stops, the last lines on stderr
say `RECORDING FAILED` with the path and the reason, and the exit status is 4.
A status of 0 means every output was written and closed whole.

Exit status: 0 the run ended as asked; 1 it could not start (firmware file,
port); 2 bad command line (numbers outside their ranges are refused); 3 the
firmware crashed or stopped; 4 a recording could not be written.

### The frames dump

One JSON object per line.

| `type` | fields |
| --- | --- |
| `meta` | first line: firmware path, geometry, options used; `render` holds what is needed to redraw the GIF and PNG pictures from the frames (scale, tick, layout, gamma) |
| `frame` | `t` seconds and `cycle` (CPU cycles) when the LEDs latched; `rows`: 7 strings, `rows[0]` the top row, each 120 x `RRGGBB` raw LED values, column 0 first; `bits`: bits clocked into a strip for this frame (2880 = all 120 columns; fewer means the columns beyond kept their old colour, more means the extra fell off the end of the strip); `gap_us`: longest pause inside the frame; `t0h_cycles`, `t1h_cycles`: shortest and longest high time of 0 and 1 bits; `marginal`, `partial_bits`: timing trouble, both 0 for a healthy frame |
| `tx` | a byte the sign sent: `t` of its start bit, `byte`, `char` |
| `rx` | a byte sent to the sign, as a listener on the sign's receive pin decoded it: `t` and `cycle` are when its start bit was seen to fall, `due` is the cycle the link's rules put it at (the pin shows it at the first instruction boundary, 0 to 4 cycles later), `byte`, `char` |
| `beat` | a rising edge on the beat input |
| `end` | last line: `t` and `cycle` when the run stopped |

A reader should skip types it does not know. `tests/check_frames.py` is a
worked example of reading it, and `tests/gif_check.py` redraws the GIF from it.

## The serial link

The firmware talks to the app with SoftwareSerial on Arduino pins 11 (TX) and
10 (RX) at 14400 baud, 8N1, and only listens around each request. The emulator
decodes the bytes the firmware bit-bangs on pin 11 and drives pin 10 with
correctly timed bits in simulated time, so the firmware's own bit-banged
receiver (its pin-change interrupt and delay loops) does the reading.

The link is exposed as a TCP port on localhost, default `127.0.0.1:14400`
(`--port n`, `--port 0` for none). It carries exactly the bytes the UART
carries, nothing added:

- the sign sends one request byte: `~` ("send the next message") or `?`
  (chooser poll)
- the peer answers with one JSON object and no newline, for example
  `{"type":"F","str":"HELLO"}`; to `?` it may answer with a checksum report,
  `{"c":13345}` (chooser contract version 2; version 1 used `{}`)

To connect another program, open a TCP connection to that port and do the
same. One client at a time; a new connection replaces the old one. A quick
look by hand: `nc 127.0.0.1 14400`, wait for `~`, type a JSON object.

There is no stdin/stdout mode for the link; use the TCP port.

### When a reply reaches the sign

The firmware runs in simulated time and the peer runs in real time, so the
emulator has to decide at which simulated moment each reply goes on the wire.
The rule is built so that the answer does not depend on this computer: not on
how long the peer took, and not on how TCP cut its bytes into pieces.

1. **A reply is one JSON object.** The emulator follows the brackets and
   strings of the stream to see where each object closes. Whitespace after an
   object (a newline, say) travels in front of the next reply. Each request
   starts a new reply: an object left open by an earlier, unfinished reply
   does not count against it.
2. **The clock is held while a request is answered.** When the sign's request
   byte (`~` or `?`, see `--request-chars`) has gone out, simulated time stops
   until the whole reply has arrived, in however many pieces. It stops for at
   most `--link-wait-ms` of real time (default 1000).
3. **The reply is placed relative to its request.** Its start bit begins
   `--link-latency-ms` (default 5) of simulated time after the end of the
   request's stop bit. That number stands in for the app's reaction time, and
   it is the same for every reply. In cycles: the request ends 10 bit times
   (11111 1/9 cycles) after its start bit began, the reply cannot start before
   that, so it is due 11112 cycles plus the latency after the request's start
   bit. A millisecond is 16000 cycles; fractions go to the nearest cycle.
4. **Replies leave in the order they came, one at a time,** as from one UART.
   A reply's bytes go out back to back at 14400 baud, every bit edge at the
   whole cycle nearest its exact time, so nothing drifts. A reply whose moment
   comes while an earlier one is still going out starts when that one's last
   stop bit ends. A reply that is due later waits for its own moment; it does
   not ride out behind an earlier one.
5. **Every byte is put on the wire at its moment, whether or not the firmware
   is listening.** The emulator never drops, repairs or re-times them. What
   becomes of them is the firmware's own code running. Two cases the link
   tests show with the current firmware: a late reply that lands inside a
   later request's window is read as that request's answer; and a late reply
   that lands after a read timed out with nothing received, while stars are
   showing, is half received (`readSerialData()` returns without
   `stopSerial()` there, and interrupts are off while pixels go out), stays in
   the receive buffer, and makes the next read fail with `InvalidInput`.

So a late reply is modelled by its delay alone. `--link-latency-ms 80` makes
every reply 80 ms late, which is past a 60 ms poll timeout and inside the next
request's window.

For a different delay per reply, start the emulator with `--link-directives`.
The peer may then put a line in front of a reply, which the emulator consumes
and does not put on the wire:

| the peer sends | effect |
| --- | --- |
| `@250` newline, then the reply | this reply's start bit begins 250 ms (simulated, fractions allowed) after its request, instead of `--link-latency-ms` |
| `@none` newline | this request gets no reply, and the clock is not held waiting for one |

A reply can be delayed past the firmware's timeout and into a later request's
window; rule 4 then decides the order on the wire. Without
`--link-directives` an `@` is an ordinary byte and goes to the sign.

Only those two forms are directives: `@`, then `none` or a plain number of
milliseconds, then a newline, where a reply would start. Anything else that
starts with `@` (a missing newline, a stray letter) is passed to the sign as
the bytes it is, from the first byte that shows it is not a directive. A
mistyped directive is therefore visible on the wire (the sign answers
`InvalidInput`) and never swallows the replies after it.

**When a client goes away.** A client's connection ends when it closes, when
a read or write on it fails, or when another client connects (the new one
replaces it). At that moment:

- Everything it left unfinished and still held back is dropped: the part of
  a reply that had arrived before the object closed, a directive without its
  reply, whitespace waiting for the next reply. None of that reaches the wire
  or the next client. (Bytes that had already gone to the wire under the
  "not repeatable" cases below are on the wire.)
- If the clock was held for its answer, the clock is released at once; the
  request simply gets no reply. The next client never saw that request.
- A reply that had arrived whole is still sent at its moment. It was handed
  over; a client that answers and hangs up does not lose its last reply.

So the next client is served exactly as a first client would be. In the link
tests, a run in which the first client leaves a half-typed directive, half a
JSON object, a directive without a reply, or nothing at all, and a second
client takes over, gives the same frames dump as a run with only the second
client. Two things about a changeover cannot be made repeatable: the simulated
moment a client connects when nothing is being held, and a request that goes
out while no client is connected (nobody answers it).

What is **not** repeatable, and how to tell:

- A reply that is not complete within `--link-wait-ms` of real time. What has
  arrived of it is placed as in rule 3, and the rest goes on the wire at
  whatever simulated moment it arrives. The reply is then given up: the next
  request's reply is looked for afresh, and a directive that was waiting for
  the late reply is dropped. The run's summary says
  `NOTE: n request(s) had no complete reply ...`.
- A reply longer than 8192 bytes. It is passed to the wire in pieces as it
  arrives, all placed as in rule 3, so it is repeatable unless its client goes
  away part way through (the pieces already passed on are sent).
- Bytes the peer sends when no request is waiting (a second object for the
  same request, for example). They go on the wire at the simulated moment they
  arrive, plus their directive's delay if they have one.
- `--link-wait-ms 0`: the clock is never held, and everything is placed by
  arrival.

`tests/link_test.py` checks rules 1 to 4 and the directives against the real
firmware, and is a worked example of a peer.

## Playlists

A playlist is a text file with one JSON object per line. Blank lines and lines
starting with `#` are skipped. `fake_app.py` sends them in order and loops.
Keys starting with `_` are for the fake app and are not sent: `"_polls": n` on
a chooser message says how long it stays selected before the playlist moves on
(default 40, `--chooser-polls`).

The fake app speaks both versions of the chooser contract:

- `--contract 1` (default): the next n `?` polls are answered with `{}`.
- `--contract 2`: the next n requests are answered about the selected
  message: `?` with `{"c":N}`, where N is the CRC-16/CCITT-FALSE of its `str`,
  a zero byte and its `flashy`; `~` with the message itself again. When the
  playlist moves on to another chooser message, `?` reports the new checksum;
  when it moves on to anything else, `?` is answered like `~`. Use it with
  firmware that polls with `?`: firmware that only sends `~` gets the chooser
  message n times.

To add one, copy `playlists/demo.jsonl`, edit, and pass the new file to
`fake_app.py`. `playlists/all_types.jsonl` has one of every message type the
firmware's `loop()` understands, with a comment on each:

| type | what the sign does | fields it reads |
| --- | --- | --- |
| `O` | characters one by one, centred | `str`, `r`, `g`, `b`, `dly` |
| `C` | chonky font with colour cycle | `str`, `dly`, `r`, `g`, `b`, `scroll` |
| `H` | chooser: red count, blinking text | `str`, `flashy` |
| `U` | utility: `subtype` `B` brightness shift, `M` beat input on/off | `str`, `subtype` |
| `K` | keyboard echo | `str`, `mode` |
| `F` | flashy fade, then stars | `str` |
| `W` | countdown, then stars | `str` |
| `S` | starfield | `stars` |
| `I` | icon invaders (`str` 1, 2, X, A, B, D) | `str`, `r`, `g`, `b` |
| `T` | track id marquee | `str` |
| `N` | chonky marquee, then stars | `str` |
| `D` | default marquee, then stars | `str` |

## Beat input

The firmware flashes the marquee white on a rising edge of A0 (pin-change
interrupt). `--bpm 128` pulses that pin 128 times a minute (high for 30 ms each
time).

## How exact it is

**Executed, not modelled.** The CPU runs the flashed image itself. Every
instruction costs the cycles the ATmega328P datasheet gives it, at 16 MHz.
Timer 0 (`millis`, `delay`), the pin-change interrupts and the I/O ports are
simavr's models of the chip's peripherals. So the firmware's own code paths
decide everything you see: its bit-banged WS2812 timing (measured in every
run: 0 bits high for 5 cycles, 1 bits for 13, as `PixelBitBanging.h` intends),
its integer widths and overflows, its interrupt masking while pixels go out,
its `random()` sequence (never seeded, so the same on the sign), its
SoftwareSerial bit timing and 64-byte receive buffer, ArduinoJson's parsing
and the one-second read timeout.

`make firmware` builds the unmodified sketch with the repo's own toolchain
file, compiler and flags, and prints the image's size and SHA-256. At the
commit this emulator was written against, that HEX is byte-identical to the
one in Afik's last CLion build directory.

**Modelled.** These are stand-ins for things outside the chip:

- The LEDs. A WS2812 model: a high pulse of 9 cycles or more is a 1; pixels
  fill in order, G R B, most significant bit first; bits past pixel 120 fall
  off the end; pixels not addressed in a frame keep their colour; a low gap
  longer than the latch time shows the frame. A frame is the whole picture:
  all strips that have gone quiet for the latch time are taken into it
  together, stamped with the moment the last of them latched. The firmware
  drives all seven strips from one port write, so they do go quiet together
  (within 8 cycles); firmware that drove strips at different times would see
  them grouped this way rather than latched one by one. The **latch time is a
  parameter** (`--reset-us`, default 50, the original WS2812B datasheet value).
  Newer WS2812B strips need about 280. Each run prints the longest pause
  inside a frame and the shortest pause between frames: the pictures are the
  same for any latch time between those two. For the current firmware that is
  25 to 101 us, because the starfield relies on `random()` being slow (about
  100 us) to latch each star. With `--reset-us 280` a whole starfield burst
  latches once, at its end, as a single star, instead of twinkling. Which one
  the real strips do has not been checked. LEDs change colour only at the
  latch, never part way through a frame.
- The serial peer: an ideal UART at exactly 14400 baud on an ideal wire, with
  the fixed answer delay described above. Each edge is due at the whole cycle
  nearest its exact time and appears on the pin at the first instruction
  boundary from there (0 to 4 cycles later), which is the first moment the
  firmware could see it. The real app's delay is not known
  and varies.
- The app: `fake_app.py` plays a list. The real app's message queue, chooser
  logic and Bluetooth commands are not here.
- The beat input: clean pulses at a fixed rate, not the real beat detector.
- Screen colours: see "Brightness on a screen". Real LED colour, glare and
  viewing distance are not reproduced.

**Missing.**

- Anything electrical: supply sag at high brightness, signal quality on long
  strips, level shifting, dead or miswired LEDs.
- Clock error: exactly 16 MHz here; the Uno's resonator is within about 0.5%.
- Power-on details: the bootloader's wait before the sketch starts, reset and
  brown-out behaviour. RAM starts as zeros.
- The diagnostic LED on pin 13 and the pulse on pin 8 are not shown.
- It has not been compared with the physical sign. Nothing here touches
  hardware.

### What the emulator shows about the current firmware

Seen while building this; the firmware is not changed by this directory.

- Red and blue are sent as `v | (v >> 1)`, green as `v`. `sendBitx8` advances
  its bit walker with `ror`, which rotates the carry flag in, and the carry is
  still set from the previous colour byte when the red and blue bytes start.
  Example: the startup fade's grey 0x5f goes out as R 0x7f, G 0x5f, B 0x7f, so
  greys lean magenta, and the chooser's red 0x80 goes out as 0xc0.
- A one-character string is drawn one column further right on side B than on
  side A (61 columns are sent per side).
- One star in eight is invisible (`y = r & 0x07` can be 7, and `2 << 7` does
  not fit in the row byte).
- At the end of a marquee the frames get shorter than the strip, so the
  columns beyond them keep what they last showed until a later frame reaches
  them.

## Files

| | |
| --- | --- |
| `Makefile` | `make`, `make firmware`, `make run`, `make test`, `make clean` |
| `firmware/` | CMake wrapper that builds the unmodified sketch (no upload target) |
| `fetch_simavr.sh` | clones simavr at the pinned commit into `third_party/` |
| `src/` | the emulator: `main.c`, `ihex.c` (firmware image reader), `ws2812.c` (LED decoder), `softuart.c` (serial pins and the queue of replies), `reply.c` (cuts the app's bytes into replies), `link.c` (TCP), `view.c` (terminal), `record.c` (GIF, PNG, frames dump) |
| `fake_app.py` | the app's side of the link |
| `playlists/` | `all_types.jsonl`, `demo.jsonl` |
| `test.sh` | runs every test below |
| `tests/check_frames.py` | the smoke run's frames against `Fonts.h`, and its GIF against its frames |
| `tests/gif_check.py` | what a recording must be, stated from the rules above in exact arithmetic: a strict GIF decoder, and the GIF and PNG pictures and their times against the frames dump |
| `tests/gif_cases.py`, `tests/gif_case.c` | chosen, random and very long-held pictures through the GIF encoder |
| `tests/link_test.py` | the serial link's timing model against the real firmware, with clients that come and go |
| `tests/record_fail_test.py` | recordings that cannot be created, written or closed |
| `tests/cli_test.py` | command line values that are refused |
| `tests/test_ihex.c` | the firmware image reader, built with the address and undefined-behaviour sanitizers |
| `tests/test_fake_app.py`, `tests/check_meta.py` | the fake app's checksum and answers; the dump's first line |
| `docs/demo.gif` | `demo.jsonl` recorded with `--bpm 128 --scale 4 --tick-cs 4` |

Build notes:

- `firmware/CMakeLists.txt` mirrors the three settings of the repo's
  `CMakeLists.txt` (sketch, board `uno`, toolchain file) but sets no serial
  port, so its build tree has no avrdude target. Keep it in step if the top
  one changes.
- `firmware/toolchain.cmake` loads the repo's `cmake/ArduinoToolchain.cmake`
  unchanged behind a one-function shim, because current CMake (3.25 and later)
  cannot load that file as it is.
- Homebrew has no simavr formula for this Mac, so the Makefile compiles
  simavr's sources directly (only the ATmega328 core), without libelf: the
  emulator reads the Intel HEX file itself. A file that is cut short (no
  end-of-file record), has a line that is not a record, or places data
  outside the 32 KiB flash is refused, not loaded as far as it goes.
- `make ARDUINO_SDK=/path/to/Arduino.app/Contents/Java` if the IDE lives
  elsewhere.

## Speed

Unpaced, about 4 to 4.6 times real time on this MacBook, so the live view has
room to run at real time (`--speed 1`, the live default; `--speed 0` runs flat
out, which is the headless default). If the Mac cannot keep up, the view says
so in yellow under the title.
