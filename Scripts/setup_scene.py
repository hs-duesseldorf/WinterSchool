# Runs inside the editor (-ExecutePythonScript): import the .ply, make /Game/Splat/SplatMap with the splat,
# a PlayerStart looking at it and a free-fly pawn (GameModeBase -> DefaultPawn), make it the default map, quit.
import os, re, traceback, unreal

PLY = os.environ["SPLAT_PLY"]
STATUS = os.environ.get("SPLAT_STATUS", "/tmp/splat_scene.status")
MAP = "/Game/Splat/SplatMap"
lines = []

def log(msg):
    unreal.log("[SplatSetup] " + msg)
    lines.append(msg)

def set_default_maps():
    ini = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_config_dir()), "DefaultEngine.ini")
    text = open(ini).read()
    full = f"{MAP}.{MAP.rsplit('/', 1)[1]}"
    for key in ("GameDefaultMap", "EditorStartupMap"):
        if re.search(rf"^{key}=", text, re.M):
            text = re.sub(rf"^{key}=.*$", f"{key}={full}", text, flags=re.M)
        else:
            text = text.replace("[/Script/EngineSettings.GameMapsSettings]",
                                f"[/Script/EngineSettings.GameMapsSettings]\n{key}={full}", 1)
    if "GameMapsSettings]" not in text:
        text += f"\n[/Script/EngineSettings.GameMapsSettings]\nGameDefaultMap={full}\nEditorStartupMap={full}\n"
    open(ini, "w").write(text)

ok = False
try:
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", PLY)
    task.set_editor_property("destination_path", "/Game/Splat")
    task.set_editor_property("destination_name", "Scene")
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    asset = unreal.load_asset("/Game/Splat/Scene")
    if not asset:
        raise RuntimeError(f"Import lieferte kein Asset (imported: {list(task.get_editor_property('imported_object_paths'))})")
    log(f"importiert: {asset.get_path_name()} ({asset.get_class().get_name()})")

    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if unreal.EditorAssetLibrary.does_asset_exist(MAP):
        unreal.EditorAssetLibrary.delete_asset(MAP)
    try:
        made = les.new_level(MAP, False)
    except TypeError:
        made = les.new_level(MAP)
    if not made:
        raise RuntimeError("new_level fehlgeschlagen")

    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    splat = eas.spawn_actor_from_object(asset, unreal.Vector(0, 0, 0))
    if not splat:
        raise RuntimeError("Splat-Actor konnte nicht gespawnt werden")
    splat.set_actor_label("GaussianSplat")
    origin, extent = splat.get_actor_bounds(False)
    r = max(extent.x, extent.y, extent.z)
    if r < 1:
        origin, r = unreal.Vector(0, 0, 0), 500.0
    log(f"Bounds: origin={origin} extent={extent}")
    start = unreal.Vector(origin.x - r * 1.6, origin.y, origin.z + r * 0.3)
    ps = eas.spawn_actor_from_class(unreal.PlayerStart, start, unreal.Rotator(0, -10, 0))
    if not ps:
        raise RuntimeError("PlayerStart konnte nicht gespawnt werden")

    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    world.get_world_settings().set_editor_property("default_game_mode", unreal.GameModeBase)

    if not les.save_current_level():
        raise RuntimeError("Level speichern fehlgeschlagen")
    set_default_maps()
    log(f"Map gespeichert: {MAP} (Spieler startet bei {start}, WASD + Maus, E/Q hoch/runter)")
    ok = True
except Exception:
    lines.append(traceback.format_exc())
    unreal.log_error(traceback.format_exc())
finally:
    with open(STATUS, "w") as f:
        f.write(("OK\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
    unreal.SystemLibrary.quit_editor()
