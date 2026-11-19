# Retarget assets shared by the offline path (Scripts/retarget_anims.py: animation files -> /Game/Professor/Anims) and
# the live path (Scripts/setup_mocap.py: Captury Live -> Live Link -> RetargetComponent). Both use the same IK Retargeter
# per source skeleton, so a fix made to it in the editor applies to recorded and live motion alike:
#
#   /Game/Mocap/Captury/SK_Captury, SKEL_Captury  the one Captury skeleton (with fingers) every recording is imported
#                                                onto, as animation only: /Game/Mocap/Captury/Anims/<recording>
#   /Game/Mocap/Rigs/IK_<Source>                 IK Rig of the source skeleton (Captury, Manny, ...)
#   /Game/Professor/IK_Professor                 IK Rig of the professor
#   /Game/Professor/RTG_<Source>_To_Professor    the retargeter
#
# The assets are made once (auto characterization, auto chain mapping, auto align) and then left alone, so editor
# tweaks survive; REBUILD_RIGS=1 makes them again.
import math, os, sys, unreal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glb_util

RIGS = "/Game/Mocap/Rigs"
PROFESSOR = "/Game/Professor/Professor"
PROFESSOR_DIR = "/Game/Professor"
CAPTURY_DIR = "/Game/Mocap/Captury"   # the Captury skeleton + its recordings (MoCap Depot OSC -> .glb), see retarget_anims.py
CAPTURY_MESH = f"{CAPTURY_DIR}/SK_Captury"
CAPTURY_ANIMS = f"{CAPTURY_DIR}/Anims"
PROJECT = os.path.abspath(unreal.Paths.project_dir())
# a Captury recording with the full skeleton (body + fingers, the FBX takes only have the body), made into SK_Captury
CAPTURY_RIG_FILE = os.environ.get("CAPTURY_RIG_FILE", os.path.join(PROJECT, "Saved/Mocap/Depot/Dab.glb"))
MANNY = "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"
REBUILD = bool(os.environ.get("REBUILD_RIGS"))
eal = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()
ST = unreal.RetargetSourceOrTarget


def first_mesh(folder):
    """The first skeletal mesh below folder, or None."""
    if not eal.does_directory_exist(folder): return None
    for p in sorted(eal.list_assets(folder, recursive=True)):
        if eal.find_asset_data(p).asset_class_path.asset_name == "SkeletalMesh": return unreal.load_asset(p)
    return None


def default_source(log):
    """(name, mesh) of the skeleton the motion comes in: Captury (SK_Captury, imported if missing), else Manny."""
    if eal.does_asset_exist(CAPTURY_MESH) or os.path.isfile(CAPTURY_RIG_FILE): return "Captury", captury_rig(log)
    return "Manny", unreal.load_asset(MANNY)


def clean_glb(path):
    """Copy of a Captury .glb with clean bone names and Hips in the skin (glb_util.clean_captury), in Saved/Mocap/Clean.
    Returns the copy's path."""
    out = os.path.join(PROJECT, "Saved/Mocap/Clean", os.path.basename(path))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    glb_util.write(out, *glb_util.clean_captury(*glb_util.read(path)))
    return out


def import_source(path):
    """Import file as is (staging copy for glTF, see clean_glb); returns the file actually imported."""
    return clean_glb(path) if path.lower().endswith(".glb") else path


def _import(path, folder, options=None):
    if eal.does_directory_exist(folder): eal.delete_directory(folder)
    task = unreal.AssetImportTask()
    for k, v in (("filename", path), ("destination_path", folder), ("automated", True), ("replace_existing", True), ("save", False)):
        task.set_editor_property(k, v)
    if options: task.set_editor_property("options", options)
    tools.import_asset_tasks([task])
    return [unreal.load_asset(p) for p in eal.list_assets(folder, recursive=True)]


def captury_rig(log):
    """SK_Captury (+ SKEL_Captury, PHYS_Captury) from CAPTURY_RIG_FILE, made once; its own take goes to Anims/."""
    if eal.does_asset_exist(CAPTURY_MESH): return unreal.load_asset(CAPTURY_MESH)
    stage = f"{CAPTURY_DIR}/RigImport"
    objs = _import(import_source(CAPTURY_RIG_FILE), stage)
    mesh = next((o for o in objs if isinstance(o, unreal.SkeletalMesh)), None)
    if not mesh: raise RuntimeError(f"{CAPTURY_RIG_FILE}: no skeletal mesh imported")
    take = unreal.Paths.get_base_filename(CAPTURY_RIG_FILE)
    for o in objs:
        if isinstance(o, unreal.SkeletalMesh): dst = CAPTURY_MESH
        elif isinstance(o, unreal.Skeleton): dst = f"{CAPTURY_DIR}/SKEL_Captury"
        elif isinstance(o, unreal.PhysicsAsset): dst = f"{CAPTURY_DIR}/PHYS_Captury"
        elif isinstance(o, unreal.AnimSequence): dst = f"{CAPTURY_ANIMS}/{take}"
        else: dst = f"{CAPTURY_DIR}/Materials/{o.get_name()}"
        if eal.does_asset_exist(dst): eal.delete_asset(dst)
        if not eal.rename_asset(o.get_path_name(), dst): raise RuntimeError(f"moving {o.get_path_name()} to {dst} failed")
    eal.delete_directory(stage)
    eal.save_directory(CAPTURY_DIR, only_if_is_dirty=False, recursive=True)
    mesh = unreal.load_asset(CAPTURY_MESH)
    comp = unreal.new_object(unreal.SkeletalMeshComponent); comp.set_skeletal_mesh_asset(mesh)
    log(f"{CAPTURY_MESH}: from {os.path.basename(CAPTURY_RIG_FILE)}, {comp.get_num_bones()} bones, "
        f"root {comp.get_bone_name(0)}, skeleton {mesh.get_editor_property('skeleton').get_name()}")
    return mesh


