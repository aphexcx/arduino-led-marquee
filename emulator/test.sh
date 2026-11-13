#!/bin/sh
# Smoke test for the sign emulator. Needs no human and no hardware.
#
# Builds the firmware and the emulator, boots the firmware, lets the startup
# animation play, serves playlists/all_types.jsonl once through the serial
# link, records a GIF and a frames dump under build/test/, and checks the
# frames against Fonts.h (tests/check_frames.py).
#
# The TCP port can be moved with SIGN_EMU_TEST_PORT (default 14417) so two
# lanes can run the test at once.
set -eu
cd "$(dirname "$0")"

PORT=${SIGN_EMU_TEST_PORT:-14417}
OUT=build/test
HEX=build/firmware/LED_Marquee.hex
PLAYLIST=playlists/all_types.jsonl

echo "== build"
make --no-print-directory all

mkdir -p "$OUT"
messages=$(grep -c '^{' "$PLAYLIST")

echo "== run: boot, startup animation, then $messages messages from $PLAYLIST"
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

echo "== check"
python3 tests/check_frames.py ../Fonts.h "$OUT/all_types.jsonl" "$OUT/all_types.gif"
ls -l "$OUT/all_types.gif" "$OUT/all_types.jsonl"
