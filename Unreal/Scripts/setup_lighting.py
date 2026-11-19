# Runs inside the editor (Scripts/setup_lighting.sh): matches TempleMap's lighting and look to the capture and sets it up
# for the web. Reads LIGHTING_JSON (Scripts/fit_capture_lighting.py output). Saves, quits.
# - Sun and sky light: the capture's sun direction; shadow distance to the scene's size (sharper, cheaper cascades). Their
#   colours and intensities follow the capture's shade: per channel Sun = Sky * (1 - shade) / shade, with Sky
#   the sky light's irradiance from the sky dome (and the capture's ground colour as its lower hemisphere), and the sky
#   light just strong enough that the brightest sunlit ground is still a valid albedo (SUN_MAX_ALBEDO). The splats' shadow
#   darkness is its own setting (ShadowStrength, GaussianSplatDeferred.usf): set to the capture's shade, 1 - shade
#   (luminance), on the splat components and skinned splat actors; this sun keeps that albedo in range. Characters get the
#   same lights, so they sit in the scene like the splats; mobile has no GI, so the sky light is all their indirect light.
#   The sky light is captured once, not every frame.
# - Height fog: removed (black inscattering only darkened the scene and cost a pass).
# - Post process: effects that cost GPU time and shift the captured colours (bloom, motion blur, vignette, grain, lens
#   flares, AO, local exposure) are off; exposure stays manual at 1, so splats show their captured colours.
# - Splat asset: lighter cooked data for the web (SPLAT_SH_WASM, SPLAT_SH_PHONE, SPLAT_MINLOD_PHONE): desktop browsers get
#   the Wasm cook, phones and tablets the Wasm_ASTC cook (Scripts/build_web.sh).
import json, math, os, traceback, unreal

MAP = os.environ.get("TEMPLE_MAP", "/Game/Temple/TempleMap")
SPLAT = os.environ.get("TEMPLE_SPLAT", "/Game/Temple/SakaShrine")
LIGHTING = json.load(open(os.environ["LIGHTING_JSON"]))
STATUS = os.environ.get("SPLAT_STATUS", "/tmp/winterschool_lighting.status")
SH_WASM, SH_PHONE = int(os.environ.get("SPLAT_SH_WASM", "1")), int(os.environ.get("SPLAT_SH_PHONE", "0"))
MINLOD_PHONE = int(os.environ.get("SPLAT_MINLOD_PHONE", "1"))
SHADOW_DISTANCE = float(os.environ.get("SHADOW_DISTANCE_CM", "3000"))
MAX_ALBEDO = float(os.environ.get("SUN_MAX_ALBEDO", "0.9"))  # bright sunlit ground maps to this albedo at most
# (SKY_INTENSITY overrides the fitted sky light intensity; the sun follows from it and the capture's shade)
lines = []
def log(m): unreal.log("[LightingSetup] " + m); lines.append(m)
MEL = unreal.MaterialEditingLibrary


def inverse_tonemap(display_linear):
    """Scene-linear value the filmic tone curve (ACES fit) maps to this display-linear value, so the unlit sky shows the
    capture's sky colour like the splats do (they get the engine's exact inverse curve)"""
    def curve(x): return x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14)
    lo, hi = 0.0, 64.0
    for _ in range(60):
        mid = (lo + hi) / 2
        lo, hi = (mid, hi) if curve(mid) < display_linear else (lo, mid)
    return lo


def dome_radiance(direction_z, horizon, zenith, ground):
    """What M_CaptureSky shows in a direction (its z), with the sky light's lower hemisphere colour below the horizon"""
    if direction_z < 0: return ground
    t = direction_z ** 0.6
    return [h + (z - h) * t for h, z in zip(horizon, zenith)]


def sky_irradiance(normal, horizon, zenith, ground, n=96):
    """Lambert radiance per unit albedo the sky light gives a surface with this normal at intensity 1 (like the engine's
    SH diffuse lookup): the cosine weighted mean of the dome over the normal's hemisphere"""
    total, weight = [0.0, 0.0, 0.0], 0.0
    for i in range(n):
        z = 1 - 2 * (i + 0.5) / n  # uniform in z = uniform in solid angle
        r = math.sqrt(max(1 - z * z, 0))
        for j in range(2 * n):
            phi = 2 * math.pi * (j + 0.5) / (2 * n)
            d = (r * math.cos(phi), r * math.sin(phi), z)
            c = sum(a * b for a, b in zip(d, normal))
            if c <= 0: continue
            L = dome_radiance(z, horizon, zenith, ground)
            total = [t + c * l for t, l in zip(total, L)]
            weight += c
    # cosine weighted mean over the hemisphere: integral(L cos) / pi, with integral(cos) = pi
    return [t / weight for t in total]


