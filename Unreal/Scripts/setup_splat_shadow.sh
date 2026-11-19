#!/bin/bash
# Creates the splat shadow light function (GaussianSplat plugin content) and puts it on the sun of SHADOW_MAPS
# (comma separated, default TempleMap), headless (Scripts/setup_splat_shadow.py). Close the editor first.
set -euo pipefail
source "$(dirname "$0")/config.sh"
export SPLAT_STATUS=${SPLAT_STATUS:-/tmp/winterschool_splat_shadow.status}
if editor_open && [ -z "${FORCE:-}" ]; then
    echo "The editor has $(basename "$UPROJECT") open. Save and close it first (or FORCE=1)." >&2; exit 1
fi
"$ENGINE/Engine/Binaries/Linux/UnrealEditor" "$UPROJECT" -ExecutePythonScript="$ROOT/Scripts/setup_splat_shadow.py" \
    -unattended -nosplash -NoSound -stdout > "$ROOT/Saved/Logs/setup_splat_shadow.log" 2>&1 || true
cat "$SPLAT_STATUS"
