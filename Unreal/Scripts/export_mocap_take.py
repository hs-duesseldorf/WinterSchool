# Runs inside the editor: samples animation sequences into "takes" for Scripts/captury_replay.py, which streams them
# to Unreal through the Captury protocol like Captury Live / Captury Replay would (the Simulate use case).
#
#   TAKE_ANIMS  comma separated anim sequence paths (/Game/Mocap/Dab/Dab_Anim, ...). A folder path takes every
#               anim sequence below it.
#   TAKE_OUT    output folder (default <project>/Saved/Mocap/Takes), one <anim name>.json per sequence
#   TAKE_ROOT   bone that becomes Captury's root joint (default: Hips, else pelvis, else the first bone). Captury streams
#               only the root's translation, so this has to be the bone that carries the motion.
#
# A take holds the skeleton's reference pose and every frame's bone transforms in component space, relative to the
# root joint's parent at its reference pose. Unreal's own sampling is used, so the streamed pose reproduces exactly
# what the sequence plays on its skeleton; the Captury conversion happens in captury_replay.py.
import json, os, traceback, unreal

STATUS = os.environ.get("TAKE_STATUS", "/tmp/export_mocap_take.status")
lines = []
def log(m): unreal.log("[MocapTake] " + m); lines.append(m)

def anims_from(paths):
    out = []
    for p in paths:
        if unreal.EditorAssetLibrary.does_directory_exist(p):
            for a in unreal.EditorAssetLibrary.list_assets(p, recursive=True):
                o = unreal.load_asset(a)
                if isinstance(o, unreal.AnimSequence): out.append(o)
        else:
            o = unreal.load_asset(p)
            if not isinstance(o, unreal.AnimSequence): raise RuntimeError(f"{p} is not an anim sequence")
            out.append(o)
    return out

def mesh_for(skel):
    """A skeletal mesh on this skeleton (only used for the bone hierarchy)."""
    reg = unreal.AssetRegistryHelpers.get_asset_registry()
    for d in reg.get_assets_by_class(unreal.TopLevelAssetPath("/Script/Engine", "SkeletalMesh")):
        m = d.get_asset()
        if m and m.get_editor_property("skeleton") == skel: return m
    return None

def tr(t):
    l, q = t.translation, t.rotation
    return [round(v, 5) for v in (l.x, l.y, l.z, q.x, q.y, q.z, q.w)]

def export(anim, out_dir, root_pref):
    ext = unreal.AnimPoseExtensions
    space = unreal.AnimPoseSpaces.WORLD  # component space
    skel = anim.get_skeleton()
    mesh = mesh_for(skel)
    if not mesh: raise RuntimeError(f"{anim.get_name()}: no skeletal mesh uses {skel.get_path_name()}")
    comp = unreal.new_object(unreal.SkeletalMeshComponent)
    comp.set_skeletal_mesh_asset(mesh)
    opts = unreal.AnimPoseEvaluationOptions()
    ref_pose = ext.get_reference_pose(skel)
    names = [str(n) for n in ext.get_bone_names(ref_pose)]
    parent = {n: str(comp.get_parent_bone(n)) for n in names}
    lower = {n.lower(): n for n in names}
    root = root_pref or lower.get("hips") or lower.get("pelvis") or names[0]
    if root not in parent: raise RuntimeError(f"{anim.get_name()}: bone {root} not found")
    # the root's subtree in hierarchy order (parents before children)
    keep, order = {root}, [root]
    for n in names:
        p = parent[n]
        if n != root and p in keep: keep.add(n); order.append(n)
    base_name = parent[root] if parent[root] in parent else None
    base = ext.get_ref_bone_pose(ref_pose, base_name, space) if base_name else unreal.Transform()
    inv = base.inverse()
    rel = lambda t: unreal.MathLibrary.compose_transforms(t, inv)  # base^-1 * t in Unreal's order
    ref = [tr(rel(ext.get_ref_bone_pose(ref_pose, n, space))) for n in order]
    num = unreal.AnimationLibrary.get_num_keys(anim)  # sampled poses, first and last frame included
    try: rate = anim.get_editor_property("target_frame_rate"); rate = (rate.numerator, rate.denominator)
    except Exception:  # no frame rate property: frame intervals over length (whole fps when it is one)
        f = (num - 1) / max(anim.get_play_length(), 1e-6)
        rate = (round(f), 1) if abs(f - round(f)) < 1e-3 else (round(f * 1000), 1000)
    fps = rate[0] / max(rate[1], 1)
    frames = []
    for f in range(num):
        pose = ext.get_anim_pose_at_frame(anim, f, opts)
        row = []
        for n in order: row += tr(rel(ext.get_bone_pose(pose, n, space)))
        frames.append(row)
    take = {"name": anim.get_name(), "source": anim.get_path_name(), "fps": list(rate),
            "actors": [{"name": anim.get_name(), "bones": order,
                        "parents": [order.index(parent[n]) if n != root else -1 for n in order],
                        "ref": ref, "frames": frames}]}
    path = os.path.join(out_dir, anim.get_name() + ".json")
    with open(path, "w") as fp: json.dump(take, fp, separators=(",", ":"))
    log(f"{anim.get_path_name()}: {len(order)} bones from {root}, {num} frames at {fps:g} fps -> {path}")

ok = False
try:
    out_dir = os.environ.get("TAKE_OUT") or os.path.join(unreal.Paths.project_saved_dir(), "Mocap", "Takes")
    out_dir = os.path.abspath(out_dir); os.makedirs(out_dir, exist_ok=True)
    anims = anims_from([p.strip() for p in os.environ["TAKE_ANIMS"].split(",") if p.strip()])
    if not anims: raise RuntimeError("no anim sequences found")
    for a in anims: export(a, out_dir, os.environ.get("TAKE_ROOT"))
    ok = True
except Exception:
    lines.append(traceback.format_exc()); unreal.log_error(traceback.format_exc())
finally:
    with open(STATUS, "w") as f: f.write(("OK\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
    if not os.environ.get("TAKE_KEEP_OPEN"): unreal.SystemLibrary.quit_editor()
