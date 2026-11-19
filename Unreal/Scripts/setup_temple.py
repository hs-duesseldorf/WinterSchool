# Runs inside the editor: imports a static world splat (TEMPLE_PLY, plain 3DGS .ply, through Epic's GaussianSplat
# importer) and its collision mesh (TEMPLE_COLLISION, .glb from collision_from_splat_transform.py), then builds /Game/Temple/TempleMap
# from SplatMap: the playground geometry is removed, the splat goes in at the origin, the collision mesh on top of it
# (complex-as-simple collision, invisible), and PlayerStart + Professor are dropped onto the collision. Saves, quits.
import math, os, traceback, unreal

PLY = os.environ["TEMPLE_PLY"]; COL = os.environ["TEMPLE_COLLISION"]
NAME = os.environ.get("TEMPLE_NAME", "SakaShrine")
STATUS = os.environ.get("SPLAT_STATUS", "/tmp/splat_temple.status")
SRC_MAP = os.environ.get("TEMPLE_SRC_MAP", "/Game/SplatMap"); MAP = os.environ.get("TEMPLE_MAP", "/Game/Temple/TempleMap")
DST = "/Game/Temple"
# where to put the player and the professor, in Unreal cm (x, y) + yaw the player looks along; z comes from a trace
START = [float(v) for v in os.environ.get("TEMPLE_START", "480,600,-133").split(",")]
PROF = [float(v) for v in os.environ.get("TEMPLE_PROF", "-120,-60").split(",")]
lines = []
def log(m): unreal.log("[TempleSetup] " + m); lines.append(m)
eal = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()


def import_file(path, name, dest):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", path); task.set_editor_property("destination_path", dest)
    task.set_editor_property("destination_name", name)
    task.set_editor_property("automated", True); task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    tools.import_asset_tasks([task])
    return [unreal.load_asset(p) for p in task.get_editor_property("imported_object_paths")]


def trace_down(world, x, y):
    hit = unreal.SystemLibrary.line_trace_single(world, unreal.Vector(x, y, 3000), unreal.Vector(x, y, -3000),
                                                 unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, True, [],
                                                 unreal.DrawDebugTrace.NONE, True)
    if not hit: return None
    t = hit.to_tuple()  # (blocking_hit, initial_overlap, time, distance, location, impact_point, ...)
    return t[5].z if t[0] else None


ok = False
try:
    if eal.does_directory_exist(DST): eal.delete_directory(DST); log(f"deleted old {DST}")

    splat = next((o for o in import_file(PLY, NAME, DST) if isinstance(o, unreal.GaussianSplat)), None)
    if not splat: raise RuntimeError(f"{PLY} was not imported as a GaussianSplat (see the log: too many splats?)")
    log(f"splat {splat.get_path_name()}")

    mesh = next((o for o in import_file(COL, NAME + "_Collision", DST + "/Collision") if isinstance(o, unreal.StaticMesh)), None)
    if not mesh: raise RuntimeError(f"{COL} did not import a StaticMesh")
    try:
        ns = mesh.get_editor_property("nanite_settings"); ns.set_editor_property("enabled", False)
        mesh.set_editor_property("nanite_settings", ns)  # no Nanite on the web
    except Exception as e: log(f"nanite setting: {e}")
    unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem).remove_collisions(mesh)  # no simple shapes, only the triangles
    body = mesh.get_editor_property("body_setup")
    body.set_editor_property("collision_trace_flag", unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
    eal.save_loaded_asset(mesh, False)
    b = mesh.get_bounding_box()
    log(f"collision {mesh.get_path_name()}: {mesh.get_num_triangles(0)} tris, bounds {b.min} .. {b.max}")

    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not les.load_level(SRC_MAP): raise RuntimeError(f"loading {SRC_MAP} failed")
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()
    gone = [a for a in actors if str(a.get_folder_path()) == "Playground" or a.get_actor_label() == "Floor"]
    for a in gone: eas.destroy_actor(a)
    log(f"removed {len(gone)} playground actors")

    sa = eas.spawn_actor_from_object(splat, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    if not sa: raise RuntimeError("spawning the splat actor failed")
    sa.set_actor_label(NAME)
    ca = eas.spawn_actor_from_object(mesh, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    ca.set_actor_label(NAME + "_Collision")
    smc = ca.get_editor_property("static_mesh_component")
    smc.set_collision_profile_name("BlockAll")
    smc.set_editor_property("cast_shadow", False)
    smc.set_visibility(False)  # still collides; tick Visible on the component to look at it
    ca.set_actor_hidden_in_game(True)
    for a in (sa, ca): a.set_folder_path(NAME)

    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas.get_all_level_actors()  # flush
    ground = trace_down(world, START[0], START[1])
    if ground is None: raise RuntimeError(f"no collision under the player start at {START[:2]}")
    for a in eas.get_all_level_actors():
        if isinstance(a, unreal.PlayerStart):
            a.set_actor_location(unreal.Vector(START[0], START[1], ground + 100), False, True)
            a.set_actor_rotation(unreal.Rotator(roll=0, pitch=0, yaw=START[2]), True)
    log(f"PlayerStart on the ground at z = {ground:.0f} cm")
    pg = trace_down(world, PROF[0], PROF[1])
    for a in eas.get_all_level_actors():
        if isinstance(a, unreal.SkinnedSplatActor) and pg is not None:
            yaw = math.degrees(math.atan2(START[1] - PROF[1], START[0] - PROF[0]))  # face the player (splat looks along +X)
            a.set_actor_location(unreal.Vector(PROF[0], PROF[1], pg), False, True)
            a.set_actor_rotation(unreal.Rotator(roll=0, pitch=0, yaw=yaw), True)
            log(f"Professor at ({PROF[0]:.0f}, {PROF[1]:.0f}, {pg:.0f}), yaw {yaw:.0f}")
    # a few probes so the log shows the collision follows the splat (ground ~0 on the plaza)
    for x, y in ((0, 0), (400, 400), (-600, -600), (800, 960), (-1000, -1000)):
        z = trace_down(world, x, y); log(f"ground at ({x}, {y}): {'none' if z is None else f'{z:.0f}'}")

    if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP): raise RuntimeError("saving the map failed")
    log(f"map saved: {MAP}"); ok = True
except Exception:
    lines.append(traceback.format_exc()); unreal.log_error(traceback.format_exc())
finally:
    with open(STATUS, "w") as f: f.write(("OK\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
    if not os.environ.get("SPLAT_KEEP_OPEN"): unreal.SystemLibrary.quit_editor()