def import_captury_anim(path, log):
    """A Captury take (.glb on the Captury skeleton: Dab.glb, or an OSC recording made into one by
    captury_osc_to_glb.py) imported as animation only onto SKEL_Captury: Anims/<file name>."""
    if not path.lower().endswith(".glb"):
        raise RuntimeError(f"{path}: Captury takes come in as .glb; pass the take's OSC recording (<Take>_OSC.zip / .osc "
                           "from the MoCap Depot), Scripts/retarget_anims.sh converts it. The Depot FBX files use different "
                           "joint axes and units per export and do not fit SKEL_Captury.")
    skel = captury_rig(log).get_editor_property("skeleton")
    name = unreal.Paths.get_base_filename(path)
    dst = f"{CAPTURY_ANIMS}/{name}"
    if os.path.abspath(path) == os.path.abspath(CAPTURY_RIG_FILE) and eal.does_asset_exist(dst):
        return unreal.load_asset(dst)  # came in with the skeleton
    p = unreal.InterchangeGenericAssetsPipeline()
    c = p.get_editor_property("common_skeletal_meshes_and_animations_properties")
    c.set_editor_property("import_only_animations", True)
    c.set_editor_property("skeleton", skel)
    stack = unreal.InterchangePipelineStackOverride(); stack.add_pipeline(p)
    stage = f"{CAPTURY_DIR}/Import"
    anims = [o for o in _import(import_source(path), stage, stack) if isinstance(o, unreal.AnimSequence)]
    if len(anims) != 1: raise RuntimeError(f"{path}: {len(anims)} animations imported onto {skel.get_name()} (want 1)")
    if anims[0].get_editor_property("skeleton") != skel: raise RuntimeError(f"{path}: not on {skel.get_name()}")
    if eal.does_asset_exist(dst): eal.delete_asset(dst)
    if not eal.rename_asset(anims[0].get_path_name(), dst): raise RuntimeError(f"moving to {dst} failed")
    eal.delete_directory(stage)
    a = unreal.load_asset(dst)
    eal.save_loaded_asset(a, False)
    log(f"{path} -> {dst} ({a.get_editor_property('sequence_length'):.1f} s)")
    return a


def _create(path, cls, factory, reuse=False):
    """A new asset at path, replacing what is there. With reuse, an asset that survives the delete (a force delete of a
    loaded, referenced one can leave its package behind) is returned as it is, for the caller to reset."""
    if eal.does_asset_exist(path) and not eal.delete_asset(path) and not reuse:
        refs = unreal.AssetRegistryHelpers.get_asset_registry().get_referencers(path.rsplit(".", 1)[0], unreal.AssetRegistryDependencyOptions())
        raise RuntimeError(f"{path} exists and cannot be deleted (referenced by {[str(r) for r in refs or []]})")
    folder, name = path.rsplit("/", 1)
    a = tools.create_asset(name, folder, cls, factory)
    if not a and reuse:
        a = unreal.load_asset(path)
        if a and not isinstance(a, cls): a = None
    if not a: raise RuntimeError(f"creating {path} failed")
    return a


def ik_rig(path, mesh, log):
    if eal.does_asset_exist(path) and not REBUILD: return unreal.load_asset(path)
    rig = _create(path, unreal.IKRigDefinition, unreal.IKRigDefinitionFactory())
    c = unreal.IKRigController.get_controller(rig)
    c.set_skeletal_mesh(mesh)
    rep = c.get_characterization_report()
    if not c.apply_auto_generated_retarget_definition():
        raise RuntimeError(f"{mesh.get_name()}: no retarget definition (template {rep.best_template_name})")
    _split_hands(c, mesh)
    c.apply_auto_fbik()
    eal.save_loaded_asset(rig, False)
    log(f"{path}: template {rep.best_template_name} ({rep.template_score:.2f}), {len(c.get_retarget_chains())} chains, "
        f"root {c.get_retarget_root()}, missing {len(rep.missing_bones)} template bones")
    return rig


def _split_hands(c, mesh):
    """LeftArm / RightArm end at the forearm, the hand is its own chain LeftHand / RightHand (with the arm's IK goal):
    the FK op can then keep only part of the wrist's motion (retargeter: hand rotation alpha)."""
    comp = unreal.new_object(unreal.SkeletalMeshComponent); comp.set_skeletal_mesh_asset(mesh)
    names = [str(x.get_editor_property("chain_name")) for x in c.get_retarget_chains()]
    for side in ("Left", "Right"):
        arm = side + "Arm"
        if arm not in names or side + "Hand" in names: continue
        hand = str(c.get_retarget_chain_end_bone(arm)); fore = str(comp.get_parent_bone(hand))
        goal = str(c.get_retarget_chain_goal(arm))
        c.set_retarget_chain_end_bone(arm, fore)
        c.set_retarget_chain_goal(arm, "None")
        c.add_retarget_chain(side + "Hand", hand, hand, goal)


def _ref_world(mesh, bone):
    APE = unreal.AnimPoseExtensions
    return APE.get_bone_pose(APE.get_reference_pose(mesh.get_editor_property("skeleton")), bone, unreal.AnimPoseSpaces.WORLD)


def _facing_yaw(src_rig, src_mesh, dst_rig, dst_mesh):
    """Degrees about Z that turn the source's reference pose to face like the target's (from the left -> right hip axis)."""
    def hips(rig, mesh):
        ch = {str(x.get_editor_property("chain_name")): str(x.get_editor_property("start_bone").get_editor_property("bone_name"))
              for x in unreal.IKRigController.get_controller(rig).get_retarget_chains()}
        v = _ref_world(mesh, ch["LeftLeg"]).translation - _ref_world(mesh, ch["RightLeg"]).translation
        return math.atan2(v.y, v.x)
    d = math.degrees(hips(dst_rig, dst_mesh) - hips(src_rig, src_mesh))
    return (d + 180) % 360 - 180


