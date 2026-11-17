#!/bin/bash
# UE 5.9 Preview 1 + Gaussian Splat -> Browser (Wasm/WebGPU)
# Waits for the Linux engine zip in ~/Downloads, installs it + Epic's QFE, makes a C++ project with one
# splat scene, builds it for the web, serves it on http://127.0.0.1:8900 and opens Chromium.
# Every step writes a marker into logs/, so a re-run continues where it stopped.
#
#   ./run.sh                    # whole pipeline
#   ./run.sh --from <step>      # redo from a step: install qfe project build scene web serve
#   SPLAT_PLY=/path/x.ply ./run.sh
set -uo pipefail

TOOLS=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
LOGS=$TOOLS/logs; mkdir -p "$LOGS"
ENGINE=${ENGINE:-$HOME/Programs/UnrealEngine/5.9.0-preview-1-full}
QFE=${QFE:-$HOME/Downloads/UE59-QFE/qfe.zip}
NAME=SplatWeb
PROJDIR=${PROJDIR:-$(dirname "$TOOLS")}
UPROJECT=$PROJDIR/$NAME.uproject
PROF=${PROF:-$HOME/Programs/WinterSchool-work/professor-rig/idle/ue}
export PROF_FBX=$PROF/Professor.fbx PROF_SPLAT=$PROF/professor.skinsplat
PORT=8900
CHROME=$(ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux64/chrome 2>/dev/null | sort -V | tail -1)

# environment: hand-installed emsdk wins, no stale EMSDK, nothing from AppImages / Vulkan SDK
unset LD_LIBRARY_PATH VULKAN_SDK VK_LAYER_PATH EMSDK EMSDK_NODE EM_CONFIG
export WASM_EMSDK_ROOT=$HOME/Programs/emsdk
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 DOTNET_CLI_TELEMETRY_OPTOUT=1 NuGetAudit=false

