#!/usr/bin/env bash
# Samples anim sequences into takes for captury_replay.py (runs a headless editor; the open editor can stay open).
#   Scripts/export_mocap_take.sh /Game/Mocap/Dab [/Game/Characters/Mannequins/Anims/Unarmed/MM_Attack_01 ...]
# Takes land in Saved/Mocap/Takes/<anim name>.json. TAKE_ROOT=<bone> picks Captury's root joint (default Hips/pelvis).
set -euo pipefail
source "$(dirname "$0")/config.sh"
[ $# -gt 0 ] || { sed -n 2,4p "$0"; exit 1; }
export TAKE_ANIMS=$(IFS=,; echo "$*") TAKE_STATUS=$(mktemp)
mkdir -p "$ROOT/Scripts/logs"
"$ENGINE/Engine/Binaries/Linux/UnrealEditor" "$UPROJECT" -ExecutePythonScript="$ROOT/Scripts/export_mocap_take.py" \
    -unattended -nosplash -nullrhi -NoSound -stdout > "$ROOT/Scripts/logs/export_mocap_take.log" 2>&1 || true
cat "$TAKE_STATUS"; grep -q '^OK' "$TAKE_STATUS"