# --- posture: the target's retarget pose matched to the source's where its capture pose differs (head, neck, legs) ---
def _qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)
def _qinv(q): return (-q[0], -q[1], -q[2], q[3])
def _qrot(q, v):
    u = q[:3]; w = q[3]
    t = [2 * (u[1] * v[2] - u[2] * v[1]), 2 * (u[2] * v[0] - u[0] * v[2]), 2 * (u[0] * v[1] - u[1] * v[0])]
    return tuple(v[i] + w * t[i] + (u[(i + 1) % 3] * t[(i + 2) % 3] - u[(i + 2) % 3] * t[(i + 1) % 3]) for i in range(3))
def _sub(a, b): return tuple(x - y for x, y in zip(a, b))
def _dot(a, b): return sum(x * y for x, y in zip(a, b))
def _cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def _unit(a): n = math.sqrt(_dot(a, a)); return tuple(x / n for x in a)
def _axis_angle(axis, rad): n = _unit(axis); s = math.sin(rad / 2); return (n[0] * s, n[1] * s, n[2] * s, math.cos(rad / 2))
def _between(a, b):
    a, b = _unit(a), _unit(b); c = _cross(a, b); s = math.sqrt(_dot(c, c))
    return (0, 0, 0, 1) if s < 1e-9 else _axis_angle(c, math.atan2(s, _dot(a, b)))


class _Pose:
    """World rotation and position of every bone of a skeleton in a retarget pose (reference pose * pose offsets)."""
    def __init__(self, c, mesh, side):
        self.c, self.side = c, side
        comp = unreal.new_object(unreal.SkeletalMeshComponent); comp.set_skeletal_mesh_asset(mesh)
        self.names = [str(comp.get_bone_name(i)) for i in range(comp.get_num_bones())]   # parents first
        self.parent = {n: str(comp.get_parent_bone(n)) for n in self.names}
        ref = unreal.AnimPoseExtensions.get_reference_pose(mesh.get_editor_property("skeleton"))
        self.local = {}
        for n in self.names:
            t = unreal.AnimPoseExtensions.get_bone_pose(ref, n, unreal.AnimPoseSpaces.LOCAL)
            self.local[n] = ((t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w), (t.translation.x, t.translation.y, t.translation.z))
        self.update()

    def offsets(self):
        return {str(k): (v.x, v.y, v.z, v.w) for k, v in
                self.c.get_retarget_pose_bone_rotation_offsets(self.c.get_current_retarget_pose_name(self.side), self.side).items()}

    def update(self):
        off = self.offsets(); self.rot, self.pos = {}, {}
        for n in self.names:
            q, t = self.local[n]; q = _qmul(q, off.get(n, (0, 0, 0, 1))); p = self.parent[n]
            if p in self.rot: self.rot[n] = _qmul(self.rot[p], q); self.pos[n] = tuple(a + b for a, b in zip(self.pos[p], _qrot(self.rot[p], t)))
            else: self.rot[n], self.pos[n] = q, t

    def turn(self, bone, D):
        """Turns the bone (and so everything below it) by the world rotation D: offset' = offset * W^-1 * D * W."""
        W = self.rot[bone]; o = self.offsets().get(bone, (0, 0, 0, 1))
        d = _qmul(o, _qmul(_qinv(W), _qmul(D, W)))
        self.c.set_rotation_offset_for_retarget_pose_bone(bone, unreal.Quat(*d), self.side); self.update()

    def children(self, bone): return [n for n in self.names if self.parent[n] == bone]

    def frame(self, lhip, rhip, lfoot):
        """Body axes (lateral = to the left, forward, up) from the hips and the left foot's toes."""
        lat = _sub(self.pos[lhip], self.pos[rhip]); lat = _unit((lat[0], lat[1], 0)); up = (0, 0, 1); fwd = _cross(up, lat)
        toe = self.children(lfoot)
        if toe and _dot(_sub(self.pos[toe[0]], self.pos[lfoot]), fwd) < 0: fwd = tuple(-x for x in fwd)
        return lat, fwd, up


