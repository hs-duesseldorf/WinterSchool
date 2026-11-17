#!/bin/bash
# Builds the editor modules, then cooks and packages the web version into WebBuild/Wasm.
set -euo pipefail
source "$(dirname "$0")/config.sh"
unset LD_LIBRARY_PATH VULKAN_SDK VK_LAYER_PATH EMSDK EMSDK_NODE EM_CONFIG
export DOTNET_CLI_TELEMETRY_OPTOUT=1

"$ENGINE/Engine/Build/BatchFiles/Linux/Build.sh" WinterSchoolEditor Linux Development -Project="$UPROJECT"
"$ENGINE/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun -project="$UPROJECT" -platform=Wasm \
    -clientconfig="$CONFIG" -build -cook -stage -pak -iostore -package -archive -archivedirectory="$WEBBUILD" \
    -map="$MAP" -unattended -utf8output -AdditionalCookerOptions="-unattended -nopause -nosplash -nullrhi"
echo "Web build: $WEBBUILD/Wasm  (run Scripts/play_web.sh)"
