# Shared settings for the scripts in this folder. Override any of them with an environment variable.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UPROJECT=$ROOT/WinterSchool.uproject
ENGINE=${ENGINE:-$HOME/Programs/UnrealEngine/5.9}   # UE 5.9 with the Wasm platform
export WASM_EMSDK_ROOT=${WASM_EMSDK_ROOT:-$HOME/Programs/emsdk}       # emsdk for the Wasm build
WEBBUILD=$ROOT/WebBuild                                               # packaged web build goes here
MAP=${MAP:-/Game/Temple/TempleMap}
CONFIG=${CONFIG:-Development}                                         # Development or Shipping
PORT=${PORT:-8900}
PY=$ENGINE/Engine/Binaries/ThirdParty/Python3/Linux/bin/python3
[ -x "$PY" ] || PY=python3

# Python with the meshing packages the import scripts need (made on first use)
VENV=$ROOT/Saved/Import/venv
import_python() {
    "$VENV/bin/python" -c "import numpy, scipy, trimesh, fast_simplification, skimage" 2>/dev/null || {
        [ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
        "$VENV/bin/pip" install -q numpy scipy trimesh fast-simplification scikit-image >&2; }
    echo "$VENV/bin/python"
}

# true when an Unreal Editor has this project open (only real UnrealEditor processes: "pgrep -f" also matches a shell
# whose command line merely mentions the editor and the project)
editor_open() {
    local p
    for p in $(pgrep -x UnrealEditor); do
        tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q "$(basename "$UPROJECT")" && return 0
    done
    return 1
}
