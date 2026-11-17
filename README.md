# WinterSchool

Unreal Engine 5.9 project that renders a rigged, animated Gaussian splat of a person in the browser (Wasm/WebGPU).

- `Plugins/SkinnedSplat/` skins a Gaussian splat with a skeleton (dual quaternion skinning in a compute shader).
- `Content/Professor/` holds the professor's skeleton, idle animation and skinned splat.
- `Content/Splat/SplatMap` is the scene.
- `Scripts/` sets up the scene and builds the web version.

## Requirements

- Unreal Engine 5.9 with the GaussianSplat plugin and Wasm target
- emsdk (`WASM_EMSDK_ROOT`)

## Build for the web

```bash
ENGINE=/path/to/UnrealEngine ./Scripts/run.sh --from build
```

The build is written to `WebBuild/` and served on http://127.0.0.1:8900.
`./Scripts/run.sh --from scene` reimports the professor from `PROF` (`Professor.fbx` and `professor.skinsplat`).
