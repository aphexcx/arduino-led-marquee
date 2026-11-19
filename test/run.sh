#!/bin/bash
# Runs the host tests of the chooser display and of the ads (no Arduino needed, only the Mac's clang++).
#
#   test/run.sh          build and run all four tests
#   test/run.sh --show   also print a few frames of the sign as ASCII art
#
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp -d)"
trap 'rm -r "$out"' EXIT

clang++ -std=c++11 -Wall -Wextra -o "$out/chooser_scroll_test" "$here/chooser_scroll_test.cpp"
"$out/chooser_scroll_test"

clang++ -std=c++11 -Wall -Wextra -o "$out/chooser_display_test" "$here/chooser_display_test.cpp"
"$out/chooser_display_test" "$@"

clang++ -std=c++11 -Wall -Wextra -o "$out/text_window_test" "$here/text_window_test.cpp"
"$out/text_window_test"

clang++ -std=c++11 -Wall -Wextra -O1 -o "$out/ad_display_test" "$here/ad_display_test.cpp"
"$out/ad_display_test" "$@"

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
