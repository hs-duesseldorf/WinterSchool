#!/bin/bash
# Matches TempleMap's sun, shadows and sky to the capture and sets the map up for web performance:
# fits the capture's lighting (Scripts/fit_capture_lighting.py -> Saved/Import/Temple/lighting.json), then applies it
# headless in the editor (Scripts/setup_lighting.py). Close the editor first. Then rebuild the web version.
set -euo pipefail
source "$(dirname "$0")/config.sh"
OUT=${OUT:-$ROOT/Saved/Import/Temple}
export LIGHTING_JSON=$OUT/lighting.json SPLAT_STATUS=${SPLAT_STATUS:-/tmp/winterschool_lighting.status}
if editor_open && [ -z "${FORCE:-}" ]; then
    echo "The editor has $(basename "$UPROJECT") open. Save and close it first (or FORCE=1)." >&2; exit 1
fi
[ -f "$LIGHTING_JSON" ] || "$(import_python)" -I "$ROOT/Scripts/fit_capture_lighting.py" "$OUT/SakaShrine.ply" --out "$LIGHTING_JSON"
"$ENGINE/Engine/Binaries/Linux/UnrealEditor" "$UPROJECT" -ExecutePythonScript="$ROOT/Scripts/setup_lighting.py" \
    -unattended -nosplash -NoSound -stdout > "$ROOT/Saved/Logs/setup_lighting.log" 2>&1 || true
cat "$SPLAT_STATUS"
