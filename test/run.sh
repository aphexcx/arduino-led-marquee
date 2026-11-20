#!/bin/bash
# Runs the host tests of the chooser display and of the ads (no Arduino needed, only the Mac's clang++).
#
#   test/run.sh          build and run all the tests
#   test/ad_display_grid.py   the ads' test over every wave interval and many scroll steps (minutes)
#   test/run.sh --show   also print a few frames of the sign as ASCII art
#
# The display test runs twice: as the sign is built normally, and as it is built with
# CHOOSER_DEBUG_PANEL_B (the measurements on panel 2), so that build is known to behave the same.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp -d)"
trap 'rm -r "$out"' EXIT

clang++ -std=c++11 -Wall -Wextra -o "$out/chooser_scroll_test" "$here/chooser_scroll_test.cpp"
"$out/chooser_scroll_test"

clang++ -std=c++11 -Wall -Wextra -o "$out/chooser_display_test" "$here/chooser_display_test.cpp"
"$out/chooser_display_test" "$@"

clang++ -std=c++11 -Wall -Wextra -DCHOOSER_DEBUG_PANEL_B=1 -o "$out/chooser_display_debug_test" \
    "$here/chooser_display_test.cpp"
"$out/chooser_display_debug_test" "$@"

clang++ -std=c++11 -Wall -Wextra -o "$out/text_window_test" "$here/text_window_test.cpp"
"$out/text_window_test"

clang++ -std=c++11 -Wall -Wextra -o "$out/brightness_shift_test" "$here/brightness_shift_test.cpp"
"$out/brightness_shift_test"

clang++ -std=c++11 -Wall -Wextra -O1 -o "$out/ad_display_test" "$here/ad_display_test.cpp"
"$out/ad_display_test" "$@"

# The same test with other wave intervals: what it expects is worked out from the settings. 20 ms
# divides the hold at the start (600 ms), 16 ms does not: the last frame of the hold comes 8 ms
# before the text moves. (Every wave interval, with steps that are and are not multiples of it:
# test/ad_display_grid.py, about 2000 builds, not run from here.)
for wave in 20 16; do
    clang++ -std=c++11 -Wall -Wextra -O1 -DAD_WAVE_FRAME_MS=$wave -o "$out/ad_display_test_$wave" "$here/ad_display_test.cpp"
    echo "ad_display_test with AD_WAVE_FRAME_MS $wave:"
    "$out/ad_display_test_$wave"
done

# AdDisplay.h refuses a scroll step and a wave interval that cannot keep pace: 9 ms with 6 ms must
# not compile, and for that reason; 12 ms with 6 ms must compile.
if clang++ -std=c++11 -fsyntax-only -DAD_SCROLL_STEP_MS=9 -DAD_WAVE_FRAME_MS=6 \
        "$here/ad_display_test.cpp" 2> "$out/refused.txt"; then
    echo "FAILED: AdDisplay.h accepted a 9 ms scroll step with a 6 ms wave frame"
    exit 1
fi
if ! grep -q "the wave frames of one scrolled column" "$out/refused.txt"; then
    echo "FAILED: a 9 ms scroll step with a 6 ms wave frame does not compile, but not for AdDisplay.h's reason:"
    cat "$out/refused.txt"
    exit 1
fi
clang++ -std=c++11 -fsyntax-only -DAD_SCROLL_STEP_MS=12 -DAD_WAVE_FRAME_MS=6 "$here/ad_display_test.cpp"
echo "ad_display_test: a 9 ms scroll step with a 6 ms wave frame is refused, 12 ms with 6 ms is not"

# test/ad_display_grid.py itself: a command line that would test nothing (a range from the larger
# interval to the smaller, an option it does not know, a number that is none) is exit status 2 and
# a line that says why, never a summary with 0 pairs and status 0. And one wave interval, the
# shortest (5 ms: every one of its pairs must be refused by the header), is run for real.
refused=0
for bad in "--waves 20-16" "--waves 20" "--waves 0-3" "--waves 5-2000" "--waves a-b" "--waves -5-3" "--waves" \
           "--waves 10-" "--waves -" "--jobs 0" "--jobs x" "--jobs -2" "--jobs" "--wave 10-10" "10" \
           "--waves 10-10 extra" "--waves 10-10 --jobs" "--waves 6-100 --waves 5-5" "--jobs 2 --jobs 4" "--jobs ²"; do
    status=0
    # (the words of $bad are the tool's arguments)
    # shellcheck disable=SC2086
    python3 "$here/ad_display_grid.py" $bad > "$out/grid.txt" 2>&1 || status=$?
    if [ "$status" -ne 2 ] || ! grep -q "^ad_display_grid: ..*" "$out/grid.txt" \
            || grep -q "pairs of step and wave" "$out/grid.txt"; then
        echo "FAILED: ad_display_grid.py $bad: exit status $status; wanted 2, a line that says why, and no summary:"
        cat "$out/grid.txt"
        exit 1
    fi
    refused=$((refused + 1))
done
python3 "$here/ad_display_grid.py" --waves 5-5 --jobs 4 > "$out/grid.txt"
if ! grep -q "^ad_display_grid: [1-9][0-9]* pairs of step and wave (waves 5 to 5 ms): 0 accepted.* 0 not as worked out$" \
        "$out/grid.txt"; then
    echo "FAILED: ad_display_grid.py --waves 5-5 did not refuse every pair of a 5 ms wave:"
    cat "$out/grid.txt"
    exit 1
fi
echo "ad_display_grid.py: $refused command lines that would test nothing are refused, each with its reason;"
echo "  $(cat "$out/grid.txt")"
