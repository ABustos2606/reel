#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alejandro Bustos
#
# Copy the upstream files Reel needs into third_party/eurorack/, at the pinned
# commits, and apply Reel's local changes from tools/patches/ (there are none:
# see third_party/eurorack/README.md). Run by hand (it needs network access);
# the build never runs it.
# Usage: tools/vendor.sh [scratch-dir]
set -eu

EURORACK_URL=https://github.com/pichenettes/eurorack.git
EURORACK_COMMIT=f4cfa7697095a28d292bff758beb51deb49b5a86
# The stmlib submodule commit recorded in eurorack's tree at EURORACK_COMMIT.
STMLIB_URL=https://github.com/pichenettes/stmlib.git
STMLIB_COMMIT=e3bd7c9cc00e4364166f9905c0509b6ffd0535ec

# The keyframer, the poly LFO, their tables and the script that made them, and
# the stmlib files they include (found with g++ -MM -DTEST). Upstream's LICENSE
# file is stmlib's; the frames files carry their notice at the top.
EURORACK_FILES="
frames/keyframer.cc
frames/keyframer.h
frames/poly_lfo.cc
frames/poly_lfo.h
frames/resources.cc
frames/resources.h
frames/resources/lookup_tables.py
"
STMLIB_FILES="
LICENSE
stmlib.h
utils/dsp.h
"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCRATCH=${1:-$ROOT/tmp/vendor}
DEST=$ROOT/third_party/eurorack

fetch() { # url commit dir
    rm -rf "$3"; mkdir -p "$3"
    git -C "$3" init -q
    git -C "$3" fetch -q --depth 1 "$1" "$2"
    git -C "$3" -c advice.detachedHead=false checkout -q FETCH_HEAD
}

fetch "$EURORACK_URL" "$EURORACK_COMMIT" "$SCRATCH/eurorack"
fetch "$STMLIB_URL" "$STMLIB_COMMIT" "$SCRATCH/stmlib"
test "$(git -C "$SCRATCH/eurorack" ls-tree HEAD stmlib | awk '{print $3}')" = "$STMLIB_COMMIT" ||
    { echo "stmlib commit does not match eurorack's submodule" >&2; exit 1; }

rm -rf "$DEST/frames" "$DEST/stmlib"
for f in $EURORACK_FILES; do
    mkdir -p "$DEST/$(dirname "$f")"; cp "$SCRATCH/eurorack/$f" "$DEST/$f"
done
for f in $STMLIB_FILES; do
    mkdir -p "$DEST/stmlib/$(dirname "$f")"; cp "$SCRATCH/stmlib/$f" "$DEST/stmlib/$f"
done
# Reel's local changes, if any are ever needed (implementation plan, section 7).
for p in "$ROOT"/tools/patches/*.patch; do
    [ -e "$p" ] || continue
    patch -s -p1 -d "$DEST" < "$p"
done
echo "vendored eurorack $EURORACK_COMMIT and stmlib $STMLIB_COMMIT into $DEST"
