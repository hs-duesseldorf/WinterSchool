#!/bin/bash
# Replaces the professor: exports the rig (Professor_Rig.npz + Animationen/K_*.npy) to a skinned 3DGS .ply + .glb,
# then imports both headless and rebuilds SplatMap. Close the editor first (it would hold the old packages).
# /Game/Professor is made again: run Scripts/setup_mocap.sh REBUILD_RIGS=1 afterwards for the live setup.
set -euo pipefail
source "$(dirname "$0")/config.sh"
PROF=${PROF:-$HOME/Documents/Medieninformatik/WinterSchool/Professor/3_Rig}
OUT=${OUT:-$ROOT/Saved/Import/Professor}
export SPLAT_STATUS=${SPLAT_STATUS:-/tmp/winterschool_import.status} PROF_PLY=$OUT.ply
if editor_open && [ -z "${FORCE:-}" ]; then
    echo "The editor has $(basename "$UPROJECT") open. Save and close it first (or FORCE=1)." >&2; exit 1
fi
mkdir -p "$(dirname "$OUT")"
"$(import_python)" "$ROOT/Scripts/export_skinsplat.py" "$PROF/Professor_Rig.npz" "$OUT" --clips "$PROF/Animationen"
# old professor (FBX + .skinsplat) and the map using it; both are in git
rm -rf "$ROOT/Content/Professor" "$ROOT/Content/Splat/SplatMap.umap"
"$ENGINE/Engine/Binaries/Linux/UnrealEditor" "$UPROJECT" -ExecutePythonScript="$ROOT/Scripts/setup_professor.py" \
    -unattended -nosplash -NoSound -stdout
cat "$SPLAT_STATUS"
