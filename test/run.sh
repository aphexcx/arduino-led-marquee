#!/bin/bash
# Runs the host tests of the chooser display (no Arduino needed, only the Mac's clang++).
#
#   test/run.sh          build and run both tests
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