def sky_material(horizon, zenith):
    path = "/Game/Temple/M_CaptureSky"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset("M_CaptureSky", "/Game/Temple", unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    mat.set_editor_property("is_sky", True)  # no fog, drawn as the sky
    # Height of the view direction (camera vector points to the camera: negate), 0 at the horizon, 1 straight up
    cam = MEL.create_material_expression(mat, unreal.MaterialExpressionCameraVectorWS, -900, 0)
    mask = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -750, 0)
    mask.set_editor_property("r", False); mask.set_editor_property("g", False); mask.set_editor_property("b", True)
    neg = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -600, 0)
    neg.set_editor_property("const_b", -1.0)
    sat = MEL.create_material_expression(mat, unreal.MaterialExpressionSaturate, -450, 0)
    pw = MEL.create_material_expression(mat, unreal.MaterialExpressionPower, -300, 0)
    pw.set_editor_property("const_exponent", 0.6)
    h = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -300, -200)
    h.set_editor_property("constant", unreal.LinearColor(*horizon, 1))
    z = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -300, 200)
    z.set_editor_property("constant", unreal.LinearColor(*zenith, 1))
    lerp = MEL.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -100, 0)
    MEL.connect_material_expressions(cam, "", mask, "")
    MEL.connect_material_expressions(mask, "", neg, "A")
    MEL.connect_material_expressions(neg, "", sat, "")
    MEL.connect_material_expressions(sat, "", pw, "Base")
    MEL.connect_material_expressions(h, "", lerp, "A")
    MEL.connect_material_expressions(z, "", lerp, "B")
    MEL.connect_material_expressions(pw, "", lerp, "Alpha")
    MEL.connect_material_property(lerp, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat, False)
    return mat


def per_platform(default, **values):
    p = unreal.PerPlatformInt()
    p.set_editor_property("default", default)
    p.set_editor_property("per_platform", {unreal.Name(k): v for k, v in values.items()})
    return p


