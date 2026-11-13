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
make -C emulator test     # smoke test, no human needed
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

- `--record out.gif`: animated GIF. Frame timing comes from the simulated
  clock, so the clip plays for exactly the simulated time it covers. A GIF
  cannot time frames finer than 1/100 s and viewers slow down anything under
  2/100 s, so the picture is sampled every 2/100 s (`--tick-cs`); the sign's
  fastest animations latch a frame every 3 to 6 ms, and the GIF shows the last
  one in each tick.
- `--png-dir dir`: one PNG per change on the same tick, plus `index.tsv` with
  each file's start time and how long it was shown.
- `--frames out.jsonl`: every latched frame, not sampled, for tests.
- `--scale n`: screen pixels per LED (default 6). `--band`: the 120 x 7 layout.

Without the app, `--duration <seconds>` stops a run after that much simulated
time; the startup animation takes the first 5.5 s.

### The frames dump

One JSON object per line.

| `type` | fields |
| --- | --- |
| `meta` | first line: firmware path, geometry, options used |
| `frame` | `t` seconds and `cycle` (CPU cycles) when the LEDs latched; `rows`: 7 strings, `rows[0]` the top row, each 120 x `RRGGBB` raw LED values, column 0 first; `bits`: bits clocked into a strip for this frame (2880 = all 120 columns; fewer means the columns beyond kept their old colour, more means the extra fell off the end of the strip); `gap_us`: longest pause inside the frame; `t0h_cycles`, `t1h_cycles`: shortest and longest high time of 0 and 1 bits; `marginal`, `partial_bits`: timing trouble, both 0 for a healthy frame |
| `tx` | a byte the sign sent: `t` of its start bit, `byte`, `char` |
| `rx` | a byte sent to the sign, same fields |
| `beat` | a rising edge on the beat input |

`tests/check_frames.py` is a worked example of reading it.

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
  `{"type":"F","str":"HELLO"}`; to `?` it may answer `{}`

To connect another program, open a TCP connection to that port and do the
same. One client at a time; a new connection replaces the old one. A quick
look by hand: `nc 127.0.0.1 14400`, wait for `~`, type a JSON object.

Timing of the answer: when a request byte goes out, the emulator holds the
simulated clock for up to one real second (`--link-wait-ms`) while the client
answers, then puts the answer's first bit on the wire 5 ms of simulated time
after the request (`--link-latency-ms`). So how fast the client is on this Mac
does not change what the firmware sees, and a run repeats exactly. A client
that takes longer than that real second is late in simulated time too, and
bytes sent without a request go on the wire when they arrive. Bytes that
arrive while the firmware is not listening are lost, as on the real sign.

There is no stdin/stdout mode for the link; use the TCP port.

## Playlists

A playlist is a text file with one JSON object per line. Blank lines and lines
starting with `#` are skipped. `fake_app.py` sends them in order and loops.
Keys starting with `_` are for the fake app and are not sent: `"_polls": n` on
a chooser message answers the next n `?` polls with `{}` before moving on
(default 40, `--chooser-polls`).

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
  longer than the latch time shows the frame. The **latch time is a
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
  the fixed answer delay described above. The real app's delay is not known
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
| `src/` | the emulator: `main.c`, `ws2812.c` (LED decoder), `softuart.c` (serial pins), `link.c` (TCP), `view.c` (terminal), `record.c` (GIF, PNG, frames dump) |
| `fake_app.py` | the app's side of the link |
| `playlists/` | `all_types.jsonl`, `demo.jsonl` |
| `test.sh`, `tests/check_frames.py` | the smoke test |
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
  emulator reads the Intel HEX file itself.
- `make ARDUINO_SDK=/path/to/Arduino.app/Contents/Java` if the IDE lives
  elsewhere.

## Speed

Unpaced, about 4 to 4.6 times real time on this MacBook, so the live view has
room to run at real time (`--speed 1`, the live default; `--speed 0` runs flat
out, which is the headless default). If the Mac cannot keep up, the view says
so in yellow under the title.
