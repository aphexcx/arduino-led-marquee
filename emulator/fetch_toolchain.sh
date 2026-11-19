#!/bin/sh
# Fetches what the firmware is built with into emulator/third_party (gitignored)
# and lays it out as the "Arduino SDK" directory the repo's arduino-cmake
# toolchain file expects. Nothing is installed outside this directory; the
# Arduino IDE in /Applications is neither needed nor touched.
#
#   ./fetch_toolchain.sh          fetch (a second run does nothing)
#   ./fetch_toolchain.sh --path   only print where the SDK directory is or will be
#
# The Arduino AVR core (Arduino.h, the timers, the pin tables: everything the
# sketch gets from "Arduino"): ArduinoCore-avr 1.8.8, the newest.
#
# The compiler: avr-gcc 7.3.0-atmel3.6.1-arduino7. It is from 2018 and it is
# still the Arduino project's newest: core 1.8.8 asks for exactly this one. It
# is also the compiler of the Arduino IDE 1.8.19, which built every image
# before this script existed. For the Mac it is an Intel program: on an Apple
# Silicon Mac it needs Rosetta. (A newer avr-gcc was tried and not taken: see
# "The toolchain" in the README.)
#
# Both archives are the ones Arduino's package index names
# (downloads.arduino.cc/packages/package_index.json), pinned here by version
# and by the SHA-256 that index gives.
#
# The result is third_party/sdk-avr-<core version>-avr-gcc-<compiler version>/:
#   lib/version.txt          what arduino-cmake reads the IDE's version from
#   hardware/arduino/avr     -> the core
#   hardware/tools/avr       -> the compiler
#   libraries/               empty (the sketch's libraries are in the repo)
set -eu

CORE_VERSION=1.8.8
CORE_SHA256=a234c2a43dcd01ce54be665806f183b8e6ec4e966d2e3e0c3358b63023d6390c
CORE_URL=https://downloads.arduino.cc/cores/staging/avr-$CORE_VERSION.tar.bz2
CORE_DIR=ArduinoCore-avr-$CORE_VERSION            # the archive's own top directory

GCC_VERSION=7.3.0-atmel3.6.1-arduino7
GCC_SHA256=f6ed2346953fcf88df223469088633eb86de997fa27ece117fd1ef170d69c1f8
GCC_URL=https://downloads.arduino.cc/tools/avr-gcc-$GCC_VERSION-x86_64-apple-darwin14.tar.bz2
GCC_DIR=avr-gcc-$GCC_VERSION                      # (the archive's own top directory is "avr")

# The sketch is compiled with -DARDUINO=<this, as a number>. It is the version
# of the IDE that built the images until now, kept so that this number does
# not change with the toolchain; no code in the repo or in the core looks at
# more than "is it at least 1.0".
IDE_VERSION=1.8.19

# (the directory's one true name, so that `make -C emulator` and `cd emulator;
# make` name the same toolchain when a link is on the way to the checkout)
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd -P)
tp="$here/third_party"
dl="$tp/downloads"
sdk="$tp/sdk-avr-$CORE_VERSION-$GCC_DIR"

case "${1:-}" in
    "") ;;
    --path)
        printf '%s\n' "$sdk"
        exit 0 ;;
    *)
        echo "usage: fetch_toolchain.sh [--path]" >&2
        exit 2 ;;
esac

if [ -f "$sdk/complete.txt" ] && [ -x "$sdk/hardware/tools/avr/bin/avr-gcc" ] \
        && [ -f "$sdk/hardware/arduino/avr/boards.txt" ]; then
    exit 0
fi
case "$(uname -s)" in
    Darwin) ;;
    *)
        echo "fetch_toolchain: the compiler pinned here is Arduino's build for the Mac." >&2
        echo "fetch_toolchain: on $(uname -s), give make an Arduino SDK directory instead (ARDUINO_SDK=...)." >&2
        exit 1 ;;
esac
mkdir -p "$dl"

# fetch_unpack <url> <sha256> <the archive's top directory> <name under third_party>
fetch_unpack() {
    [ -d "$tp/$4" ] && return 0
    file="$dl/${1##*/}"
    if ! { [ -f "$file" ] && [ "$(shasum -a 256 "$file" | cut -d' ' -f1)" = "$2" ]; }; then
        echo "fetch_toolchain: fetching $1"
        curl -fsSL --retry 3 --connect-timeout 30 --max-time 1800 -o "$file.part" "$1"
        got=$(shasum -a 256 "$file.part" | cut -d' ' -f1)
        if [ "$got" != "$2" ]; then
            echo "fetch_toolchain: ${1##*/} has SHA-256 $got, expected $2. Not unpacking it." >&2
            exit 1
        fi
        mv "$file.part" "$file"
    fi
    # Unpacked beside its place and moved there whole, so that a run that was
    # interrupted leaves nothing that looks finished.
    tmp=$(mktemp -d "$tp/unpack.XXXXXX")
    if ! tar -xjf "$file" -C "$tmp"; then
        rm -r "$tmp"
        echo "fetch_toolchain: ${1##*/} could not be unpacked." >&2
        exit 1
    fi
    # (another run of this script in the same checkout may have put it there
    # meanwhile: then that one stands, and mv must not move ours into it)
    if [ -d "$tp/$4" ]; then
        rm -r "$tmp"
        return 0
    fi
    mv "$tmp/$3" "$tp/$4"
    rmdir "$tmp"
}

fetch_unpack "$CORE_URL" "$CORE_SHA256" "$CORE_DIR" "$CORE_DIR"
fetch_unpack "$GCC_URL" "$GCC_SHA256" avr "$GCC_DIR"

if ! "$tp/$GCC_DIR/bin/avr-gcc" --version > /dev/null 2>&1; then
    echo "fetch_toolchain: $tp/$GCC_DIR/bin/avr-gcc does not run on this computer." >&2
    echo "fetch_toolchain: it is an Intel program: on an Apple Silicon Mac it needs Rosetta." >&2
    exit 1
fi

mkdir -p "$sdk/lib" "$sdk/hardware/arduino" "$sdk/hardware/tools" "$sdk/libraries"
printf '%s\n' "$IDE_VERSION" > "$sdk/lib/version.txt"
[ -e "$sdk/hardware/arduino/avr" ] || ln -s "../../../$CORE_DIR" "$sdk/hardware/arduino/avr"
[ -e "$sdk/hardware/tools/avr" ] || ln -s "../../../$GCC_DIR" "$sdk/hardware/tools/avr"
printf '%s\n' "ArduinoCore-avr $CORE_VERSION, avr-gcc $GCC_VERSION" > "$sdk/complete.txt"
echo "fetch_toolchain: $(cat "$sdk/complete.txt") is in $sdk"