ok = False
try:
    splat = unreal.load_asset(SPLAT)
    splat.set_editor_property("max_sh_degree", per_platform(3, Wasm=SH_WASM, Wasm_ASTC=SH_PHONE))
    splat.set_editor_property("min_lod", per_platform(0, Wasm_ASTC=MINLOD_PHONE))
    unreal.EditorAssetLibrary.save_loaded_asset(splat, False)
    log(f"{SPLAT}: SH degree web {SH_WASM}, phones {SH_PHONE}; min LOD phones {MINLOD_PHONE}")

    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not les.load_level(MAP): raise RuntimeError(f"loading {MAP} failed")
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    horizon = [inverse_tonemap(v) for v in LIGHTING["sky_horizon_linear"]]
    zenith = [inverse_tonemap(v) for v in LIGHTING["sky_zenith_linear"]]
    ground = [inverse_tonemap(v) for v in LIGHTING["ground_linear"]]
    r = LIGHTING["light_rotation"]
    # direction towards the sun, from the light's rotation (Unreal: X forward, the light shines along -to_sun)
    pitch, yaw = math.radians(r["pitch"]), math.radians(r["yaw"])
    to_sun = (-math.cos(pitch) * math.cos(yaw), -math.cos(pitch) * math.sin(yaw), -math.sin(pitch))
    sky_unit = sky_irradiance(to_sun, horizon, zenith, ground)  # splat GBuffer normals face the sun
    shade = LIGHTING["shadow_color_linear"]
    # (a channel the capture saturated says nothing about the lights: cap it below the tone curve's top)
    lit = [inverse_tonemap(min(v, 0.95)) for v in LIGHTING["ground_lit_p95_linear"]]
    # Sun + Sky = Sky / shade must reach lit / MAX_ALBEDO in every channel
    sky_intensity = float(os.environ.get("SKY_INTENSITY", 0) or max(
        sh * l / (MAX_ALBEDO * k) for sh, l, k in zip(shade, lit, sky_unit)))
    sky_irr = [sky_intensity * k for k in sky_unit]
    sun_irr = [k * (1 - sh) / sh for k, sh in zip(sky_irr, shade)]  # Lambert radiance per albedo = lux * colour / pi
    sun_lux = math.pi * max(sun_irr)
    sun_color = [v / max(sun_irr) for v in sun_irr]
    shadow_strength = 1.0 - (0.2126 * shade[0] + 0.7152 * shade[1] + 0.0722 * shade[2])
    sky = sky_light = None
    for a in eas.get_all_level_actors():
        for comp in a.get_components_by_class(unreal.GaussianSplatComponent):
            comp.set_editor_property("shadow_strength", shadow_strength)
            log(f"{a.get_actor_label()}: splat shadow strength {shadow_strength:.2f}")
        if isinstance(a, unreal.SkinnedSplatActor):
            a.set_editor_property("shadow_strength", shadow_strength)
            log(f"{a.get_actor_label()}: skinned splat shadow strength {shadow_strength:.2f}")
        name = a.get_actor_label()
        if isinstance(a, unreal.DirectionalLight):
            a.set_actor_rotation(unreal.Rotator(roll=r["roll"], pitch=r["pitch"], yaw=r["yaw"]), False)
            c = a.get_component_by_class(unreal.DirectionalLightComponent)
            c.set_editor_property("dynamic_shadow_distance_movable_light", SHADOW_DISTANCE)
            c.set_editor_property("use_temperature", False)
            c.set_editor_property("intensity", sun_lux)
            c.set_light_color(unreal.LinearColor(*sun_color, 1))
            log(f"sun: pitch {r['pitch']}, yaw {r['yaw']}, {sun_lux:.2f} lux, colour {[round(v, 3) for v in sun_color]}, "
                f"shadow distance {SHADOW_DISTANCE:.0f} cm")
        elif isinstance(a, unreal.SkyLight):
            c = a.get_component_by_class(unreal.SkyLightComponent)
            c.set_editor_property("real_time_capture", False)
            # The dome has no lower half (and splats are not in the capture): the capture's ground colour stands in for it
            c.set_editor_property("source_type", unreal.SkyLightSourceType.SLS_CAPTURED_SCENE)
            c.set_editor_property("lower_hemisphere_is_black", True)
            c.set_editor_property("lower_hemisphere_color", unreal.LinearColor(*ground, 1))
            c.set_editor_property("intensity", sky_intensity)
            c.set_light_color(unreal.LinearColor(1, 1, 1, 1))
            sky_light = c
            log(f"sky light: intensity {sky_intensity:.2f}, lower hemisphere {[round(v, 3) for v in ground]}, captured once; "
                f"sun / sky irradiance {[round(a / b, 2) for a, b in zip(sun_irr, sky_irr)]}")
        elif isinstance(a, unreal.ExponentialHeightFog):
            eas.destroy_actor(a)
            log("height fog removed")
        elif name == "SM_SkySphere":
            sky = a
        elif isinstance(a, unreal.PostProcessVolume):
            s = a.get_editor_property("settings")
            for prop, value in [("bloom_intensity", 0.0), ("motion_blur_amount", 0.0), ("vignette_intensity", 0.0),
                                ("film_grain_intensity", 0.0), ("lens_flare_intensity", 0.0),
                                ("ambient_occlusion_intensity", 0.0), ("local_exposure_highlight_contrast_scale", 1.0),
                                ("local_exposure_shadow_contrast_scale", 1.0), ("scene_fringe_intensity", 0.0)]:
                try:
                    s.set_editor_property("override_" + prop, True)
                    s.set_editor_property(prop, value)
                except Exception as e:
                    log(f"post process {prop}: {e}")
            a.set_editor_property("settings", s)
            log("post process: bloom, motion blur, vignette, grain, lens flares, AO, local exposure off")
    if sky is None: raise RuntimeError("no SM_SkySphere in the map")
    mat = sky_material(horizon, zenith)
    c = sky.get_component_by_class(unreal.StaticMeshComponent)
    c.set_material(0, mat)
    c.set_editor_property("cast_shadow", False)
    if sky_light: sky_light.recapture_sky()  # now that the dome shows the capture's sky
    log(f"sky: M_CaptureSky, horizon {[round(v, 3) for v in horizon]}, zenith {[round(v, 3) for v in zenith]} (scene-linear)")

    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP): raise RuntimeError("saving the map failed")
    log(f"map saved: {MAP}"); ok = True
except Exception:
    lines.append(traceback.format_exc()); unreal.log_error(traceback.format_exc())
finally:
    with open(STATUS, "w") as f: f.write(("OK\n" if ok else "FAIL\n") + "\n".join(lines) + "\n")
    if not os.environ.get("SPLAT_KEEP_OPEN"): unreal.SystemLibrary.quit_editor()
