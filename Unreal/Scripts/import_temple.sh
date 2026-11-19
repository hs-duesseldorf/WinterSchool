#!/bin/bash
# The world: "Saka Shrine" (Japan) by kotohibi, https://superspl.at/scene/3f8c617d, CC BY 4.0 (credit the author).
# Downloads its full-detail SOG chunks, merges them to one .ply (PlayCanvas splat-transform), levels it (the capture is
# tilted ~2.4 deg) with the plaza at height 0, crops far floaters, builds a collision mesh and imports both headless
# into /Game/Temple + /Game/Temple/TempleMap (setup_temple.py). Close the editor first.
# Collision: splat-transform --collision-mesh (voxelises the splats, keeps the cluster connected to the plaza, fills
# floors), then collision_from_splat_transform.py (axes for Unreal, ~300k tris).
# Needs node/npx and python3 (a venv with the meshing packages is made in Saved/Import/venv, see config.sh).
set -euo pipefail
source "$(dirname "$0")/config.sh"
OUT=${OUT:-$ROOT/Saved/Import/Temple}
SRC=https://d28zzqy0iyovbz.cloudfront.net/3f8c617d/v1
ST="npx -y @playcanvas/splat-transform@3.11.2"
export SPLAT_STATUS=${SPLAT_STATUS:-/tmp/winterschool_temple.status} TEMPLE_PLY=$OUT/SakaShrine.ply TEMPLE_COLLISION=$OUT/SakaShrine_Collision.glb
if editor_open && [ -z "${FORCE:-}" ]; then
    echo "The editor has $(basename "$UPROJECT") open. Save and close it first (or FORCE=1)." >&2; exit 1
fi
mkdir -p "$OUT/sog"
if [ ! -f "$OUT/sog/done" ]; then
    curl -sfL "$SRC/lod-meta.json" -o "$OUT/sog/lod-meta.json"
    for c in 0_0 0_1 0_2 0_3 0_4 0_5; do   # LOD 0 = all 3M splats
        mkdir -p "$OUT/sog/$c"
        for f in meta.json means_l.webp means_u.webp quats.webp scales.webp sh0.webp shN_centroids.webp shN_labels.webp; do
            curl -sfL "$SRC/$c/$f" -o "$OUT/sog/$c/$f"
        done
    done
    touch "$OUT/sog/done"
fi
# actions after the output apply to the merged set (after an input they only touch that input). splat-transform works
# in its engine frame = the PLY turned 180 deg about Z (x, y negated): -t 0,4.64,0 moves the plaza from PLY y=4.64 to 0,
# the box keeps 40 m above and 25 m below it (PLY y is down). The capture has no real scale: -s 0.4 makes it life size
# (measured: stair risers 0.40 -> 0.16 m, torii 11.6 -> 4.6 m tall, stone lanterns 8.4-9.5 -> 3.4-3.8 m and 5.7 -> 2.3 m)
$ST -q -w "$OUT"/sog/0_{0,1,2,3,4,5}/meta.json "$TEMPLE_PLY" -r -2.07,0,-1.23 -t 0,4.64,0 -N -B -75,-25,-75,75,40,75 \
    -s ${TEMPLE_SCALE:-0.4}
IPY=$(import_python)
# --seed-pos is in splat-transform's frame (PLY with x, y negated): 1 m above the plaza. 5 cm voxels. Needs a big node heap.
mkdir -p "$OUT/st"
NODE_OPTIONS=--max-old-space-size=${NODE_HEAP_MB:-24000} $ST -q -w "$TEMPLE_PLY" --filter-cluster --seed-pos 0,1,0 \
    --voxel-size ${VOXEL_SIZE:-0.05} --voxel-floor-fill --collision-mesh "$OUT/st/SakaShrine.voxel.json"
"$IPY" "$ROOT/Scripts/collision_from_splat_transform.py" "$OUT/st/SakaShrine.collision.glb" "$TEMPLE_COLLISION" \
    --tris ${COLLISION_TRIS:-300000}
"$ENGINE/Engine/Binaries/Linux/UnrealEditor" "$UPROJECT" -ExecutePythonScript="$ROOT/Scripts/setup_temple.py" \
    -unattended -nosplash -NoSound -stdout
cat "$SPLAT_STATUS"
