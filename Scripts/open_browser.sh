#!/bin/bash
# Opens the web build in Playwright's Chromium with a throwaway profile (never a real browser profile)
# and the switches Chrome on Linux needs for WebGPU.
TOOLS=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
URL=${1:-http://127.0.0.1:8900/index.html}
CHROME=$(ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux64/chrome 2>/dev/null | sort -V | tail -1)
[ -x "$CHROME" ] || { echo "kein Chromium gefunden; öffne $URL in Chrome mit --enable-features=Vulkan --use-angle=vulkan"; exit 1; }
PROFILE=$(dirname "$TOOLS")/Saved/BrowserProfile
mkdir -p "$PROFILE"
setsid nohup "$CHROME" --user-data-dir="$PROFILE" --no-first-run --no-default-browser-check \
  --enable-features=Vulkan --use-angle=vulkan --enable-unsafe-webgpu --ignore-gpu-blocklist \
  "$URL" > /dev/null 2>&1 < /dev/null &
echo "Chromium geöffnet: $URL"