say()  { printf '\033[1;36m[%s]\033[0m %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$LOGS/run.log"; }
die()  { printf '\033[1;31m[%s] FEHLER:\033[0m %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$LOGS/run.log"; echo "FAILED: $*" > "$LOGS/STATUS"; exit 1; }
done_() { touch "$LOGS/.done-$1"; }
need()  { [ ! -f "$LOGS/.done-$1" ]; }

STEPS="install qfe project build scene web serve"
if [ "${1:-}" = "--from" ]; then
  hit=0; for s in $STEPS; do [ "$s" = "$2" ] && hit=1; [ $hit = 1 ] && rm -f "$LOGS/.done-$s"; done
  [ $hit = 1 ] || die "unbekannter Schritt $2 (gültig: $STEPS)"
fi
echo "RUNNING" > "$LOGS/STATUS"

[ -f "$PROF_FBX" ] && [ -f "$PROF_SPLAT" ] || die "Professor-Daten fehlen in $PROF"
[ -f "$QFE" ] || die "QFE-Zip fehlt: $QFE"
[ -f "$WASM_EMSDK_ROOT/upstream/emscripten/emscripten-version.txt" ] || die "emsdk fehlt in $WASM_EMSDK_ROOT"

# ---- 1. engine zip -> $ENGINE ------------------------------------------------------------------
if need install; then
  if [ -f "$ENGINE/Engine/Binaries/Linux/UnrealEditor" ]; then say "Engine schon entpackt: $ENGINE"; else
    say "warte auf die Engine-Zip in ~/Downloads (Linux_Unreal_Engine_5.9*.zip) ..."
    while :; do
      ZIP=$(ls -t "$HOME"/Downloads/*nreal*ngine*5.9*.zip "$HOME"/Downloads/*Linux*Unreal*.zip 2>/dev/null | grep -v UE59-QFE | head -1)
      if [ -n "$ZIP" ]; then
        s1=$(stat -c %s "$ZIP"); sleep 15; s2=$(stat -c %s "$ZIP")
        [ "$s1" = "$s2" ] && unzip -l "$ZIP" > /dev/null 2>&1 && break
      else sleep 20; fi
    done
    say "gefunden: $ZIP ($(du -h "$ZIP" | cut -f1)), entpacke nach $ENGINE"
    TMP=$ENGINE.unpacking; rm -rf "$TMP"; mkdir -p "$TMP"
    unzip -q "$ZIP" -d "$TMP" > "$LOGS/unzip.log" 2>&1 || die "unzip fehlgeschlagen, siehe $LOGS/unzip.log"
    ROOT=$(dirname "$(dirname "$(dirname "$(find "$TMP" -maxdepth 5 -path '*/Engine/Build/Build.version' | head -1)")")")
    [ -f "$ROOT/Engine/Build/Build.version" ] || die "keine Engine/ in der Zip gefunden"
    grep -q '"MinorVersion": 9' "$ROOT/Engine/Build/Build.version" || die "Zip ist nicht UE 5.9: $(cat "$ROOT/Engine/Build/Build.version")"
    mv "$ROOT" "$ENGINE" && rm -rf "$TMP"
  fi
  done_ install
fi
PY=$ENGINE/Engine/Binaries/ThirdParty/Python3/Linux/bin/python3
[ -x "$PY" ] || PY=python3

# ---- 2. Epic QFE (Wasm files for Linux installed builds) ---------------------------------------
if need qfe; then
  say "spiele QFE ein"
  unzip -o -q "$QFE" -d "$ENGINE" > "$LOGS/qfe.log" 2>&1 || die "QFE-unzip fehlgeschlagen, siehe $LOGS/qfe.log"
  [ -d "$ENGINE/Engine/Platforms/Wasm/Build/Tools/web" ] || die "nach dem QFE fehlen die Wasm-Tools"
  done_ qfe
fi

# ---- 3. C++ project from TP_ThirdPerson -------------------------------------------------------
if need project; then
  say "lege Projekt an: $PROJDIR"
  "$PY" -I "$ENGINE/Engine/Platforms/Wasm/Skills/web-deploy/scripts/new_template_project.py" \
      --template TP_ThirdPerson --out "$PROJDIR" --web-ready --force \
      --engine-root "$ENGINE" > "$LOGS/project.log" 2>&1 || die "Projekt-Skript fehlgeschlagen, siehe $LOGS/project.log"
  [ -f "$UPROJECT" ] || die "$UPROJECT wurde nicht angelegt"
  python3 -I "$TOOLS/patch_project.py" "$UPROJECT" | tee -a "$LOGS/run.log" || die "uproject-Patch fehlgeschlagen"
  done_ project
fi

# ---- 4. editor build (project modules only on an installed engine) -----------------------------
if need build; then
  [ -d "$TOOLS/SkinnedSplat" ] && mkdir -p "$PROJDIR/Plugins" && rsync -a --delete "$TOOLS/SkinnedSplat/" "$PROJDIR/Plugins/SkinnedSplat/"
  say "baue ${NAME}Editor (Linux Development)"
  "$ENGINE/Engine/Build/BatchFiles/Linux/Build.sh" "${NAME}Editor" Linux Development -Project="$UPROJECT" \
      > "$LOGS/build-editor.log" 2>&1 || { grep -E " error|Error:" "$LOGS/build-editor.log" | head -20; die "Editor-Build fehlgeschlagen, siehe $LOGS/build-editor.log"; }
  done_ build
fi

# ---- 5. import splat + make the map (editor runs the Python script, then quits) ----------------
if need scene; then
  say "importiere den Professor und baue die Map (Editor startet, erster Start kompiliert Shader)"
  rm -f "$LOGS/scene.status"
  export SPLAT_STATUS=$LOGS/scene.status
  timeout 5400 "$ENGINE/Engine/Binaries/Linux/UnrealEditor" "$UPROJECT" \
      -ExecutePythonScript="$TOOLS/setup_professor.py" -unattended -nosplash -NoSound -stdout -FullStdOutLogOutput \
      > "$LOGS/scene.log" 2>&1
  grep -q '^OK' "$LOGS/scene.status" 2>/dev/null || { tail -5 "$LOGS/scene.status" 2>/dev/null; grep -E "Error|Traceback" "$LOGS/scene.log" | tail -15; die "Szenen-Setup fehlgeschlagen, siehe $LOGS/scene.log"; }
  cat "$LOGS/scene.status" | tee -a "$LOGS/run.log"
  done_ scene
fi

# ---- 6. web build: build + cook + stage + package ----------------------------------------------
OUT=$PROJDIR/WebBuild
if need web; then
  say "baue Web-Version (Wasm, Development) -> $OUT  (Link braucht ein paar Minuten und viel RAM)"
  "$ENGINE/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun -project="$UPROJECT" -platform=Wasm \
      -clientconfig=Development -build -cook -stage -pak -iostore -package -archive -archivedirectory="$OUT" \
      -map=/Game/Splat/SplatMap -unattended -utf8output \
      -AdditionalCookerOptions="-unattended -nopause -nosplash -nullrhi" > "$LOGS/web.log" 2>&1 \
      || { grep -E "Error|error:|FAILED" "$LOGS/web.log" | grep -v "0 error" | tail -25; die "Web-Build fehlgeschlagen, siehe $LOGS/web.log"; }
  [ -f "$OUT/Wasm/index.html" ] || die "kein $OUT/Wasm/index.html nach dem Build"
  "$PY" -I "$ENGINE/Engine/Platforms/Wasm/Build/Tools/deploy/check_stage.py" "$OUT/Wasm" > "$LOGS/check_stage.log" 2>&1 \
      && say "check_stage: ok" || say "check_stage meldet etwas (für lokal unkritisch), siehe $LOGS/check_stage.log"
  done_ web
fi

# ---- 7. serve + browser -------------------------------------------------------------------------
say "starte Server auf http://127.0.0.1:$PORT"
pkill -f "serve.py --root $OUT/Wasm" 2>/dev/null
TS_HOST=$(tailscale status --json 2>/dev/null | python3 -c "import json,sys;print(json.load(sys.stdin)['Self']['DNSName'].rstrip('.'))" 2>/dev/null)
ALLOW=(); [ -n "$TS_HOST" ] && ALLOW=(--allow-host "$TS_HOST")  # serve.py answers 403 to unknown Host names
setsid nohup "$PY" -I "$ENGINE/Engine/Platforms/Wasm/Build/Tools/web/serve.py" --root "$OUT/Wasm" --port $PORT "${ALLOW[@]}" \
    > "$LOGS/serve.log" 2>&1 < /dev/null &
sleep 3
curl -sf -o /dev/null "http://127.0.0.1:$PORT/index.html" || die "Server antwortet nicht, siehe $LOGS/serve.log"
done_ serve

# ---- 8. Tailscale: HTTPS in the tailnet (port 10000; 443/8443 are already used by other services) ----
TS_PORT=${TS_PORT:-10000}
TS_URL=""
if command -v tailscale > /dev/null; then
  TS_HOST=$(tailscale status --json 2>/dev/null | python3 -c "import json,sys;print(json.load(sys.stdin)['Self']['DNSName'].rstrip('.'))" 2>/dev/null)
  if [ -n "$TS_HOST" ] && tailscale serve --bg --https=$TS_PORT http://127.0.0.1:$PORT > "$LOGS/tailscale.log" 2>&1; then
    TS_URL="https://$TS_HOST:$TS_PORT/index.html"
    sleep 2
    curl -sf -o /dev/null --max-time 20 "$TS_URL" && say "Tailscale: $TS_URL" \
      || say "Tailscale serve gesetzt, aber $TS_URL antwortet (noch) nicht, siehe $LOGS/tailscale.log"
  else
    say "Tailscale serve fehlgeschlagen, siehe $LOGS/tailscale.log"
  fi
fi
echo "OK http://127.0.0.1:$PORT/index.html ${TS_URL}" > "$LOGS/STATUS"
say "fertig: http://127.0.0.1:$PORT/index.html ${TS_URL:+ / $TS_URL}"
[ "${NO_BROWSER:-0}" = 1 ] || "$TOOLS/open_browser.sh"
