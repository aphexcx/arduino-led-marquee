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
#      that come and go (tests/link_test.py); and what fake_app.py --contract 3
#      writes when it leaves chooser mode, as a listener on the sign's receive
#      pin decodes it (tests/fake_app_wire_test.py)
#   5. chooser contract version 3 on the real firmware, if this firmware has it:
#      a scripted app that pushes (tests/push_test.py), the same against the
#      bench image (make firmware-debug: its numbers on panel B and its line of
#      numbers to the app are read and held against the wire; and a copy of that
#      run with panel B left on an older attention must fail), then fake_app.py
#      --contract 3 at real-time speed, checked the same way
#   6. the ads on the real firmware: letters one by one and chonky, short
#      texts and texts wider than a panel, hearts, and how long each stays
#      (tests/ads_test.py)
#   7. messages the sign must survive and draw as they read: brightness
#      numbers of any size, keyboard lines of any length, empty texts, a
#      negative number of stars, icons it does not have (tests/fixes_test.py)
#   8. nothing of the picture before a track id or the icons stays lit beside
#      them (tests/leftover_test.py)
#   9. the jump game: every frame of a list is drawn as its text reads, answers
#      that are not a good frame leave the picture and are asked for again, and
#      a scripted game's frames a second (tests/game_test.py)
#   Before 7: the checks of 7 and 8 themselves, on frames in memory: right
#   frames pass, and a message with one part missing, cut short, dark, moved,
#   out of order, a bit too long or in another colour does not
#   (tests/fixes_selftest.py; no emulator, a minute)
#
# The TCP ports can be moved with SIGN_EMU_TEST_PORT (default 14417; the link
# tests use the eleven after it, the push tests the four after those, the ads
# test three from the sixteenth, the test of 7 seven from the twenty-fourth, the
# test of 8 two from the thirty-first, the test of 9 five from the fortieth) so
# two lanes can run the tests at once.
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

echo "== unit: the checkers' reading of Fonts.h"
python3 tests/push_test.py font ../Fonts.h

messages=$(grep -c '^{' "$PLAYLIST")

echo "== smoke run: boot, startup animation, then $messages messages from $PLAYLIST"
# The fake app plays the list once and hangs up at the request after its last
# message, that is when the sign has finished drawing it; the run ends there.
# (Not after a number of requests: firmware that polls spends requests on its
# polls, and the last message would never be served.) --duration is only a
# safety net.
build/sign-emu --headless --port "$PORT" --wait-client 30 \
    --until-disconnect --duration 300 \
    --record "$OUT/all_types.gif" --frames "$OUT/all_types.jsonl" \
    "$HEX" 2> "$OUT/sign-emu.log" &
emu=$!
if ! ./fake_app.py --quiet --once --port "$PORT" --connect-timeout 30 "$PLAYLIST"; then
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

# (the link tests are over: their first port is free again)
contract3=""
if grep -q CHOOSER_ATTN_FIRST_BYTE_MS ../LED_Marquee.ino; then
    contract3="--contract3"
fi
echo "== the fake app leaving chooser mode: its break and its answer, on the wire"
python3 tests/fake_app_wire_test.py build/sign-emu "$HEX" build/tests/fake-wire $((PORT + 1)) $contract3

if [ -n "$contract3" ]; then
    echo "== chooser contract 3: a scripted app that pushes"
    python3 tests/push_test.py run build/sign-emu "$HEX" ../Fonts.h build/tests/push $((PORT + 12))

    echo "== chooser contract 3: a push moved through an idle sign's own request"
    python3 tests/push_test.py idle build/sign-emu "$HEX" ../Fonts.h build/tests/push-idle $((PORT + 15))

    echo "== chooser contract 3: a pushed message's first byte moved across the sign's request byte"
    python3 tests/push_test.py cross build/sign-emu "$HEX" ../Fonts.h build/tests/push-cross $((PORT + 15))

    echo "== chooser contract 3: the same against the bench image (numbers on panel B and to the app)"
    make --no-print-directory firmware-debug
    python3 tests/push_test.py run --bench build/sign-emu build/firmware-debug/LED_Marquee.hex \
        ../Fonts.h build/tests/push-debug $((PORT + 14))

    echo "== chooser contract 3: the bench check itself (panel B left on an older attention must fail)"
    python3 tests/push_test.py stuck-panel ../Fonts.h build/tests/push-debug/push.jsonl \
        build/tests/push-stuck

    echo "== chooser contract 3: fake_app.py --contract 3, at real-time speed"
    # The fake app counts this computer's seconds, so the run is paced. Request 1
    # brings the first chooser message, request 2 is the sign asking after the
    # app left chooser mode, request 3 ends the run.
    build/sign-emu --headless --speed 1 --link-directives --port $((PORT + 13)) --wait-client 30 \
        --until-requests 3 --duration 120 --frames "$OUT/chooser_push.jsonl" \
        "$HEX" 2> "$OUT/chooser_push.log" &
    emu=$!
    if ! ./fake_app.py --quiet --contract 3 --port $((PORT + 13)) --connect-timeout 30 \
            playlists/chooser_push.jsonl; then
        kill "$emu" 2>/dev/null || true
        echo "FAILED: the fake app could not run"
        exit 1
    fi
    status=0
    wait "$emu" || status=$?
    cat "$OUT/chooser_push.log"
    if [ "$status" -ne 0 ]; then
        echo "FAILED: sign-emu exited with status $status"
        exit 1
    fi
    # (bytes glued to a break, or anything else the wire did not carry as it was sent)
    if grep -q WARNING "$OUT/chooser_push.log"; then
        echo "FAILED: sign-emu warned about this run"
        exit 1
    fi
    python3 tests/push_test.py check ../Fonts.h "$OUT/chooser_push.jsonl"
else
    echo "== chooser contract 3: SKIPPED, this firmware does not have it (no"
    echo "   CHOOSER_ATTN_FIRST_BYTE_MS in ../LED_Marquee.ino). The break directive itself was"
    echo "   tested above, on the wire."
fi

echo "== the ads: one by one and chonky, short and long, on the real firmware"
python3 tests/ads_test.py build/sign-emu "$HEX" .. build/tests/ads $((PORT + 16))

echo "== the checks of the next two tests themselves: right frames pass, a changed part fails"
python3 tests/fixes_selftest.py ..

echo "== messages the sign must survive: brightness numbers, long keyboard lines, empty texts, unknown icons"
python3 tests/fixes_test.py build/sign-emu "$HEX" .. build/tests/fixes $((PORT + 24))

echo "== a track id and the icons after a text: nothing of the text stays lit beside them"
python3 tests/leftover_test.py build/sign-emu "$HEX" .. build/tests/leftover $((PORT + 31))

echo "== the jump game: its pictures, answers that are no frame, its pace"
python3 tests/game_test.py build/sign-emu "$HEX" .. build/tests/game $((PORT + 40))

echo "ALL TESTS PASSED"