def _match_posture(c, src_rig, src_mesh, dst_rig, dst_mesh, log):
    """Turns the target's neck, head and legs in its retarget pose until they lean / point like the source's.

    The FK chains carry rotations over as world-space changes from the retarget pose, so whatever the target's retarget
    pose has more than the source's stays in every frame. The professor's capture pose has the head pitched 22 deg down
    and the neck 12 deg further forward than Captury's T-pose, and the toes turned 29 deg out (whole leg turned out:
    knees and shoes face outwards, which reads as bow legs). Auto align only matches limb directions, not this.
    - neck: forward lean of the Neck chain (its start to the Head chain's start bone)
    - head: forward lean of the head's up direction (Head chain bone to its end bone: Captury HeadEE, professor head_end)
    - legs: the whole leg turns about its hip-ankle line until the foot (to the toes) points like the source's"""
    def chains(rig):
        return {str(x.get_editor_property("chain_name")): (str(x.get_editor_property("start_bone").get_editor_property("bone_name")),
                                                          str(x.get_editor_property("end_bone").get_editor_property("bone_name")))
                for x in unreal.IKRigController.get_controller(rig).get_retarget_chains()}
    sc, dc = chains(src_rig), chains(dst_rig)
    S, D = _Pose(c, src_mesh, ST.SOURCE), _Pose(c, dst_mesh, ST.TARGET)
    if not all(k in sc and k in dc for k in ("LeftLeg", "RightLeg", "Neck", "Head")): log("posture: chains missing, skipped"); return
    fS = S.frame(sc["LeftLeg"][0], sc["RightLeg"][0], sc["LeftLeg"][1])
    def frameD(): return D.frame(dc["LeftLeg"][0], dc["RightLeg"][0], dc["LeftLeg"][1])
    lean = lambda v, f: math.degrees(math.atan2(_dot(v, f[1]), _dot(v, f[2])))
    done = []

    def match_lean(bone, a_d, b_d, v_s, what):
        f = frameD(); v = _sub(D.pos[b_d], D.pos[a_d]); want = lean(v_s, fS); have = lean(v, f)
        D.turn(bone, _axis_angle(f[0], math.radians(want - have) * (1 if _dot(_cross(f[2], f[1]), f[0]) > 0 else -1)))
        done.append(f"{what} {have:+.0f} -> {lean(_sub(D.pos[b_d], D.pos[a_d]), frameD()):+.0f} deg (source {want:+.0f})")

    neck_s, neck_d = sc["Neck"][0], dc["Neck"][0]; head_s, head_d = sc["Head"][1], dc["Head"][1]
    if "LeftArm" in sc and "LeftArm" in dc and "RightArm" in sc and "RightArm" in dc:
        # the head's place over the shoulders (the arm chains' starts), not the neck bone's own lean: the two
        # skeletons put the neck joint at different heights (Captury's above the shoulders, the professor's level)
        def over(P, ch):
            a, b = P.pos[ch["LeftArm"][0]], P.pos[ch["RightArm"][0]]
            return _sub(P.pos[ch["Head"][0]], tuple((x + y) / 2 for x, y in zip(a, b)))
        want = lean(over(S, sc), fS); first = lean(over(D, dc), frameD())
        for _ in range(12):  # the neck turns about its own joint, below the head and above the shoulders: iterate
            f = frameD(); have = lean(over(D, dc), f)
            if abs(want - have) < 0.1: break
            D.turn(neck_d, _axis_angle(f[0], math.radians(want - have) * (1 if _dot(_cross(f[2], f[1]), f[0]) > 0 else -1)))
        done.append(f"head over shoulders {first:+.0f} -> {lean(over(D, dc), frameD()):+.0f} deg (source {want:+.0f})")
    else:
        match_lean(neck_d, neck_d, dc["Head"][0], _sub(S.pos[sc["Head"][0]], S.pos[neck_s]), "neck lean")
    es, ed = S.children(head_s), D.children(head_d)
    if es and ed: match_lean(head_d, head_d, ed[0], _sub(S.pos[es[0]], S.pos[head_s]), "head pitch")
    else: done.append("head pitch: no head end bone")

    def foot_out(P, f, side, ch):
        foot = ch[side + "Leg"][1]; toe = P.children(foot)
        if not toe: return None
        v = _sub(P.pos[toe[0]], P.pos[foot]); out = f[0] if side == "Left" else tuple(-x for x in f[0])
        return math.degrees(math.atan2(_dot(v, out), _dot(v, f[1])))
    for side in ("Left", "Right"):
        want, have = foot_out(S, fS, side, sc), foot_out(D, frameD(), side, dc)
        if want is None or have is None: done.append(f"{side} leg: no toe bone"); continue
        hip, foot = dc[side + "Leg"][0], dc[side + "Leg"][1]; axis = _sub(D.pos[hip], D.pos[foot])
        best = None
        for sgn in (1, -1):   # which way round turns the toes in: try both, keep the closer
            o = D.offsets().get(hip, (0, 0, 0, 1)); D.turn(hip, _axis_angle(axis, sgn * math.radians(have - want)))
            got = foot_out(D, frameD(), side, dc)
            if best is None or abs(got - want) < abs(best[1] - want): best = (sgn, got)
            D.c.set_rotation_offset_for_retarget_pose_bone(hip, unreal.Quat(*o), ST.TARGET); D.update()
        D.turn(hip, _axis_angle(axis, best[0] * math.radians(have - want)))
        done.append(f"{side} toes out {have:+.0f} -> {best[1]:+.0f} deg (source {want:+.0f})")

    # forearm twist: the knuckle line (index -> pinky base) about the forearm, as the source's
    def knuckles(P, hand):
        kids = {n.lower(): n for n in P.children(hand)}
        i = next((v for k, v in kids.items() if "index" in k), None); p = next((v for k, v in kids.items() if "pinky" in k), None)
        return _sub(P.pos[i], P.pos[p]) if i and p else None
    fD = frameD()
    to_dst = lambda v: tuple(sum(c * a[j] for c, a in zip([_dot(v, x) for x in fS], fD)) for j in range(3))
    for side in ("Left", "Right"):
        hs, hd = sc[side + "Arm"][1], dc[side + "Arm"][1]; ks, kd = knuckles(S, hs), knuckles(D, hd)
        if ks is None or kd is None: done.append(f"{side} forearm: no finger bones"); continue
        fore = D.parent[hd]; axis = _unit(_sub(D.pos[hd], D.pos[fore]))
        flat = lambda v: _unit(_sub(v, tuple(axis[j] * _dot(v, axis) for j in range(3))))
        a, b = flat(kd), flat(to_dst(ks)); ang = math.atan2(_dot(axis, _cross(a, b)), _dot(a, b))
        D.turn(fore, _axis_angle(axis, ang))
        left = math.degrees(math.acos(max(-1, min(1, _dot(flat(knuckles(D, hd)), b)))))
        done.append(f"{side} forearm twist {math.degrees(ang):+.0f} deg (left {left:.0f})")
    log("posture: " + "; ".join(done))


def _chain(rig, name):
    return next((x for x in unreal.IKRigController.get_controller(rig).get_retarget_chains()
                 if str(x.get_editor_property("chain_name")) == name), None)


