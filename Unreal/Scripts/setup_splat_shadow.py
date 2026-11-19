# Runs inside the editor (Scripts/setup_splat_shadow.sh): creates the splat shadow light function and puts it on the sun
# of each map in SHADOW_MAPS. Saves, quits.
#
# Splats have no scene proxy, so Unreal's shadow passes never draw them; the GaussianSplat plugin draws every splat with
# Cast Shadow into a shadow map of its own (GaussianSplatShadow.cpp) and the sun applies it as a light function, which
# multiplies only the sun's direct light, after its regular shadows. Plugin content, /GaussianSplat/Shadow:
#   RT_GaussianSplatShadowMap     RGBA16F, cascades of SHADOW_RESOLUTION^2 side by side (r.GaussianSplat.Shadow.Cascades)
#   RT_GaussianSplatShadowParams  RGBA32F 32 x 1, the cascade transforms (written with the map every frame)
#   M_GaussianSplatShadow         light function: GaussianSplatShadow() of /Plugin/GaussianSplat/Public/GaussianSplatShadow.ush
# The renderer finds the sun by these render target names in its light function.
import os, traceback, unreal

STATUS = os.environ.get("SPLAT_STATUS", "/tmp/winterschool_splat_shadow.status")
MAPS = [m for m in os.environ.get("SHADOW_MAPS", "/Game/Temple/TempleMap").split(",") if m]
# Splat actors that keep their shadow off: captured scenes already contain their own shadows (would darken twice)
NO_SHADOW = [a for a in os.environ.get("NO_SHADOW_ACTORS", "SakaShrine").split(",") if a]
RESOLUTION = int(os.environ.get("SHADOW_RESOLUTION", "1024"))
SLOTS = int(os.environ.get("SHADOW_SLOTS", "2"))  # most cascades the map holds
DIR = "/GaussianSplat/Shadow"
INCLUDE = "/Plugin/GaussianSplat/Public/GaussianSplatShadow.ush"
lines = []
def log(m): unreal.log("[SplatShadowSetup] " + m); lines.append(m)
eal = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()
MEL = unreal.MaterialEditingLibrary


def asset(name, cls, factory):
    path = f"{DIR}/{name}"
    a = unreal.load_asset(path) if eal.does_asset_exist(path) else tools.create_asset(name, DIR, cls, factory)
    if not a: raise RuntimeError(f"creating {path} failed")
    return a


def render_target(name, width, height, fmt):
    rt = asset(name, unreal.TextureRenderTarget2D, unreal.TextureRenderTargetFactoryNew())
    for prop, value in [("size_x", width), ("size_y", height), ("render_target_format", fmt),
                        ("clear_color", unreal.LinearColor(1, 1, 1, 1)), ("auto_generate_mips", False),
                        ("address_x", unreal.TextureAddress.TA_CLAMP), ("address_y", unreal.TextureAddress.TA_CLAMP),
                        ("filter", unreal.TextureFilter.TF_NEAREST)]:
        rt.set_editor_property(prop, value)
    eal.save_loaded_asset(rt, False)
    log(f"{DIR}/{name}: {width} x {height} {fmt}")
    return rt


def light_function(shadow_map, params):
    mat = asset("M_GaussianSplatShadow", unreal.Material, unreal.MaterialFactoryNew())
    MEL.delete_all_material_expressions(mat)
    mat.set_editor_property("material_domain", unreal.MaterialDomain.MD_LIGHT_FUNCTION)
    pos = MEL.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -700, 0)
    textures = []
    for i, rt in enumerate((shadow_map, params)):
        t = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureObject, -700, 150 + 150 * i)
        t.set_editor_property("texture", rt)
        t.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        textures.append(t)
    custom = MEL.create_material_expression(mat, unreal.MaterialExpressionCustom, -350, 0)
    inputs = []
    for n in ("WorldPosition", "ShadowMap", "ShadowParams"):
        ci = unreal.CustomInput(); ci.set_editor_property("input_name", n); inputs.append(ci)
    custom.set_editor_property("inputs", inputs)
    custom.set_editor_property("include_file_paths", [INCLUDE])
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("description", "GaussianSplatShadow")
    custom.set_editor_property("code", "return GaussianSplatShadow(WorldPosition, ShadowMap, ShadowParams, View.FrameNumber).xxx;")
    MEL.connect_material_expressions(pos, "", custom, "WorldPosition")
    MEL.connect_material_expressions(textures[0], "", custom, "ShadowMap")
    MEL.connect_material_expressions(textures[1], "", custom, "ShadowParams")
    MEL.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(mat)
    eal.save_loaded_asset(mat, False)
    log(f"{DIR}/M_GaussianSplatShadow: light function")
    return mat


ok = False
try:
    shadow_map = render_target("RT_GaussianSplatShadowMap", RESOLUTION * SLOTS, RESOLUTION, unreal.TextureRenderTargetFormat.RTF_RGBA16F)
    params = render_target("RT_GaussianSplatShadowParams", 32, 1, unreal.TextureRenderTargetFormat.RTF_RGBA32F)
    mat = light_function(shadow_map, params)

    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for m in MAPS:
        if not les.load_level(m): raise RuntimeError(f"loading {m} failed")
        suns = [a for a in eas.get_all_level_actors() if isinstance(a, unreal.DirectionalLight)]
        for a in suns:
            c = a.get_component_by_class(unreal.DirectionalLightComponent)
            c.set_editor_property("light_function_material", mat)
            c.set_editor_property("disabled_brightness", 1.0)  # no splat shadow rather than a dimmed sun when faded out
        for a in eas.get_all_level_actors():
            if a.get_actor_label() in NO_SHADOW:
                for c in a.get_components_by_class(unreal.GaussianSplatComponent): c.set_editor_property("cast_shadow", False)
        casters = [f"{a.get_actor_label()}={c.get_editor_property('cast_shadow')}" for a in eas.get_all_level_actors()
                   for c in a.get_components_by_class(unreal.GaussianSplatComponent)]
        world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
        if not unreal.EditorLoadingAndSavingUtils.save_map(world, m): raise RuntimeError(f"saving {m} failed")
        log(f"{m}: light function on {[a.get_actor_label() for a in suns]}; splat components (cast shadow) {casters}")
    ok = True
except Exception:
    lines.append(traceback.format_exc()); unreal.log_error(traceback.format_exc())
finally:
    with open(STATUS, "w") as f: f.write(("OK\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
    if not os.environ.get("SPLAT_KEEP_OPEN"): unreal.SystemLibrary.quit_editor()
