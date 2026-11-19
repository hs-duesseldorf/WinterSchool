#!/bin/bash
# Serves WebBuild/Wasm with the headers Unreal's Wasm build needs and prints the URL.
#   Scripts/play_web.sh          local only: http://127.0.0.1:8900/
#   Scripts/play_web.sh --lan    also reachable from other devices on the network (phone).
#                                Phones only allow WebGPU over HTTPS: set TLS_CERT/TLS_KEY (PEM files), or put the
#                                local port behind HTTPS, e.g. `tailscale serve --bg --https=10000 http://127.0.0.1:8900`.
#   Scripts/play_web.sh --tailscale   local, plus this machine's Tailscale name allowed (for `tailscale serve`).
#   Scripts/play_web.sh --log    (combine with the above) a page opened with ?ue-session=1 sends its console log here:
#                                printed on stdout, the way to get a phone's log. The server stops when that game ends.
set -euo pipefail
source "$(dirname "$0")/config.sh"
[ -f "$WEBBUILD/Wasm/index.html" ] || { echo "No web build yet, run Scripts/build_web.sh first."; exit 1; }
SERVE=$ENGINE/Engine/Platforms/Wasm/Build/Tools/web/serve.py
[ -f "$SERVE" ] || SERVE=$WEBBUILD/serve.py
ARGS=(--root "$WEBBUILD/Wasm" --port "$PORT")
OPTS=" $* "
if [[ $OPTS == *" --lan "* ]]; then
  IP=$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')
  ARGS+=(--bind 0.0.0.0 --allow-host "$IP"); S=http
  [ -n "${TLS_CERT:-}" ] && { ARGS+=(--tls-cert "$TLS_CERT" --tls-key "$TLS_KEY"); S=https; }
  echo "On your network: $S://$IP:$PORT/index.html"
fi
if [[ $OPTS == *" --tailscale "* ]]; then
  TS=$(tailscale status --self --json | "$PY" -I -c "import json, sys; print(json.load(sys.stdin)['Self']['DNSName'].rstrip('.'))")
  ARGS+=(--allow-host "$TS")
  tailscale serve status 2>/dev/null | grep -B1 "127.0.0.1:$PORT\b" | sed -n "s|^\(https://[^ ]*\).*|Tailscale: \1/index.html|p"
fi
[[ $OPTS == *" --log "* ]] && { ARGS+=(--game-run); echo "Logging pages opened with ?ue-session=1 (add it to the URL)"; }
echo "Local: http://127.0.0.1:$PORT/index.html  (Chrome or Edge with WebGPU; on Linux enable Vulkan in chrome://flags)"
[[ $OPTS == *" --log "* ]] && exec "$PY" -I -B "$ROOT/Scripts/serve_phone_log.py" "$SERVE" "${ARGS[@]}"
exec "$PY" -I -B "$SERVE" "${ARGS[@]}"
