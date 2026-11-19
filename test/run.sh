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
