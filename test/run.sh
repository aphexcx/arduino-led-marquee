#!/bin/bash
# Runs the host tests of the chooser display and of the ads (no Arduino needed, only the Mac's clang++).
#
#   test/run.sh          build and run all the tests
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

clang++ -std=c++11 -Wall -Wextra -O1 -o "$out/led_budget_test" "$here/led_budget_test.cpp"
"$out/led_budget_test"

# The ads again with the supply's settings on (they are off in the sketch): a limit for the two
# sides together with a ramp, a limit for one side, both limits, and the ramp alone
for settings in "-DLED_CURRENT_LIMIT_MA=3000 -DAD_RAMP_MS=100" "-DLED_CURRENT_LIMIT_SIDE_MA=1200" \
                "-DLED_CURRENT_LIMIT_MA=6000 -DLED_CURRENT_LIMIT_SIDE_MA=2000 -DAD_RAMP_MS=35" "-DAD_RAMP_MS=100"; do
    # shellcheck disable=SC2086
    clang++ -std=c++11 -Wall -Wextra -O1 $settings -o "$out/ad_display_test_budget" "$here/ad_display_test.cpp"
    "$out/ad_display_test_budget"
done
