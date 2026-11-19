# Runs inside the editor: builds the live mocap pipeline with Unreal's own pieces (Live Link + Performance Capture +
# IK Retargeter) and a test map /Game/Mocap/MocapLive:
#
#   Captury Live / Captury Replay / Scripts/captury_replay.py
#     -> Live Link source "Captury Live" (127.0.0.1, preset /Game/Mocap/LLP_Captury, applied at startup)
#     -> CapturePerformer: the performer's own skeleton (MOCAP_SOURCE mesh) driven by the Live Link subject
#     -> RetargetComponent + IK Retargeter -> professor (skinned splat) and Quinn, at runtime, every frame
#
#   MOCAP_SOURCE    skeletal mesh of the streamed skeleton (default: /Game/Mocap/Captury/SK_Captury, made from
#                   CAPTURY_RIG_FILE if missing, else Manny, which matches takes of the mannequin anims)
#   MOCAP_SOURCE_NAME  its name in the rig assets (default Captury / Manny)
#   MOCAP_SUBJECT   Live Link subject name (default Performer1, the replay's first actor)
#   MOCAP_ADDRESS   Captury machine (default 127.0.0.1)
#
# The professor's retargeter is the one the offline retarget uses (Scripts/retarget_common.py):
# /Game/Professor/RTG_<source>_To_Professor. It is made once and kept, so editor tweaks apply to live and recorded
# motion alike (REBUILD_RIGS=1 makes it again). Everything is plain assets and actors, tweak them in the editor.
import os, sys, traceback, unreal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import retarget_common as rc

STATUS = os.environ.get("MOCAP_STATUS", "/tmp/setup_mocap.status")
SUBJECT = os.environ.get("MOCAP_SUBJECT", "Performer1")
ADDRESS = os.environ.get("MOCAP_ADDRESS", "127.0.0.1")
DST, RIGS, MAP = "/Game/Mocap", "/Game/Mocap/Rigs", "/Game/Mocap/MocapLive"
PRESET = "/Game/Mocap/LLP_Captury"
QUINN = "/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple"
lines = []
def log(m): unreal.log("[MocapSetup] " + m); lines.append(m)
eal = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()

def add_component(actor, cls):
    sds = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    root = sds.k2_gather_subobject_data_for_instance(actor)[0]
    handle, fail = sds.add_new_subobject(unreal.AddNewSubobjectParams(parent_handle=root, new_class=cls))
    if not fail.is_empty(): raise RuntimeError(f"adding {cls.__name__} to {actor.get_actor_label()}: {fail}")
    lib = unreal.SubobjectDataBlueprintFunctionLibrary
    return lib.get_object(lib.get_data(handle))

def component_ref(name):
    r = unreal.ComponentReference(); r.set_editor_property("component_property", name); return r

def live_link_preset():
    if not unreal.CapturyLiveLinkLibrary.add_captury_source(ADDRESS, False, False):
        raise RuntimeError(f"Captury source for {ADDRESS} not created")
    if eal.does_asset_exist(PRESET): eal.delete_asset(PRESET)
    preset = tools.create_asset(PRESET.rsplit("/", 1)[1], DST, unreal.LiveLinkPreset, None)
    if not preset: raise RuntimeError("creating the Live Link preset failed")
    preset.build_from_client()
    eal.save_loaded_asset(preset, False)
    # Project Settings > Live Link > Default Live Link Preset (applied whenever the editor or the game starts). The
    # settings class is not exposed to Python, so the line goes into DefaultGame.ini directly.
    ini = os.path.join(unreal.Paths.project_config_dir(), "DefaultGame.ini")
    section, key = "[/Script/LiveLink.LiveLinkSettings]", f"DefaultLiveLinkPreset={PRESET}.{PRESET.rsplit('/', 1)[1]}"
    text = open(ini).read() if os.path.exists(ini) else ""
    out, skip = [], False
    for line in text.splitlines():  # drop an older copy of the section
        if line.strip().startswith("["): skip = line.strip() == section
        if not skip: out.append(line)
    with open(ini, "w") as f: f.write("\n".join(out).rstrip() + f"\n\n{section}\n{key}\n")
    log(f"{PRESET}: Captury source {ADDRESS}:2101, default Live Link preset")

ok = False
try:
    # the map and the preset are made again; the rigs are kept (retarget_common.py)
    for p in (MAP, PRESET):
        if eal.does_asset_exist(p): eal.delete_asset(p)
    if os.environ.get("MOCAP_SOURCE"):
        src_mesh = unreal.load_asset(os.environ["MOCAP_SOURCE"])
        src_name = src_mesh.get_name()
    else:
        src_name, src_mesh = rc.default_source(log)
    src_name = os.environ.get("MOCAP_SOURCE_NAME", src_name)
    rtg_prof, prof_data, prof_mesh = rc.professor_retargeter(src_name, src_mesh, log)
    quinn = unreal.load_asset(QUINN)
    rtg_quinn = rc.retargeter(f"{RIGS}/RTG_{src_name}_To_{quinn.get_name()}", rc.ik_rig(f"{RIGS}/IK_{src_name}", src_mesh, log),
                              src_mesh, rc.ik_rig(f"{RIGS}/IK_{quinn.get_name()}", quinn, log), quinn, log)
    live_link_preset()

    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if eal.does_asset_exist(MAP): eal.delete_asset(MAP)
    if not les.new_level_from_template(MAP, "/Engine/Maps/Templates/Template_Default"): raise RuntimeError("new level failed")
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    # the performer: the streamed skeleton itself, driven straight by Live Link
    perf = eas.spawn_actor_from_class(unreal.CapturePerformer, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    perf.set_actor_label("Performer1")
    perf.set_mocap_mesh(src_mesh)
    perf.set_live_link_subject(unreal.LiveLinkSubjectName(SUBJECT))

    # retargeted characters next to it: the professor splat (its hidden driving mesh is the controlled mesh) and Quinn
    # yaw 90: the splat actor turns its driving mesh by -90 (Mixamo axes), this makes it face the performer's way
    prof = eas.spawn_actor_from_class(unreal.SkinnedSplatActor, unreal.Vector(250, 0, 0), unreal.Rotator(0, 0, 90))
    prof.set_actor_label("ProfessorLive")
    prof.set_editor_property("splat_data", prof_data)
    rcomp = add_component(prof, unreal.RetargetComponent)
    rcomp.set_editor_property("controlled_skeletal_mesh_component", component_ref("Mesh"))
    rcomp.set_retarget_asset(rtg_prof); rcomp.set_source_performer(perf)

    q = eas.spawn_actor_from_class(unreal.CaptureCharacter, unreal.Vector(-250, 0, 0), unreal.Rotator(0, 0, 0))
    q.set_actor_label("QuinnLive")
    q.skeletal_mesh_component.set_skeletal_mesh_asset(quinn)
    q.set_retarget_asset(rtg_quinn); q.set_source_performer(perf)

    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP): raise RuntimeError("saving the map failed")
    log(f"map saved: {MAP} (performer subject {SUBJECT}, mesh {src_mesh.get_name()})"); ok = True
except Exception:
    lines.append(traceback.format_exc()); unreal.log_error(traceback.format_exc())
finally:
    with open(STATUS, "w") as f: f.write(("OK\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
    if not os.environ.get("MOCAP_KEEP_OPEN"): unreal.SystemLibrary.quit_editor()
