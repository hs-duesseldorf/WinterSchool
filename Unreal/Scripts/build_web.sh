#!/bin/bash
# Builds the editor modules, then cooks and packages the web version into WebBuild/Wasm.
# The site carries two texture sets: BC for desktop GPUs and ASTC for phones/Apple GPUs (iPhone, iPad, Android).
# The page picks the set the GPU supports and downloads only that one. Two cooks, so it takes about twice as long;
#   TEXTURES=bc Scripts/build_web.sh    desktop-only build (BC textures, one cook, does not run on phones)
set -euo pipefail
source "$(dirname "$0")/config.sh"
unset LD_LIBRARY_PATH VULKAN_SDK VK_LAYER_PATH EMSDK EMSDK_NODE EM_CONFIG
export DOTNET_CLI_TELEMETRY_OPTOUT=1
TEXTURES=${TEXTURES:-both}
STAGE=$ROOT/Saved/WebStage                     # the two stages before they are merged

UAT=("$ENGINE/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun -project="$UPROJECT" -platform=Wasm -clientconfig="$CONFIG"
     -iostore -map="$MAP" -unattended -utf8output -AdditionalCookerOptions="-unattended -nopause -nosplash -nullrhi")
ARCHIVE=(-archive -archivedirectory="$WEBBUILD")

"$ENGINE/Engine/Build/BatchFiles/Linux/Build.sh" WinterSchoolEditor Linux Development -Project="$UPROJECT"
if [ "$TEXTURES" = bc ]; then
  "${UAT[@]}" -build -cook -stage -pak -package "${ARCHIVE[@]}"
else
  # Fresh cooks (no -cookincremental): the two stages must differ only in their texture containers
  "${UAT[@]}" -build -cook -stage -pak -package -stagingdirectory="$STAGE/Bc"
  "${UAT[@]}" -cookflavor=ASTC -skipbuild -cook -stage -pak -package -stagingdirectory="$STAGE/Astc"
  "${UAT[@]}" -skipbuild -skipcook -stage -pak -package -stagingdirectory="$STAGE/Bc" \
      -WasmAddTextureSet="$STAGE/Astc/Wasm_ASTC" "${ARCHIVE[@]}"
fi
echo "Web build: $WEBBUILD/Wasm  (run Scripts/play_web.sh)"