def _scale_neck(c, src_rig, src_mesh, dst_rig, dst_mesh):
    """Scales the Neck chain's bend (relative to the chest) by the source's neck length over the target's.

    Captury's Neck bone is short and starts above the shoulders (9 cm to the head); the professor's neck_01 sits at
    shoulder height (17 cm to the head). The same forward bend then swings the professor's head twice as far in front
    of his shoulders (Handstand start: 40 deg vs the source's 18 deg). The FK op blends the chain's local rotation
    with the retarget pose by its rotation alpha; the Head chain still copies the head's own orientation."""
    def neck(rig, mesh):
        n, h = _chain(rig, "Neck"), _chain(rig, "Head")
        if not n or not h: return None
        a = _ref_world(mesh, str(n.get_editor_property("start_bone").get_editor_property("bone_name"))).translation
        b = _ref_world(mesh, str(h.get_editor_property("start_bone").get_editor_property("bone_name"))).translation
        return (b - a).length()
    ls, ld = neck(src_rig, src_mesh), neck(dst_rig, dst_mesh)
    if not ls or not ld or ls >= ld: return None
    alpha = float(os.environ.get("NECK_ALPHA", ls / ld))
    for i in range(c.get_num_retarget_ops()):
        if "FKChainsOp" not in str(c.get_op_type_by_name(c.get_op_name(i))): continue
        oc = c.get_op_controller(i); s = oc.get_settings(); chains = list(s.get_editor_property("chains_to_retarget"))
        for k, x in enumerate(chains):
            if str(x.get_editor_property("target_chain_name")) == "Neck":
                x.set_editor_property("rotation_alpha", alpha); chains[k] = x
        s.set_editor_property("chains_to_retarget", chains); oc.set_settings(s)
        break
    else:
        return None
    # the Head chain is carried over relative to the neck, so it would lose the bend taken off the neck: pin the head's
    # world rotation to the source head's (keeping their offset in the retarget poses); its children follow
    hs, hd = (str(_chain(r, "Head").get_editor_property("start_bone").get_editor_property("bone_name")) for r in (src_rig, dst_rig))
    i = c.add_retarget_op("/Script/IKRig.IKRetargetPinBoneOp")
    oc = c.get_op_controller(i); s = oc.get_settings()
    for k, v in (("skeleton_to_copy_from", ST.SOURCE), ("copy_translation", False), ("copy_rotation", True), ("copy_scale", False),
                 ("rotation_mode", unreal.PinBoneRotationMode.MAINTAIN_OFFSET_FROM_BONE_TO_COPY_FROM), ("propagate_to_children", True)):
        s.set_editor_property(k, v)
    oc.set_settings(s); oc.set_bone_pair(hs, hd)
    return f"neck bend x{alpha:.2f} (neck {ls:.0f} vs {ld:.0f} cm), {hd} rotation pinned to the source {hs}"


