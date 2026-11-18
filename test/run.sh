#!/bin/bash
# Runs the host tests of the chooser display (no Arduino needed, only the Mac's clang++).
#
#   test/run.sh          build and run the tests
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
