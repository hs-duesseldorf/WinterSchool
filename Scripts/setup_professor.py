# Runs inside the editor: import Professor.fbx (skeleton + idle), load professor.skinsplat into a data asset,
# copy the ThirdPerson map to /Game/Splat/SplatMap, put the skinned splat in front of the PlayerStart, save, quit.
import os, re, traceback, unreal

FBX = os.environ["PROF_FBX"]; SPLAT = os.environ["PROF_SPLAT"]
STATUS = os.environ.get("SPLAT_STATUS", "/tmp/splat_scene.status")
MAP = "/Game/Splat/SplatMap"; SRC_MAP = "/Game/ThirdPerson/Lvl_ThirdPerson"; DST = "/Game/Professor"
lines = []
def log(m): unreal.log("[SplatSetup] " + m); lines.append(m)

def set_default_maps():
    ini = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_config_dir()), "DefaultEngine.ini")
    text = open(ini).read(); full = f"{MAP}.{MAP.rsplit('/', 1)[1]}"
    for key in ("GameDefaultMap", "EditorStartupMap"):
        if re.search(rf"^{key}=", text, re.M): text = re.sub(rf"^{key}=.*$", f"{key}={full}", text, flags=re.M)
        else: text = text.replace("[/Script/EngineSettings.GameMapsSettings]", f"[/Script/EngineSettings.GameMapsSettings]\n{key}={full}", 1)
    open(ini, "w").write(text)

ok = False
try:
    eal = unreal.EditorAssetLibrary
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", FBX); task.set_editor_property("destination_path", DST)
    task.set_editor_property("automated", True); task.set_editor_property("replace_existing", True); task.set_editor_property("save", True)
    try:
        ui = unreal.FbxImportUI()
        ui.set_editor_property("import_mesh", True); ui.set_editor_property("import_as_skeletal", True)
        ui.set_editor_property("import_animations", True); ui.set_editor_property("import_materials", False)
        ui.set_editor_property("import_textures", False); ui.set_editor_property("mesh_type_to_import", unreal.FBXImportType.FBXIT_SKELETAL_MESH)
        task.set_editor_property("options", ui)
    except Exception as e: log(f"FbxImportUI nicht gesetzt: {e}")
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    log("importiert: " + ", ".join(task.get_editor_property("imported_object_paths")))
    mesh = anim = None
    for p in eal.list_assets(DST, True):
        a = unreal.load_asset(p)
        if isinstance(a, unreal.SkeletalMesh) and not mesh: mesh = a
        if isinstance(a, unreal.AnimSequence) and not anim: anim = a
    if not mesh: raise RuntimeError("kein SkeletalMesh importiert")
    log(f"mesh={mesh.get_path_name()} anim={anim.get_path_name() if anim else None}")

    data = unreal.load_asset(f"{DST}/ProfessorSplat")
    if not isinstance(data, unreal.SkinnedSplatData):
        fac = unreal.DataAssetFactory(); fac.set_editor_property("data_asset_class", unreal.SkinnedSplatData)
        data = unreal.AssetToolsHelpers.get_asset_tools().create_asset("ProfessorSplat", DST, unreal.SkinnedSplatData, fac)
    if not data or not data.load_from_file(SPLAT): raise RuntimeError("Splat-Daten nicht geladen")
    eal.save_loaded_asset(data, False); log(f"splat data: {data.get_num_bytes()} bytes")

    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not les.load_level(SRC_MAP): raise RuntimeError("ThirdPerson-Map laden fehlgeschlagen")
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    ps = [a for a in eas.get_all_level_actors() if isinstance(a, unreal.PlayerStart)]
    if ps:
        loc = ps[0].get_actor_location(); fwd = ps[0].get_actor_forward_vector(); yaw = ps[0].get_actor_rotation().yaw
    else:
        loc, fwd, yaw = unreal.Vector(0, 0, 100), unreal.Vector(1, 0, 0), 0.0
    # 4 m in front of the player, on the floor (PlayerStart sits ~ capsule half height above it), facing the player
    pos = unreal.Vector(loc.x + fwd.x * 400, loc.y + fwd.y * 400, loc.z - 90)
    # Blender rig faces -Y; after FBX import that is +Y in UE -> rotate so it faces back towards the player
    act = eas.spawn_actor_from_class(unreal.SkinnedSplatActor, pos, unreal.Rotator(0, yaw + 90, 0))
    if not act: raise RuntimeError("SkinnedSplatActor nicht gespawnt")
    act.set_actor_label("Professor")
    act.set_editor_property("splat_data", data)
    if anim: act.set_editor_property("animation", anim)
    act.get_editor_property("mesh").set_editor_property("skeletal_mesh_asset", mesh)
    log(f"Professor bei {pos}, PlayerStart {loc}")
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP): raise RuntimeError("Map speichern fehlgeschlagen")
    set_default_maps(); log(f"Map gespeichert: {MAP}"); ok = True
except Exception:
    lines.append(traceback.format_exc()); unreal.log_error(traceback.format_exc())
finally:
    with open(STATUS, "w") as f: f.write(("OK\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
    unreal.SystemLibrary.quit_editor()
