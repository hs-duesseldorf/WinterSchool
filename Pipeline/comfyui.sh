#!/usr/bin/env bash
# Start ComfyUI with the ProfSplat nodes and the Professor workflow, open it in the browser.
#   ./comfyui.sh            start (or just open, if it is already running)
#   PORT=8190 ./comfyui.sh  another port
#   ./comfyui.sh --stop     stop a ComfyUI started by this script
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${PROFSPLAT_ROOT:-$HERE/work}"   # envs, models, ComfyUI install, outputs
PORT="${PORT:-8188}"
COMFY="$WORK/third_party/ComfyUI"
URL="http://127.0.0.1:$PORT"
LOG="$WORK/logs/comfyui_$PORT.log"
PIDF="$WORK/logs/comfyui_$PORT.pid"

if [[ "${1:-}" == "--stop" ]]; then
  [[ -f "$PIDF" ]] && kill "$(cat "$PIDF")" 2>/dev/null && rm -f "$PIDF" && echo "stopped" || echo "not running"
  exit 0
fi

# the workflow next to this script shows up in ComfyUI's workflow sidebar, the Eingabebilder in Load Image
mkdir -p "$COMFY/user/default/workflows" "$WORK/logs"
for wf in "$HERE"/Professor.json; do
  [[ -f "$wf" ]] && cp -f "$wf" "$COMFY/user/default/workflows/"
done
for im in "$HERE"/Eingabebilder/*; do
  [[ -f "$im" ]] && cp -n "$im" "$COMFY/input/" || true
done

if ! curl -sf "$URL/system_stats" >/dev/null; then
  echo "starting ComfyUI on $URL (log: $LOG)"
  cd "$COMFY"
  PROFSPLAT_ROOT="$WORK" setsid "$WORK/envs/comfy/bin/python" main.py --listen 127.0.0.1 --port "$PORT" \
    --disable-auto-launch --disable-pinned-memory >"$LOG" 2>&1 &
  echo $! >"$PIDF"
  for _ in $(seq 150); do
    curl -sf "$URL/system_stats" >/dev/null && break
    kill -0 "$(cat "$PIDF")" 2>/dev/null || { echo "ComfyUI exited, see $LOG"; tail -20 "$LOG"; exit 1; }
    sleep 2
  done
fi
echo "ComfyUI: $URL  (workflow: Workflows sidebar -> Professor.json)"
[[ "${NO_BROWSER:-}" ]] || xdg-open "$URL" >/dev/null 2>&1 || true
