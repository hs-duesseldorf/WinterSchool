#!/bin/bash
# Reimports the professor (Professor.fbx + professor.skinsplat from PROF) and rebuilds SplatMap in the editor.
set -euo pipefail
source "$(dirname "$0")/config.sh"
PROF=${PROF:-$HOME/Programs/WinterSchool-work/professor-rig/idle/ue}
export PROF_FBX=$PROF/Professor.fbx PROF_SPLAT=$PROF/professor.skinsplat SPLAT_STATUS=${SPLAT_STATUS:-/tmp/winterschool_import.status}
"$ENGINE/Engine/Binaries/Linux/UnrealEditor" "$UPROJECT" -ExecutePythonScript="$ROOT/Scripts/setup_professor.py" \
    -unattended -nosplash -NoSound -stdout
cat "$SPLAT_STATUS"
