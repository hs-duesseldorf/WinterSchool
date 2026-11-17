#!/bin/bash
# Serves WebBuild/Wasm with the headers Unreal's Wasm build needs and prints the URL.
#   Scripts/play_web.sh          local only: http://127.0.0.1:8900/
#   Scripts/play_web.sh --lan    also reachable from other devices on the network (phone).
#                                Phones only allow WebGPU over HTTPS: set TLS_CERT/TLS_KEY (PEM files), or put the
#                                local port behind HTTPS, e.g. `tailscale serve --bg --https=10000 http://127.0.0.1:8900`.
set -euo pipefail
source "$(dirname "$0")/config.sh"
[ -f "$WEBBUILD/Wasm/index.html" ] || { echo "No web build yet, run Scripts/build_web.sh first."; exit 1; }
SERVE=$ENGINE/Engine/Platforms/Wasm/Build/Tools/web/serve.py
[ -f "$SERVE" ] || SERVE=$WEBBUILD/serve.py
ARGS=(--root "$WEBBUILD/Wasm" --port "$PORT")
if [ "${1:-}" = "--lan" ]; then
  IP=$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')
  ARGS+=(--bind 0.0.0.0 --allow-host "$IP"); S=http
  [ -n "${TLS_CERT:-}" ] && { ARGS+=(--tls-cert "$TLS_CERT" --tls-key "$TLS_KEY"); S=https; }
  echo "On your network: $S://$IP:$PORT/index.html"
fi
echo "Local: http://127.0.0.1:$PORT/index.html  (Chrome or Edge with WebGPU; on Linux enable Vulkan in chrome://flags)"
exec "$PY" -I -B "$SERVE" "${ARGS[@]}"
