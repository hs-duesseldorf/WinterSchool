# WinterSchool

Winter School 2027: a generated, rigged Gaussian-splat professor, rendered in Unreal 5.9 on the web.

## Unreal/
UE 5.9 project (Wasm build). Plugins: `SkinnedSplat` (skinned splat rendering), `GaussianSplat`, `CapturyLiveLink` (mocap).
- `Scripts/build_web.sh` / `play_web.sh`: package and serve the web build. Paths in `Scripts/config.sh`.
- `Scripts/import_professor.sh`, `import_temple.sh`, `retarget_anims.sh`: import the professor rig, the Saka Shrine world, mocap.
- World: "Saka Shrine" by kotohibi, CC BY 4.0 (stored via Git LFS).

## Pipeline/
ComfyUI pipeline: prompt (+ optional photo) → image → views → 360° video → Gaussian splat.
- `./comfyui.sh` starts ComfyUI, workflow `Professor.json`. Headless: `python3 run.py --run NAME --prompt "..."`.
- `ProfSplat_Nodes/`: the custom nodes (link into ComfyUI's `custom_nodes`).
- Needs the install (ComfyUI + models, Python envs, LichtFeld, COLMAP) in `Pipeline/work/` or `$PROFSPLAT_ROOT`; not in the repo.
