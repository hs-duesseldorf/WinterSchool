#!/usr/bin/env bash
# Retargets recorded motion onto the professor (Scripts/retarget_anims.py), headless:
#   Scripts/retarget_anims.sh Saved/Mocap/DepotOSC/*.zip Saved/Mocap/Depot/Dab.glb   # MoCap Depot takes (Captury)
#   Scripts/retarget_anims.sh /Game/Characters/Mannequins/Anims/Unarmed                # anims already in the project
# Captury takes are the Depot's OSC recordings (<Take>_OSC.zip / .osc), made into .glb on the Captury skeleton by
# captury_osc_to_glb.py (Saved/Mocap/Converted), or a .glb on that skeleton (Dab.glb). They are imported as animation
# only onto /Game/Mocap/Captury/SKEL_Captury (Anims/<name>), retargeted with /Game/Professor/RTG_<source>_To_Professor
# (the live setup's retargeter) into /Game/Professor/Anims and added to the professor. Close the editor first.
set -euo pipefail
source "$(dirname "$0")/config.sh"
[ $# -gt 0 ] || { sed -n '2,8p' "$0"; exit 1; }
if editor_open && [ -z "${FORCE:-}" ]; then
    echo "The editor has $(basename "$UPROJECT") open. Save and close it first (or FORCE=1)." >&2; exit 1
fi
RIG=${CAPTURY_RIG_FILE:-$ROOT/Saved/Mocap/Depot/Dab.glb}
inputs=()
for i in "$@"; do
    if [[ $i == /Game/* ]]; then inputs+=("$i")
    elif [[ ${i,,} == *.osc || ${i,,} == *osc.zip ]]; then
        out=$("$(import_python)" "$ROOT/Scripts/captury_osc_to_glb.py" "$RIG" "$ROOT/Saved/Mocap/Converted" "$i")
        echo "$out"; inputs+=("$(sed -n 's/.* -> \(.*\.glb\): .*/\1/p' <<< "$out")")
    else inputs+=("$(realpath "$i")"); fi
done
export RETARGET_INPUTS=$(IFS=,; echo "${inputs[*]}") RETARGET_STATUS=$(mktemp)
mkdir -p "$ROOT/Scripts/logs"
"$ENGINE/Engine/Binaries/Linux/UnrealEditor" "$UPROJECT" -ExecutePythonScript="$ROOT/Scripts/retarget_anims.py" \
    -unattended -nosplash -NoSound -stdout > "$ROOT/Scripts/logs/retarget_anims.log" 2>&1 || true
cat "$RETARGET_STATUS"; grep -q '^OK' "$RETARGET_STATUS"
