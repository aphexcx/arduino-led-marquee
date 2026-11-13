#!/bin/sh
# Tests for the sign emulator. Needs no human and no hardware.
#
#   1. unit tests: the HEX loader (built with the address and undefined-
#      behaviour sanitizers), the GIF encoder against a strict decoder, the
#      fake app's checksum and answers
#   2. the smoke run: boots the firmware, lets the startup animation play,
#      serves playlists/all_types.jsonl once through the serial link, records a
#      GIF and a frames dump under build/test/, and checks the frames against
#      Fonts.h and the GIF against the frames (tests/check_frames.py)
#   3. the command line, and recordings that cannot be written
#      (tests/cli_test.py, tests/record_fail_test.py)
#   4. the serial link's timing model against the real firmware, with clients
#      that come and go (tests/link_test.py)
#
# The TCP ports can be moved with SIGN_EMU_TEST_PORT (default 14417; the link
# tests use the eleven after it) so two lanes can run the tests at once.
set -eu
cd "$(dirname "$0")"

PORT=${SIGN_EMU_TEST_PORT:-14417}
OUT=build/test
HEX=build/firmware/LED_Marquee.hex
PLAYLIST=playlists/all_types.jsonl

echo "== build"
make --no-print-directory all test-programs

mkdir -p "$OUT"

echo "== unit: HEX loader (sanitizers on)"
mkdir -p build/tests/ihex
build/tests/test_ihex build/tests/ihex

echo "== unit: GIF encoder against a strict decoder"
python3 tests/gif_cases.py build/tests/gif_case build/tests/gif

echo "== unit: fake app"
python3 tests/test_fake_app.py build/tests/fake_app

messages=$(grep -c '^{' "$PLAYLIST")

echo "== smoke run: boot, startup animation, then $messages messages from $PLAYLIST"
# The run ends when the sign asks for message number messages + 1, that is
# when it has finished drawing the last one. --duration is only a safety net.
build/sign-emu --headless --port "$PORT" --wait-client 30 \
    --until-requests $((messages + 1)) --duration 300 \
    --record "$OUT/all_types.gif" --frames "$OUT/all_types.jsonl" \
    "$HEX" 2> "$OUT/sign-emu.log" &
emu=$!
if ! ./fake_app.py --quiet --port "$PORT" --connect-timeout 30 "$PLAYLIST"; then
    kill "$emu" 2>/dev/null || true
    cat "$OUT/sign-emu.log"
    echo "FAILED: the fake app could not run"
    exit 1
fi
status=0
wait "$emu" || status=$?
cat "$OUT/sign-emu.log"
if [ "$status" -ne 0 ]; then
    echo "FAILED: sign-emu exited with status $status"
    exit 1
fi

echo "== check the smoke run"
python3 tests/check_frames.py ../Fonts.h "$OUT/all_types.jsonl" "$OUT/all_types.gif"
ls -l "$OUT/all_types.gif" "$OUT/all_types.jsonl"

echo "== a firmware path with a quote and a backslash in it"
mkdir -p "$OUT/path"
odd="$OUT/path/a\"b\\c.hex"
cp "$HEX" "$odd"
build/sign-emu --headless --port 0 --duration 0.2 --frames "$OUT/path/out.jsonl" "$odd" 2> /dev/null
python3 tests/check_meta.py "$OUT/path/out.jsonl" "$odd"

echo "== command line"
python3 tests/cli_test.py build/sign-emu "$HEX" build/tests/cli

echo "== recordings that cannot be written"
python3 tests/record_fail_test.py build/sign-emu "$HEX" build/tests/recfail

echo "== serial link timing"
python3 tests/link_test.py build/sign-emu "$HEX" build/tests/link $((PORT + 1))

echo "ALL TESTS PASSED"
