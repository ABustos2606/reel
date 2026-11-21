#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alejandro Bustos
#
# Renders tools/mockup/reel-page.html to docs/images/mockup-reel-*.png at
# 800 x 480 with headless Chrome. Usage: tools/mockup/render.sh [chrome]
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CHROME=${1:-"/c/Program Files/Google/Chrome/Application/chrome.exe"}
mkdir -p "$ROOT/docs/images"
n=1
for state in on-keyframe between curves sequencer clear; do
    out="$ROOT/docs/images/mockup-reel-$n-$state.png"
    page="file:///$(cygpath -m "$ROOT/tools/mockup/reel-page.html" 2>/dev/null || echo "$ROOT/tools/mockup/reel-page.html")#$state"
    "$CHROME" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 \
        --allow-file-access-from-files --window-size=800,480 \
        --screenshot="$(cygpath -w "$out" 2>/dev/null || echo "$out")" "$page" >/dev/null 2>&1
    echo "$out"
    n=$((n + 1))
done
