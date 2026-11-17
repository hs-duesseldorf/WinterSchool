# Shared settings for the scripts in this folder. Override any of them with an environment variable.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UPROJECT=$ROOT/WinterSchool.uproject
ENGINE=${ENGINE:-$HOME/Programs/UnrealEngine/5.9.0-preview-1-full}   # UE 5.9 with the Wasm platform
export WASM_EMSDK_ROOT=${WASM_EMSDK_ROOT:-$HOME/Programs/emsdk}       # emsdk for the Wasm build
WEBBUILD=$ROOT/WebBuild                                               # packaged web build goes here
MAP=${MAP:-/Game/Splat/SplatMap}
CONFIG=${CONFIG:-Development}                                         # Development or Shipping
PORT=${PORT:-8900}
PY=$ENGINE/Engine/Binaries/ThirdParty/Python3/Linux/bin/python3
[ -x "$PY" ] || PY=python3
