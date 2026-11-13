#!/bin/sh
# Fetches simavr (the AVR simulator the emulator is built on) at a pinned
# commit into emulator/third_party/simavr. That directory is gitignored.
# Homebrew has no simavr formula for this Mac, so it is built from source by
# the Makefile; nothing is installed outside this directory.
set -eu

TAG=v1.8
COMMIT=f44723e8c42431136d5b4de81f789ded56d7e8fa
URL=https://github.com/buserror/simavr.git

here=$(cd "$(dirname "$0")" && pwd)
dest="$here/third_party/simavr"

if [ -d "$dest/.git" ]; then
    have=$(git -C "$dest" rev-parse HEAD)
    if [ "$have" = "$COMMIT" ]; then
        exit 0
    fi
    echo "fetch_simavr: $dest is at $have, expected $COMMIT ($TAG)." >&2
    echo "fetch_simavr: move that directory away and run this again." >&2
    exit 1
fi

mkdir -p "$here/third_party"
git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$TAG" "$URL" "$dest"
have=$(git -C "$dest" rev-parse HEAD)
if [ "$have" != "$COMMIT" ]; then
    echo "fetch_simavr: tag $TAG is now $have, expected $COMMIT. Not building it." >&2
    exit 1
fi
echo "fetch_simavr: simavr $TAG ($COMMIT) is in $dest"