# --- the professor's retargeter: Auto-Rig-Pro style (matched rest poses + world rotation copy) with stock ops ---
def _qmat(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
def _matq(m):
    t = m[0][0] + m[1][1] + m[2][2]
    if t > 0:
        r = math.sqrt(t + 1) * 2; return ((m[2][1] - m[1][2]) / r, (m[0][2] - m[2][0]) / r, (m[1][0] - m[0][1]) / r, r / 4)
    i = max(range(3), key=lambda k: m[k][k]); j, k = (i + 1) % 3, (i + 2) % 3
    r = math.sqrt(1 + m[i][i] - m[j][j] - m[k][k]) * 2; q = [0.0] * 4
    q[i] = r / 4; q[j] = (m[j][i] + m[i][j]) / r; q[k] = (m[k][i] + m[i][k]) / r; q[3] = (m[k][j] - m[j][k]) / r
    return tuple(q)
def _fit2(a_from, b_from, a_to, b_to):
    """Rotation (quat) turning direction a_from onto a_to exactly and b_from (made perpendicular) onto b_to's plane."""
    def frame(a, b):
        a = _unit(a); b = _unit(_sub(b, tuple(x * _dot(b, a) for x in a))); return a, b, _cross(a, b)
    F, T = frame(a_from, b_from), frame(a_to, b_to)
    m = [[sum(T[k][i] * F[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return _matq(m)
def _yaw(v): return math.atan2(v[1], v[0])


def _professor_rest_match(c, src_rig, src_mesh, dst_rig, dst_mesh, log):
    """The professor's retarget pose matched to the source's T-pose like Auto-Rig-Pro's rest pose match:
    - torso, collarbones, neck, head: no offsets. He stands upright in his capture pose like the actor in the T-pose;
      his auto-placed spine joints lean inside the body and his collarbones rise 25 deg where Captury's drop, so
      matching those bone directions would tip him back and drop his shoulders.
    - upper arms, forearms, thighs, shins: bone direction onto the source's, rolled so the elbow / knee hinge axis
      lands on the source's (vertical for T-pose arms, sideways for legs): a bend then stays a bend, no twist.
    - hands: middle finger direction and knuckle line (index -> pinky) onto the source's.
    - feet: yaw only, the toes point like the source's and the sole stays flat."""
    ch = lambda rig: {str(x.get_editor_property("chain_name")): (str(x.get_editor_property("start_bone").get_editor_property("bone_name")),
                                                                 str(x.get_editor_property("end_bone").get_editor_property("bone_name")))
                      for x in unreal.IKRigController.get_controller(rig).get_retarget_chains()}
    sc, dc = ch(src_rig), ch(dst_rig)
    pose = c.get_current_retarget_pose_name(ST.TARGET)
    for k in list(c.get_retarget_pose_bone_rotation_offsets(pose, ST.TARGET).keys()):
        c.set_rotation_offset_for_retarget_pose_bone(str(k), unreal.Quat(0, 0, 0, 1), ST.TARGET)
    S, D = _Pose(c, src_mesh, ST.SOURCE), _Pose(c, dst_mesh, ST.TARGET)
    fS = S.frame(sc["LeftLeg"][0], sc["RightLeg"][0], sc["LeftLeg"][1]); fD = D.frame(dc["LeftLeg"][0], dc["RightLeg"][0], dc["LeftLeg"][1])
    ref = {n: D.rot[n] for n in D.names}   # reference (capture) pose, world
    done = []

    def set_world(bone, q_fit):   # world rotation = q_fit * reference world rotation
        W = _qmul(q_fit, ref[bone]); D.turn(bone, _qmul(W, _qinv(D.rot[bone])))

    def finger(P, hand, key):
        return next((n for n in P.children(hand) if key in n.lower()), None)

    for side in ("Left", "Right"):
        for limb, hinge_of in ((side + "Arm", "arm"), (side + "Leg", "leg")):
            a_s, e_s = sc[limb]; a_d, e_d = dc[limb]
            # chain bones (start .. end) and the next bone (the end's child in the chain direction)
            def bones(P, a, e):
                out = [e]
                while out[-1] != a: out.append(P.parent[out[-1]])
                return out[::-1]
            bs, bd = bones(S, a_s, e_s), bones(D, a_d, e_d)
            if hinge_of == "arm": bs.append(S.children(e_s)[0] if S.children(e_s) else e_s); bd.append(dc[side + "Hand"][0])
            if len(bs) != len(bd) or len(bd) < 3: done.append(f"{limb}: {len(bs)} vs {len(bd)} bones, skipped"); continue
            # the target's hinge axis in its capture pose: arms from their own bend, legs sideways (knee bends back)
            u0, l0 = _sub(D.pos[bd[1]], D.pos[bd[0]]), _sub(D.pos[bd[2]], D.pos[bd[1]])
            h_t = _cross(u0, l0) if hinge_of == "arm" else _cross(u0, tuple(-x for x in fD[1]))
            fwd_t = fD[1] if hinge_of == "arm" else tuple(-x for x in fD[1])
            sgn = 1 if _dot(h_t, _cross(u0, fwd_t)) >= 0 else -1
            for i in range(len(bd) - 1):
                b, nb = bd[i], bd[i + 1]
                v_t = _sub(D.pos[nb], D.pos[b]); D_ref_dir = _qrot(_qmul(ref[b], _qinv(D.rot[b])), v_t)   # in the capture pose
                v_s = _sub(S.pos[bs[i + 1]], S.pos[bs[i]])
                fwd_s = fS[1] if hinge_of == "arm" else tuple(-x for x in fS[1])
                h_s = tuple(sgn * x for x in _cross(v_s, fwd_s))
                set_world(b, _fit2(D_ref_dir, h_t, v_s, h_s))
            done.append(f"{limb} matched (dir + hinge)")
        # hand: middle finger + knuckle line
        hs, hd = sc[side + "Hand"][0] if side + "Hand" in sc else sc[side + "Arm"][1], dc[side + "Hand"][0]
        ms, md = finger(S, hs, "middle"), finger(D, hd, "middle"); i_s, i_d = finger(S, hs, "index"), finger(D, hd, "index")
        p_s, p_d = finger(S, hs, "pinky"), finger(D, hd, "pinky")
        if all((ms, md, i_s, i_d, p_s, p_d)):
            Wd = _qmul(ref[hd], _qinv(D.rot[hd]))   # current -> capture pose, for the target's hand vectors
            fm, fk = _qrot(Wd, _sub(D.pos[md], D.pos[hd])), _qrot(Wd, _sub(D.pos[i_d], D.pos[p_d]))
            set_world(hd, _fit2(fm, fk, _sub(S.pos[ms], S.pos[hs]), _sub(S.pos[i_s], S.pos[p_s])))
            done.append(f"{side} hand matched (fingers)")
        # foot: yaw only, absolute (flat sole)
        f_s, f_d = sc[side + "Leg"][1], dc[side + "Leg"][1]
        t_s, t_d = S.children(f_s), D.children(f_d)
        if t_s and t_d:
            Wd = _qmul(ref[f_d], _qinv(D.rot[f_d]))
            phi = _yaw(_sub(S.pos[t_s[0]], S.pos[f_s])) - _yaw(_qrot(Wd, _sub(D.pos[t_d[0]], D.pos[f_d])))
            set_world(f_d, _axis_angle((0, 0, 1), phi))
            done.append(f"{side} foot yaw {math.degrees((phi + math.pi) % (2 * math.pi) - math.pi):+.0f} deg")
    # head: Captury carries the head ~10 deg further forward than the actors held it (the offline retarget took that
    # off per take as the median bend); FK One to One keeps a retarget pose offset in every frame, so the neck leans
    # back by the takes' mean bias here, live too
    nk = _chain(dst_rig, "Neck")
    if nk and PROFESSOR_HEAD_BACK:
        n = str(nk.get_editor_property("start_bone").get_editor_property("bone_name"))
        D.turn(n, _axis_angle(_cross(fD[1], fD[2]), math.radians(PROFESSOR_HEAD_BACK)))   # (fwd x up) x up = -fwd: up -> back
        done.append(f"{n} {PROFESSOR_HEAD_BACK:.0f} deg back")
    log("rest match: " + "; ".join(done))


# FK rotation alpha per target chain (share of the motion against the parent kept); chains not listed: 1
PROFESSOR_ALPHA = {"Neck": 0.25, "Head": 0.5, "LeftClavicle": 0.5, "RightClavicle": 0.5, "LeftHand": 0.6, "RightHand": 0.6}
PROFESSOR_KNEE_OUT = float(os.environ.get("KNEE_OUT", 6.0))   # deg, knees turned out against the source's (Captury leans in)
# Align Pole Vector's static angular offset that reproduces the plain FK knee (measured on the 15 Depot takes against
# the op switched off): at 0 the op turns both knees ~95 deg about the leg (bow legs); more = left knee out, right in
PROFESSOR_POLE_ZERO = {"LeftLeg": -87.0, "RightLeg": -101.0}
PROFESSOR_HEAD_BACK = float(os.environ.get("HEAD_BACK", 9.0))   # deg, neck leaned back in the retarget pose (Captury head bias)


def _professor_ops(c, src_rig, dst_rig, dst_mesh, log):
    """The op stack for the professor (stock ops only):
      Pelvis Motion -> Filter Bones (pelvis, ~1.2 Hz: per-step wobble damped, the turn kept; before FK so the body
      does not inherit its lag) -> FK Chains (One to One world rotation copy; neck / head / collarbones / hands keep
      part of their motion) -> Pole Vector Alignment (knees: the source's bend plane, turned out) -> Filter Bones
      (neck, head: Captury's head jitter) -> IK Chains + Floor Constraint -> Run IK Rig (legs only: the feet go where
      the source's go, on the floor) -> Root Motion -> Remap Curves"""
    ctl = lambda name: c.get_op_controller(c.get_index_of_op_by_name(name))
    names = lambda: [str(c.get_op_name(i)) for i in range(c.get_num_retarget_ops())]
    typ = lambda i: str(c.get_op_type_by_name(c.get_op_name(i)))
    first = lambda t: next((str(c.get_op_name(i)) for i in range(c.get_num_retarget_ops()) if t in typ(i)), None)
    legs = ("LeftLeg", "RightLeg")
    pelvis = str(unreal.IKRigController.get_controller(dst_rig).get_retarget_root())

    fk = first("FKChainsOp"); oc = ctl(fk); s = oc.get_settings(); chains = list(s.get_editor_property("chains_to_retarget"))
    for k, x in enumerate(chains):
        n = str(x.get_editor_property("target_chain_name"))
        x.set_editor_property("rotation_mode", unreal.FKChainRotationMode.ONE_TO_ONE)
        x.set_editor_property("rotation_alpha", PROFESSOR_ALPHA.get(n, 1.0))
        if n in ("LeftFoot", "RightFoot"): x.set_editor_property("enable_fk", False)   # toes rigid with the foot
        chains[k] = x
    s.set_editor_property("chains_to_retarget", chains); oc.set_settings(s)

    def add(t):
        return str(c.get_op_name(c.add_retarget_op(t)))
    # pelvis filter, right after Pelvis Motion
    pf = add("/Script/IKRig.IKRetargetFilterBoneOp")
    oc = ctl(pf); oc.add_bone_to_filter(pelvis); s = oc.get_settings()
    fs = s.get_editor_property("filter_settings"); fs.set_editor_property("cutoff_frequency", 1.2); fs.set_editor_property("responsiveness", 0.3)
    s.set_editor_property("filter_settings", fs); oc.set_settings(s)
    c.move_retarget_op_in_stack(c.get_index_of_op_by_name(pf), 1)
    # pole vectors: legs only, offset calibrated to the FK knee and then turned out (the offline retarget's >= 8 deg
    # knee-out per take; a twist in the retarget pose would only roll the bones, the knee stays where FK puts it)
    pv = add("/Script/IKRig.IKRetargetAlignPoleVectorOp"); oc = ctl(pv); s = oc.get_settings()
    al = list(s.get_editor_property("chains_to_align"))
    for k, x in enumerate(al):
        n = str(x.get_editor_property("target_chain_name"))
        x.set_editor_property("enabled", n in legs)
        if n in legs: x.set_editor_property("static_angular_offset", PROFESSOR_POLE_ZERO[n] + PROFESSOR_KNEE_OUT * (1 if n == "LeftLeg" else -1))
        al[k] = x
    s.set_editor_property("chains_to_align", al); oc.set_settings(s)
    c.move_retarget_op_in_stack(c.get_index_of_op_by_name(pv), c.get_index_of_op_by_name(fk) + 1)
    # neck / head filter
    hf = add("/Script/IKRig.IKRetargetFilterBoneOp"); oc = ctl(hf)
    for ch_ in ("Neck", "Head"):
        x = _chain(dst_rig, ch_)
        if x: oc.add_bone_to_filter(str(x.get_editor_property("start_bone").get_editor_property("bone_name")))
    s = oc.get_settings(); fs = s.get_editor_property("filter_settings"); fs.set_editor_property("cutoff_frequency", 2.0)
    s.set_editor_property("filter_settings", fs); oc.set_settings(s)
    c.move_retarget_op_in_stack(c.get_index_of_op_by_name(hf), c.get_index_of_op_by_name(pv) + 1)
    # leg IK: IK Chains + Floor Constraint under Run IK Rig, legs only
    run = first("RunIKRigOp")
    if run:
        ik = add("/Script/IKRig.IKRetargetIKChainsOp"); c.assign_ik_rig_to_all_ops(ST.TARGET, dst_rig)
        c.set_parent_op_by_name(ik, run)
        oc = ctl(ik); s = oc.get_settings(); ic = list(s.get_editor_property("chains_to_retarget"))
        for k, x in enumerate(ic):
            x.set_editor_property("enable_ik", str(x.get_editor_property("target_chain_name")) in legs); ic[k] = x
        s.set_editor_property("chains_to_retarget", ic); oc.set_settings(s)
        fl = first("FloorConstraintOp") or add("/Script/IKRig.IKRetargetFloorConstraintOp")
        c.set_parent_op_by_name(fl, run)
        oc = ctl(fl); s = oc.get_settings(); fc = list(s.get_editor_property("chains_to_affect"))
        for k, x in enumerate(fc):
            x.set_editor_property("enable_floor_constraint", str(x.get_editor_property("target_chain_name")) in legs); fc[k] = x
        s.set_editor_property("chains_to_affect", fc); oc.set_settings(s)
        oc = ctl(run); s = oc.get_settings(); rc_ = [x for x in s.get_editor_property("chains") if str(x.get_editor_property("target_chain_name")) in legs]
        s.set_editor_property("chains", rc_); oc.set_settings(s)
    log("ops: " + " -> ".join(f"{n}" + (f" [{c.get_parent_op_by_name(n)}]" if str(c.get_parent_op_by_name(n)) not in ("None", "") else "") for n in names()))


def _root_motion(c, src_rig, log):
    # Root Motion writes the source root's motion onto the target root bone; a skeleton without a root bone above the
    # pelvis gets the pelvis there, and the op then overwrites the pelvis rotation every frame (frozen hips). The
    # Pelvis Motion op moves it already.
    off = []
    for i in range(c.get_num_retarget_ops()):
        n = c.get_op_name(i)
        if "RootMotionOp" in str(c.get_op_type_by_name(n)):
            oc = c.get_op_controller(i)
            if str(oc.get_target_root_bone()) == str(oc.get_target_pelvis_bone()):
                c.set_retarget_op_enabled(i, False); off.append(str(n))
            elif str(oc.get_source_root_bone()) == str(unreal.IKRigController.get_controller(src_rig).get_retarget_root()):
                # the source has no root bone (Captury: Hips is the top bone): copying its "root" would put the
                # pelvis' height and turns on the target's root bone; make the root motion from the target pelvis
                try:
                    s = oc.get_settings()
                    s.set_editor_property("root_motion_source", unreal.RootMotionSource.GENERATE_FROM_TARGET_PELVIS)
                    s.set_editor_property("root_height_source", unreal.RootMotionHeightSource.SNAP_TO_GROUND)
                    oc.set_settings(s); off.append(f"{n}: root motion from target pelvis")
                except Exception as e:
                    c.set_retarget_op_enabled(i, False); off.append(f"{n} ({e})")
    return off


def retargeter(path, src_rig, src_mesh, dst_rig, dst_mesh, log, professor=False):
    if eal.does_asset_exist(path) and not REBUILD: return unreal.load_asset(path)
    rtg = _create(path, unreal.IKRetargeter, unreal.IKRetargetFactory(), reuse=True)
    c = unreal.IKRetargeterController.get_controller(rtg)
    c.remove_all_ops()  # (a reused retargeter starts over)
    c.set_ik_rig(ST.SOURCE, src_rig); c.set_ik_rig(ST.TARGET, dst_rig)
    c.set_preview_mesh(ST.SOURCE, src_mesh); c.set_preview_mesh(ST.TARGET, dst_mesh)
    c.add_default_ops()
    c.assign_ik_rig_to_all_ops(ST.SOURCE, src_rig); c.assign_ik_rig_to_all_ops(ST.TARGET, dst_rig)
    c.auto_map_chains(unreal.AutoMapChainType.FUZZY, True)
    # Both retarget poses must face the same way: the retargeter carries rotations over as world-space deltas, so a
    # source turned 90 deg makes a forward bend a sideways one. Captury's T-pose faces wherever the actor stood at
    # calibration (SK_Captury: ~107 deg off the professor's +Y). The source's retarget root is turned about the vertical
    # by the angle between the two hip axes (the leg chains' start bones).
    yaw = _facing_yaw(src_rig, src_mesh, dst_rig, dst_mesh)
    if abs(yaw) > 1:
        root = str(unreal.IKRigController.get_controller(src_rig).get_retarget_root())
        R = _ref_world(src_mesh, root).rotation; h = math.radians(yaw) / 2
        c.set_rotation_offset_for_retarget_pose_bone(root, R.inversed() * unreal.Quat(0, 0, math.sin(h), math.cos(h)) * R, ST.SOURCE)
    if professor:
        _professor_rest_match(c, src_rig, src_mesh, dst_rig, dst_mesh, log)
        _professor_ops(c, src_rig, dst_rig, dst_mesh, log)
        _root_motion(c, src_rig, log)
        eal.save_loaded_asset(rtg, False)
        log(f"{path}: {c.get_num_retarget_ops()} ops, source turned {yaw:.0f} deg to face the target")
        return rtg
    c.auto_align_all_bones(ST.TARGET)  # match the target's retarget pose to the source's (T-pose vs A-pose)
    # ...but only for the limbs: on skeletons whose bones point along Y (Mixamo, Captury) auto align puts big rolls on
    # hips and spine (the professor got -14/+23/-54 deg), which tilts the torso sideways. Both stand upright in their
    # ref pose, so the torso (the head's ancestors) keeps no offset.
    comp = unreal.new_object(unreal.SkeletalMeshComponent); comp.set_skeletal_mesh_asset(dst_mesh)
    head = next(ch for ch in unreal.IKRigController.get_controller(dst_rig).get_retarget_chains()
                if str(ch.get_editor_property("chain_name")) == "Head")
    torso, b = set(), str(head.get_editor_property("end_bone").get_editor_property("bone_name"))
    while b != "None": torso.add(b); b = str(comp.get_parent_bone(b))
    pose = c.get_current_retarget_pose_name(ST.TARGET)
    reset = [str(k) for k in c.get_retarget_pose_bone_rotation_offsets(pose, ST.TARGET).keys() if str(k) in torso]
    for k in reset: c.set_rotation_offset_for_retarget_pose_bone(k, unreal.Quat(0, 0, 0, 1), ST.TARGET)
    _match_posture(c, src_rig, src_mesh, dst_rig, dst_mesh, log)
    neck = _scale_neck(c, src_rig, src_mesh, dst_rig, dst_mesh)
    off = _root_motion(c, src_rig, log)
    eal.save_loaded_asset(rtg, False)
    log(f"{path}: {c.get_num_retarget_ops()} ops, source turned {yaw:.0f} deg to face the target" + (f", root motion op: {off}" if off else "")
        + (f", torso offsets reset {reset}" if reset else "") + (f", {neck}" if neck else ""))
    return rtg


def professor_retargeter(src_name, src_mesh, log):
    """The professor's retargeter for a source skeleton (made if missing): (retargeter, professor data, professor mesh)."""
    data = unreal.load_asset(PROFESSOR)
    if not data: raise RuntimeError(f"{PROFESSOR} missing (Scripts/import_professor.sh)")
    mesh = data.get_editor_property("mesh")
    rtg_path = f"{PROFESSOR_DIR}/RTG_{src_name}_To_Professor"
    # the retargeter references both IK rigs: delete it first, or rebuilding them cannot delete the old ones
    # (if it survives the delete, retargeter() reuses and resets it)
    if REBUILD and eal.does_asset_exist(rtg_path): eal.delete_asset(rtg_path)
    src_rig = ik_rig(f"{RIGS}/IK_{src_name}", src_mesh, log)
    dst_rig = ik_rig(f"{PROFESSOR_DIR}/IK_Professor", mesh, log)
    rtg = retargeter(rtg_path, src_rig, src_mesh, dst_rig, mesh, log, professor=True)
    return rtg, data, mesh
