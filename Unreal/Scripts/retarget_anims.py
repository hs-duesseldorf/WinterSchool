# Runs inside the editor (Scripts/retarget_anims.sh): retargets recorded motion onto the professor.
#
#   RETARGET_INPUTS  comma separated: animation files (.fbx / .glb / .gltf, e.g. MoCap Depot downloads) or anim sequence
#                    asset paths / folders already in the project
#   RETARGET_SOURCE  name of the source skeleton (default Captury for files, else the skeleton of the given assets'
#                    first mesh); picks /Game/Professor/RTG_<name>_To_Professor (made on first use, retarget_common.py)
#
# Files are Captury recordings: imported as animation only onto the one Captury skeleton (/Game/Mocap/Captury/SK_Captury,
# made from CAPTURY_RIG_FILE, default Saved/Mocap/Depot/Dab.glb, on first use) into /Game/Mocap/Captury/Anims/<file
# name>. Every animation is then retargeted with the professor's retargeter (the same one the live setup uses) into /Game/Professor/Anims/<anim name>
# and added to the professor's clip list. Existing anims of the same name are replaced.
import os, sys, traceback, unreal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import retarget_common as rc

STATUS = os.environ.get("RETARGET_STATUS", "/tmp/retarget_anims.status")
INPUTS = [i.strip() for i in os.environ.get("RETARGET_INPUTS", "").split(",") if i.strip()]
ANIMS = f"{rc.PROFESSOR_DIR}/Anims"
lines = []
def log(m): unreal.log("[Retarget] " + m); lines.append(m)
eal = rc.eal


def clip_name(anim):
    """Name of the retargeted clip: the source anim's, without an importer "_Anim" suffix (Hallo_Anim -> Hallo)."""
    return anim.get_name().removesuffix("_Anim")


def mesh_of(anim):
    skel = anim.get_editor_property("skeleton")
    reg = unreal.AssetRegistryHelpers.get_asset_registry()
    for d in reg.get_assets_by_class(unreal.TopLevelAssetPath("/Script/Engine", "SkeletalMesh")):
        m = d.get_asset()
        if m and m.get_editor_property("skeleton") == skel: return m
    raise RuntimeError(f"{anim.get_path_name()}: no skeletal mesh on its skeleton")


def project_anims(path):
    if eal.does_directory_exist(path):
        return [o for o in map(unreal.load_asset, eal.list_assets(path, recursive=True)) if isinstance(o, unreal.AnimSequence)]
    o = unreal.load_asset(path)
    if not isinstance(o, unreal.AnimSequence): raise RuntimeError(f"{path} is not an anim sequence")
    return [o]


ok = False
try:
    if not INPUTS: raise RuntimeError("RETARGET_INPUTS is empty")
    batches = []  # (source mesh, anims)
    for i in INPUTS:
        if i.startswith("/Game/"):
            for a in project_anims(i): batches.append((mesh_of(a), [a]))
        else:
            if not os.path.isfile(i): raise RuntimeError(f"{i}: no such file")
            batches.append((rc.captury_rig(log), [rc.import_captury_anim(os.path.abspath(i), log)]))
    from_files = any(not i.startswith("/Game/") for i in INPUTS)
    src_name = os.environ.get("RETARGET_SOURCE") or ("Captury" if from_files else batches[0][0].get_name())
    rtg, data, prof_mesh = rc.professor_retargeter(src_name, batches[0][0], log)

    clips = [c for c in data.get_editor_property("animations") if c]
    for src_mesh, anims in batches:
        for a in anims:  # replace an older retarget of the same name
            old = f"{ANIMS}/{clip_name(a)}"
            if eal.does_asset_exist(old):
                o = unreal.load_asset(old); clips = [c for c in clips if c != o]; eal.delete_asset(old)
        inputs = unreal.IKRetargetBatchOperationInputs()
        inputs.set_editor_property("assets_to_retarget", [eal.find_asset_data(a.get_path_name()) for a in anims])
        for k, v in (("search", "_Anim"), ("replace", ""), ("source_mesh", src_mesh), ("target_mesh", prof_mesh), ("ik_retarget_asset", rtg), ("target_path", ANIMS),
                     ("include_referenced_assets", False), ("overwrite_existing_files", True)):
            inputs.set_editor_property(k, v)
        for r in unreal.IKRetargetBatchOperation.run_batch_retarget_with_results(inputs):
            src = r.get_editor_property("source_asset").asset_name
            if r.get_editor_property("status") == unreal.IKRetargetBatchAssetStatus.RETARGETED:
                new = r.get_editor_property("retargeted_asset").get_asset()
                clips.append(new); eal.save_loaded_asset(new, False)
                log(f"{src} -> {new.get_path_name()} ({new.get_editor_property('sequence_length'):.1f} s)")
            else:
                log(f"{src}: {r.get_editor_property('status')} {r.get_editor_property('reason')}")
    data.set_editor_property("animations", clips)
    eal.save_loaded_asset(data, False)
    log(f"{data.get_path_name()}: {len(clips)} clips (retargeter {rtg.get_path_name()})")
    ok = True
except Exception:
    lines.append(traceback.format_exc()); unreal.log_error(traceback.format_exc())
finally:
    with open(STATUS, "w") as f: f.write(("OK\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
    if not os.environ.get("RETARGET_KEEP_OPEN"): unreal.SystemLibrary.quit_editor()
