#!/bin/sh
# Fetches what the firmware is built with into emulator/third_party (gitignored)
# and lays it out as the "Arduino SDK" directory the repo's arduino-cmake
# toolchain file expects. Nothing is installed outside this directory; the
# Arduino IDE in /Applications is neither needed nor touched.
#
#   ./fetch_toolchain.sh <compiler>          fetch (a second run does nothing)
#   ./fetch_toolchain.sh --path <compiler>   only print where it is or will be
#
#   arduino-7.3.0    avr-gcc 7.3.0-atmel3.6.1-arduino7, the Arduino project's own
#                    newest build (2018; an Intel program, it needs Rosetta on an
#                    Apple Silicon Mac). This is the compiler of the Arduino IDE
#                    1.8.19, which built every image before this script existed.
#
# The Arduino AVR core (Arduino.h, the timers, the pin tables: everything the
# sketch gets from "Arduino") is ArduinoCore-avr 1.8.8.
#
# Every archive is pinned by version and SHA-256; the two from Arduino are the
# ones its package index (downloads.arduino.cc/packages/package_index.json)
# names, with the checksums it gives.
#
# The result is third_party/sdk-avr-<core version>-<compiler>/:
#   lib/version.txt          what arduino-cmake reads the IDE's version from
#   hardware/arduino/avr     -> the core
#   hardware/tools/avr       -> the compiler
#   libraries/               empty (the sketch's libraries are in the repo)
set -eu

CORE_VERSION=1.8.8
CORE_SHA256=a234c2a43dcd01ce54be665806f183b8e6ec4e966d2e3e0c3358b63023d6390c
CORE_URL=https://downloads.arduino.cc/cores/staging/avr-$CORE_VERSION.tar.bz2
CORE_DIR=ArduinoCore-avr-$CORE_VERSION            # the archive's own top directory

ARDUINO_GCC_VERSION=7.3.0-atmel3.6.1-arduino7
ARDUINO_GCC_SHA256=f6ed2346953fcf88df223469088633eb86de997fa27ece117fd1ef170d69c1f8
ARDUINO_GCC_URL=https://downloads.arduino.cc/tools/avr-gcc-$ARDUINO_GCC_VERSION-x86_64-apple-darwin14.tar.bz2

# The sketch is compiled with -DARDUINO=<this, as a number>. It is the version
# of the IDE that built the images until now, kept so that this number does
# not change with the toolchain; no code in the repo or in the core looks at
# more than "is it at least 1.0".
IDE_VERSION=1.8.19

COMPILERS="arduino-7.3.0"

only_path=0
if [ "${1:-}" = --path ]; then
    only_path=1
    shift
fi
if [ $# -ne 1 ]; then
    echo "usage: fetch_toolchain.sh [--path] <compiler>, one of: $COMPILERS" >&2
    exit 2
fi
compiler=$1
here=$(cd "$(dirname "$0")" && pwd)
tp="$here/third_party"
dl="$tp/downloads"
sdk="$tp/sdk-avr-$CORE_VERSION-$compiler"

case "$compiler" in
    arduino-7.3.0)         tools="avr-gcc-$ARDUINO_GCC_VERSION" ;;
    *)
        echo "fetch_toolchain: no compiler '$compiler' (there is: $COMPILERS)" >&2
        exit 2 ;;
esac

if [ "$only_path" = 1 ]; then
    printf '%s\n' "$sdk"
    exit 0
fi
if [ -f "$sdk/complete.txt" ]; then
    exit 0
fi
mkdir -p "$dl"

# fetch_unpack <url> <sha256> <the archive's top directory> <name under third_party>
fetch_unpack() {
    [ -d "$tp/$4" ] && return 0
    file="$dl/${1##*/}"
    if ! { [ -f "$file" ] && [ "$(shasum -a 256 "$file" | cut -d' ' -f1)" = "$2" ]; }; then
        echo "fetch_toolchain: fetching $1"
        curl -fsSL --retry 3 -o "$file.part" "$1"
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
    tar -xjf "$file" -C "$tmp"
    mv "$tmp/$3" "$tp/$4"
    rmdir "$tmp"
}

fetch_unpack "$CORE_URL" "$CORE_SHA256" "$CORE_DIR" "$CORE_DIR"
case "$compiler" in
    arduino-*) fetch_unpack "$ARDUINO_GCC_URL" "$ARDUINO_GCC_SHA256" avr "$tools" ;;
esac

if ! "$tp/$tools/bin/avr-gcc" --version > /dev/null 2>&1; then
    echo "fetch_toolchain: $tp/$tools/bin/avr-gcc does not run on this computer." >&2
    case "$compiler" in
        arduino-*) echo "fetch_toolchain: Arduino's avr-gcc for the Mac is an Intel program: it needs Rosetta." >&2 ;;
    esac
    exit 1
fi

mkdir -p "$sdk/lib" "$sdk/hardware/arduino" "$sdk/hardware/tools" "$sdk/libraries"
printf '%s\n' "$IDE_VERSION" > "$sdk/lib/version.txt"
[ -e "$sdk/hardware/arduino/avr" ] || ln -s "../../../$CORE_DIR" "$sdk/hardware/arduino/avr"
[ -e "$sdk/hardware/tools/avr" ] || ln -s "../../../$tools" "$sdk/hardware/tools/avr"
printf '%s\n' "ArduinoCore-avr $CORE_VERSION, $("$tp/$tools/bin/avr-gcc" --version | head -n 1)" > "$sdk/complete.txt"
echo "fetch_toolchain: $(cat "$sdk/complete.txt") is in $sdk"
