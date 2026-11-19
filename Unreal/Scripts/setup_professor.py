# Runs inside the editor: imports the professor (PROF_PLY, a skinned 3DGS .ply, plus the .glb next to it with skeleton
# and clips) through the skinned splat factory, creates an example splat material, rebuilds /Game/Splat/SplatMap from
# the ThirdPerson map with the professor in front of the PlayerStart, saves, quits. The default maps are left alone.
#
# /Game/Professor: Professor (splat data), SK_/SKEL_/PHYS_Professor (the driving skeletal mesh, its bone display),
# M_Bone, Anims/<clip>; IK_Professor + RTG_<source>_To_Professor come from Scripts/setup_mocap.sh / retarget_anims.sh.
import os, re, traceback, unreal

PLY = os.environ["PROF_PLY"]
STATUS = os.environ.get("SPLAT_STATUS", "/tmp/splat_scene.status")
MAP = "/Game/Splat/SplatMap"; SRC_MAP = "/Game/ThirdPerson/Lvl_ThirdPerson"; DST = "/Game/Professor"
MAT = "/Game/Splat/Materials/M_SplatTint"
lines = []
def log(m): unreal.log("[SplatSetup] " + m); lines.append(m)
eal = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()

def make_tint_material():
    """Unlit translucent: Emissive = VertexColor (the splat color) * Tint, Opacity = VertexColor.a * TintOpacity."""
    if eal.does_asset_exist(MAT): eal.delete_asset(MAT)
    path, name = MAT.rsplit("/", 1)
    m = tools.create_asset(name, path, unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    try: m.set_editor_property("used_with_lidar_point_cloud", True)
    except Exception as e: log(f"usage flag not set from python ({e}); the actor sets it")
    mel = unreal.MaterialEditingLibrary
    vc = mel.create_material_expression(m, unreal.MaterialExpressionVertexColor, -600, 0)
    tint = mel.create_material_expression(m, unreal.MaterialExpressionVectorParameter, -600, 200)
    tint.set_editor_property("parameter_name", "Tint"); tint.set_editor_property("default_value", unreal.LinearColor(1, 1, 1, 1))
    op = mel.create_material_expression(m, unreal.MaterialExpressionScalarParameter, -600, 400)
    op.set_editor_property("parameter_name", "TintOpacity"); op.set_editor_property("default_value", 1.0)
    mul = mel.create_material_expression(m, unreal.MaterialExpressionMultiply, -300, 0)
    mel.connect_material_expressions(vc, "", mul, "A"); mel.connect_material_expressions(tint, "", mul, "B")
    mul_a = mel.create_material_expression(m, unreal.MaterialExpressionMultiply, -300, 300)
    mel.connect_material_expressions(vc, "A", mul_a, "A"); mel.connect_material_expressions(op, "", mul_a, "B")
    mel.connect_material_property(mul, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.connect_material_property(mul_a, "", unreal.MaterialProperty.MP_OPACITY)
    mel.recompile_material(m); eal.save_loaded_asset(m, False)
    log(f"material {MAT}")
    return m

ok = False
try:
    # the old FBX + .skinsplat professor and its map are replaced completely
    for p in (DST, "/Game/ZZTest"):
        if eal.does_directory_exist(p): eal.delete_directory(p); log(f"deleted {p}")

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", PLY); task.set_editor_property("destination_path", DST)
    task.set_editor_property("destination_name", "Professor")
    task.set_editor_property("automated", True); task.set_editor_property("replace_existing", True); task.set_editor_property("save", True)
    tools.import_asset_tasks([task])
    data = next((o for o in map(unreal.load_asset, task.get_editor_property("imported_object_paths"))
                 if isinstance(o, unreal.SkinnedSplatData)), None)
    if not data: raise RuntimeError(f"{PLY} was not imported as SkinnedSplatData")
    if not data.get_editor_property("mesh") and not unreal.SkinnedSplatEditorLibrary.import_rig(data):
        raise RuntimeError("rig import failed")
    eal.save_directory(DST, only_if_is_dirty=False, recursive=True)  # the rig import leaves its packages unsaved
    if eal.does_directory_exist(f"{DST}/RigImport"): eal.delete_directory(f"{DST}/RigImport")  # its staging folder
    anims = data.get_editor_property("animations")
    log(f"splat {data.get_path_name()}: {data.get_editor_property('num_splats')} splats, mesh "
        f"{data.get_editor_property('mesh').get_path_name()}, {len(anims)} clips, default "
        f"{data.get_editor_property('default_animation').get_name()}")
    mat = make_tint_material()

    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not les.load_level(SRC_MAP): raise RuntimeError("loading the ThirdPerson map failed")
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    ps = [a for a in eas.get_all_level_actors() if isinstance(a, unreal.PlayerStart)]
    if ps: loc = ps[0].get_actor_location(); fwd = ps[0].get_actor_forward_vector(); yaw = ps[0].get_actor_rotation().yaw
    else: loc, fwd, yaw = unreal.Vector(0, 0, 100), unreal.Vector(1, 0, 0), 0.0
    # 4 m in front of the player on the floor (PlayerStart sits ~ capsule half height above it). The splat looks along
    # +X with its feet at z = 0, so yaw + 180 turns it towards the player.
    pos = unreal.Vector(loc.x + fwd.x * 400, loc.y + fwd.y * 400, loc.z - 90)
    act = eas.spawn_actor_from_class(unreal.SkinnedSplatActor, pos, unreal.Rotator(roll=0, pitch=0, yaw=yaw + 180))
    if not act: raise RuntimeError("spawning SkinnedSplatActor failed")
    act.set_actor_label("Professor")
    act.set_editor_property("splat_data", data)
    log(f"Professor at {pos}, yaw {yaw + 180:.0f}, PlayerStart {loc}; example material {MAT} (not assigned)")
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP): raise RuntimeError("saving the map failed")
    log(f"map saved: {MAP}"); ok = True  # the default maps stay on the temple (setup_temple.py)
except Exception:
    lines.append(traceback.format_exc()); unreal.log_error(traceback.format_exc())
finally:
    with open(STATUS, "w") as f: f.write(("OK\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
    if not os.environ.get("SPLAT_KEEP_OPEN"): unreal.SystemLibrary.quit_editor()
