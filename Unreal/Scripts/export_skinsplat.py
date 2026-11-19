#!/usr/bin/env python3
"""Rigged splat (.npz from the professor rig pipeline) + clips (K_*.npy) -> standard files for Unreal:

  <out>.ply  3DGS PLY (INRIA layout: x y z nx ny nz f_dc_* opacity scale_* rot_*), COLMAP space (X right, Y down,
             Z forward), metres, so Epic's GaussianSplat importer and every splat viewer read it as is. Skinning is added
             as extra float vertex properties bone_0..3 / weight_0..3 (ignored by other readers) and one
             "comment bone <index> <name> <parent> <x> <y> <z>" line per bone (capture-pose joint, PLY space).
  <out>.glb  glTF 2.0 skeleton (rest pose = capture pose), one skinned mesh of octahedral bone proxies (the editor's
             bone display) and one animation per clip. Unreal imports it as Skeleton + SkeletalMesh + AnimSequences.
             The splats cast their shadow themselves (GaussianSplat's splat shadow map), no stand-in body is needed.

Both files share one space: feet on the floor, hips over the origin, scaled to --height. The splats are in the
capture pose, so a splat moves by  bone_animated * bone_rest^-1  (dual-quaternion blended with its weights).

Rig conventions: gs holds capture-pose splats [xyz, opacity, scale3, quat wxyz, rgb], pose[b] maps capture -> rest,
K[f, b] maps rest -> animated, rig space is right-handed Y up with the character facing +Z (= glTF convention).

Usage: export_skinsplat.py rig.npz out/Professor [--clips DIR ... | --clip name=K.npy ...] [--height 1.79] [--fps 30]
"""
import argparse, glob, json, os, struct
import numpy as np
from scipy.spatial.transform import Rotation

SH_C0 = 0.28209479177387814
FLIP = np.diag([-1.0, -1.0, 1.0])   # glTF (Y up, front +Z) -> COLMAP (Y down, front +Z): 180 deg about Z, no mirror


def morton_order(p):
    q = ((p - p.min(0)) / max(np.ptp(p, 0).max(), 1e-9) * 1023).astype(np.uint64)
    def spread(v):
        v = (v | (v << 16)) & 0x030000FF; v = (v | (v << 8)) & 0x0300F00F
        v = (v | (v << 4)) & 0x030C30C3; return (v | (v << 2)) & 0x09249249
    return np.argsort(spread(q[:, 0]) | (spread(q[:, 1]) << 1) | (spread(q[:, 2]) << 2), kind='stable')


def write_ply(path, g, idx, w, joints_ply, names, parents):
    n = len(g['pos'])
    props = ['x', 'y', 'z', 'nx', 'ny', 'nz', 'f_dc_0', 'f_dc_1', 'f_dc_2', 'opacity',
             'scale_0', 'scale_1', 'scale_2', 'rot_0', 'rot_1', 'rot_2', 'rot_3',
             'bone_0', 'bone_1', 'bone_2', 'bone_3', 'weight_0', 'weight_1', 'weight_2', 'weight_3']
    data = np.zeros((n, len(props)), '<f4')
    data[:, 0:3] = g['pos']; data[:, 6:9] = g['dc']; data[:, 9] = g['opacity_logit']
    data[:, 10:13] = g['log_scale']; data[:, 13:17] = g['quat_wxyz']
    data[:, 17:21] = idx; data[:, 21:25] = w
    head = ['ply', 'format binary_little_endian 1.0', 'comment skinned gaussian splat (WinterSchool skinned splat), capture pose',
            f'comment bones {len(names)}']
    head += [f'comment bone {b} {names[b]} {parents[b]} {joints_ply[b][0]:.6f} {joints_ply[b][1]:.6f} {joints_ply[b][2]:.6f}'
             for b in range(len(names))]
    head += [f'element vertex {n}'] + [f'property float {p}' for p in props] + ['end_header']
    with open(path, 'wb') as f:
        f.write(('\n'.join(head) + '\n').encode()); f.write(data.tobytes())


class Glb:
    def __init__(self):
        self.bin = bytearray(); self.views = []; self.accessors = []

    def add(self, arr, comp, typ, target=None, minmax=False):
        arr = np.ascontiguousarray(arr)
        while len(self.bin) % 4: self.bin.append(0)
        view = dict(buffer=0, byteOffset=len(self.bin), byteLength=arr.nbytes)
        if target: view['target'] = target
        self.bin += arr.tobytes(); self.views.append(view)
        acc = dict(bufferView=len(self.views) - 1, componentType=comp, count=len(arr), type=typ)
        if minmax:
            flat = arr.reshape(len(arr), -1)
            acc['min'] = flat.min(0).tolist(); acc['max'] = flat.max(0).tolist()
        self.accessors.append(acc); return len(self.accessors) - 1

    def write(self, path, doc):
        doc['buffers'] = [dict(byteLength=len(self.bin))]; doc['bufferViews'] = self.views; doc['accessors'] = self.accessors
        js = json.dumps(doc, separators=(',', ':')).encode(); js += b' ' * (-len(js) % 4)
        bn = bytes(self.bin) + b'\0' * (-len(self.bin) % 4)
        with open(path, 'wb') as f:
            f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bn)))
            f.write(struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(bn), 0x004E4942) + bn)


def local_trs(G, parents):
    """global rigid frames (..., B, 4, 4) -> local translation (..., B, 3), rotation xyzw (..., B, 4)."""
    L = G.copy()
    for b, p in enumerate(parents):
        if p >= 0: L[..., b, :, :] = np.linalg.inv(G[..., p, :, :]) @ G[..., b, :, :]
    q = Rotation.from_matrix(L[..., :3, :3].reshape(-1, 3, 3)).as_quat().reshape(L.shape[:-2] + (4,))
    return L[..., :3, 3], q


# UE5 Mannequin (Manny / Quinn) bone names for the Mixamo skeleton the rig comes with, so the professor works with
# Unreal's IK Rig templates, retargeters and Manny animations like any UE5 character
MANNEQUIN = dict(Hips='pelvis', Spine='spine_01', Spine1='spine_02', Spine2='spine_03', Neck='neck_01', Head='head')
for _s, _x in (('Left', 'l'), ('Right', 'r')):
    MANNEQUIN.update({f'{_s}Shoulder': f'clavicle_{_x}', f'{_s}Arm': f'upperarm_{_x}', f'{_s}ForeArm': f'lowerarm_{_x}',
                      f'{_s}Hand': f'hand_{_x}', f'{_s}UpLeg': f'thigh_{_x}', f'{_s}Leg': f'calf_{_x}',
                      f'{_s}Foot': f'foot_{_x}', f'{_s}ToeBase': f'ball_{_x}'})
    for _f in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky'):
        MANNEQUIN.update({f'{_s}Hand{_f}{_i}': f'{_f.lower()}_0{_i}_{_x}' for _i in (1, 2, 3)})


def write_glb(path, names, parents, Ref, clips, fps, root_name='root'):
    """glTF skeleton + octahedral bone proxy mesh + clips. A static root_name joint at the origin (feet level, UE's
    root bone) parents the skeleton's root; it is GLB only, the .ply has the skinning bones."""
    nb = len(names); G = Glb()
    t0, q0 = local_trs(Ref, parents)
    nodes = [dict(name=names[b], translation=t0[b].tolist(), rotation=q0[b].tolist()) for b in range(nb)]
    for b, p in enumerate(parents):
        if p >= 0: nodes[p].setdefault('children', []).append(b)
    nodes.append(dict(name=root_name, children=[int(np.where(parents < 0)[0][0])]))
    root = nb  # node index of the root joint, the proxy mesh node follows it

    # proxy mesh: Blender style octahedral bones from every joint to each child joint (leaves point on along the
    # parent direction), each rigidly bound to its joint; flat shaded, so 3 own vertices per face
    head = Ref[:, :3, 3]; segs = []
    for b in range(nb):
        kids = [c for c in range(nb) if parents[c] == b]
        for c in kids: segs.append((b, head[b], head[c]))
        if not kids:
            p = parents[b]; d = head[b] - head[p] if p >= 0 else np.array([0, 0.1, 0])
            n = np.linalg.norm(d); segs.append((b, head[b], head[b] + d / max(n, 1e-9) * min(max(0.5 * n, 0.03), 0.1)))
    P, N, J = [], [], []
    for b, h, t in segs:
        d = t - h; L = np.linalg.norm(d)
        if L < 1e-5: continue
        ax = d / L; ref = Ref[b, :3, 0] if abs(Ref[b, :3, 0] @ ax) < 0.9 else Ref[b, :3, 2]
        u = ref - (ref @ ax) * ax; u /= np.linalg.norm(u); v = np.cross(ax, u); r = 0.1 * L; m = h + 0.1 * d
        ring = [m + r * u, m + r * v, m - r * u, m - r * v]
        for i in range(4):
            a, c = ring[i], ring[(i + 1) % 4]
            for tri in ((h, c, a), (t, a, c)):
                n = np.cross(tri[1] - tri[0], tri[2] - tri[0]); n /= np.linalg.norm(n)
                P += tri; N += [n] * 3; J += [b] * 3
    pos = np.array(P, '<f4'); nrm = np.array(N, '<f4')
    joints = np.zeros((len(J), 4), '<u1'); joints[:, 0] = J
    weights = np.zeros((len(J), 4), '<f4'); weights[:, 0] = 1
    prim = dict(attributes=dict(POSITION=G.add(pos, 5126, 'VEC3', 34962, True), NORMAL=G.add(nrm, 5126, 'VEC3', 34962),
                                JOINTS_0=G.add(joints, 5121, 'VEC4', 34962), WEIGHTS_0=G.add(weights, 5126, 'VEC4', 34962)),
                indices=G.add(np.arange(len(J), dtype='<u2'), 5123, 'SCALAR', 34963), material=0)
    ibm = np.concatenate([np.linalg.inv(Ref), np.eye(4)[None]])  # skinning bones, then the root joint
    ibm = G.add(ibm.transpose(0, 2, 1).astype('<f4').reshape(nb + 1, 16), 5126, 'MAT4')
    nodes.append(dict(name='SplatProxy', mesh=0, skin=0))
    doc = dict(asset=dict(version='2.0', generator='WinterSchool export_skinsplat.py'),
               scene=0, scenes=[dict(nodes=[root, nb + 1])], nodes=nodes,
               meshes=[dict(name='SplatProxy', primitives=[prim])],
               skins=[dict(name='Skeleton', joints=list(range(nb + 1)), skeleton=root, inverseBindMatrices=ibm)],
               materials=[dict(name='Bone', pbrMetallicRoughness=dict(baseColorFactor=[0.75, 0.75, 0.78, 1], metallicFactor=0, roughnessFactor=0.6))], animations=[])
    for name, A in clips:
        t, q = local_trs(A, parents)
        for f in range(1, len(q)):  # keep quaternions on one hemisphere for interpolation
            q[f] *= np.where((q[f] * q[f - 1]).sum(-1, keepdims=True) < 0, -1, 1)
        times = G.add((np.arange(len(A)) / fps).astype('<f4'), 5126, 'SCALAR', minmax=True)
        samplers, channels = [], []
        for b in range(nb):
            for path_, val, typ in (('translation', t[:, b], 'VEC3'), ('rotation', q[:, b], 'VEC4')):
                samplers.append(dict(input=times, output=G.add(val.astype('<f4'), 5126, typ), interpolation='LINEAR'))
                channels.append(dict(sampler=len(samplers) - 1, target=dict(node=b, path=path_)))
        doc['animations'].append(dict(name=name, samplers=samplers, channels=channels))
    G.write(path, doc)


def centre_leg_joints(pos, W, J, names, parents, log=print):
    """Capture-pose joints J with the hip and ankle joints moved sideways onto their leg's splats.

    The professor's auto-placed rig has bowed legs: hips 14 rig units apart (thigh splats centred ~25 apart), knees
    outside, ankles ~3 cm inside the shin splats. Splats don't care (they move by bone_animated * bone_rest^-1), but a
    retargeter turns bones about these pivots: narrow hips give a narrow stance (feet cross) and a bowed hip-knee-ankle
    line shows as bow legs. Only the sideways coordinate (along the hip axis) moves, symmetric about the hips' midpoint:
    hip to the lateral centre of its thigh splats (weight > 0.5) at 30-50% of the thigh, ankle to that of its shin splats
    at 30-70% of the shin (3rd-97th percentile midpoint per slice: left and right of a limb are both captured, unlike
    front and back). Knees, heights and depths stay; joints below an ankle move with it."""
    J = J.copy(); old = J.copy(); idx = {n: i for i, n in enumerate(names)}
    need = [f'{s}{b}' for s in ('Left', 'Right') for b in ('UpLeg', 'Leg', 'Foot')]
    if not all(n in idx for n in need): log('  legs not found, joints kept'); return J
    L, R = J[idx['LeftUpLeg']], J[idx['RightUpLeg']]; mid = (L + R) / 2; lat = (L - R) / np.linalg.norm(L - R)

    def centre(bone, child, fracs):
        a, b = J[idx[bone]], J[idx[child]]; u = b - a; n = np.linalg.norm(u); u /= n
        sel = W[:, idx[bone]] > 0.5; s = (pos[sel] - a) @ u / n; x = (pos[sel] - mid) @ lat; c = []
        for f in fracs:
            m = np.abs(s - f) < 0.05
            if m.sum() >= 30: lo, hi = np.percentile(x[m], [3, 97]); c.append((lo + hi) / 2)
        return np.mean(c) if c else None

    for joint, bone, child, fracs in (('UpLeg', 'UpLeg', 'Leg', (0.3, 0.4, 0.5)), ('Foot', 'Leg', 'Foot', (0.3, 0.5, 0.7))):
        cl, cr = centre('Left' + bone, 'Left' + child, fracs), centre('Right' + bone, 'Right' + child, fracs)
        if cl is None or cr is None: log(f'  {joint}: too few splats, kept'); continue
        w = (abs(cl) + abs(cr)) / 2
        for side, sign in (('Left', 1), ('Right', -1)):
            i = idx[side + joint]; J[i] += (sign * w - (J[i] - mid) @ lat) * lat
            log(f'  {side}{joint}: {(old[i] - mid) @ lat:+.3f} -> {sign * w:+.3f} sideways (rig units)')
    for i in range(len(names)):   # toes etc. follow their ankle
        a = parents[i]
        while a >= 0 and not names[a].endswith('Foot'): a = parents[a]
        if a >= 0 and not names[i].endswith('Foot'): J[i] = old[i] + (J[a] - old[a])
    return J


def rig_joints(d, names, ankle_height=0.075, height=1.79, min_opacity=0.004, log=print):
    """The rig's capture-pose joints as the professor uses them, as the retargeter sees them:
    hips and ankles centred on the legs (centre_leg_joints), then the ankles raised off the sole onto the calf line,
    ankle_height metres above the floor (the auto rig puts them at the sole, so the foot rolled about the ground).
    Returns (J, splats kept, floor y, rig units per metre); names without the mixamorig: prefix."""
    g = d['gs'].astype(np.float64); W = d['bw'].astype(np.float64)
    keep = g[:, 3] > min_opacity; g, W = g[keep], W[keep]
    parents = d['parents'].astype(np.int32)
    J = centre_leg_joints(g[:, :3], W, d['joints'].astype(np.float64), names, parents, log=log)
    lo, hi = np.percentile(g[:, 1], [0.5, 99.5]); per_m = (hi - lo) / height
    if ankle_height > 0 and all(f'{s}{b}' in names for s in ('Left', 'Right') for b in ('Leg', 'Foot')):
        for s in ('Left', 'Right'):
            k, a = names.index(s + 'Leg'), names.index(s + 'Foot')
            u = J[a] - J[k]; t = (lo + ankle_height * per_m - J[k][1]) / u[1]; new = J[k] + t * u
            log(f'  {s}Foot raised {(J[a][1] - lo) / per_m:.3f} -> {ankle_height:.3f} m above the floor')
            J[a] = new
    return J, keep, lo, per_m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rig'); ap.add_argument('out', help='output path without extension')
    ap.add_argument('--clips', action='append', default=[], help='folder with K_<name>.npy files (repeatable)')
    ap.add_argument('--clip', action='append', default=[], help='name=path/to/K.npy (repeatable)')
    ap.add_argument('--height', type=float, default=1.79, help='standing height in metres')
    ap.add_argument('--fps', type=float, default=30.0)
    ap.add_argument('--min-opacity', type=float, default=0.004)
    ap.add_argument('--strip-prefix', default='mixamorig:')
    ap.add_argument('--keep-joints', action='store_true', help="keep the rig's leg joints (no rig_joints)")
    ap.add_argument('--ankle-height', type=float, default=0.075, help='ankle joints this high above the floor, m (0: as centred)')
    ap.add_argument('--bone-names', choices=('mannequin', 'source'), default='mannequin',
                    help='mannequin: UE5 Manny names (pelvis, spine_01, hand_l, ...) plus a root bone; source: as in the rig')
    a = ap.parse_args()

    d = np.load(a.rig)
    g = d['gs'].astype(np.float64); W = d['bw'].astype(np.float64)
    keep = g[:, 3] > a.min_opacity; g, W = g[keep], W[keep]
    P = d['pose'].astype(np.float64); nb = len(P)
    Jr = d['joints_rest'].astype(np.float64); J = d['joints'].astype(np.float64)
    names = [str(n) for n in d['bones']]; names = [n[len(a.strip_prefix):] if n.startswith(a.strip_prefix) else n for n in names]
    parents = d['parents'].astype(np.int32)
    src_names = names
    if not a.keep_joints:   # hips and ankles onto their legs' splats (capture pose), rest joints to match
        print('leg joints:')
        J = rig_joints(d, names, a.ankle_height, a.height, a.min_opacity)[0]
        Jr = np.einsum('bij,bj->bi', P, np.c_[J, np.ones(nb)])[:, :3]
    if a.bone_names == 'mannequin': names = [MANNEQUIN.get(n, n) for n in names]
    assert len(set(names)) == nb, 'bone names not unique'
    assert nb <= 255 and (parents < 0).sum() == 1 and all(' ' not in n for n in names)

    # common space: hips over (0,0), lowest splats (0.5th percentile, ignores floaters) on the floor, scaled to height
    lo, hi = np.percentile(g[:, 1], [0.5, 99.5])
    hips = J[src_names.index('Hips')] if 'Hips' in src_names else J[0]
    o = np.array([hips[0], lo, hips[2]]); k = a.height / (hi - lo)
    print(f'{len(g)} splats ({(~keep).sum()} dropped), {nb} bones, rig height {hi - lo:.3f} -> x{k:.4f}')

    def norm(M):  # bone frames (local -> rig space, ...,4,4) -> frames in the normalised (glTF) space
        M = M.copy(); M[..., :3, 3] = k * (M[..., :3, 3] - o); return M

    def T(p):
        M = np.tile(np.eye(4), (len(p), 1, 1)); M[:, :3, 3] = p; return M

    Ref = norm(np.linalg.inv(P) @ T(Jr))     # capture pose: pose^-1 @ T(joint_rest)

    pos = k * (g[:, :3] - o)
    order = morton_order(pos); g, W, pos = g[order], W[order], pos[order]
    R = FLIP @ Rotation.from_quat(g[:, [8, 9, 10, 7]]).as_matrix()
    q = Rotation.from_matrix(R).as_quat()[:, [3, 0, 1, 2]]
    op = np.clip(g[:, 3], 1e-6, 1 - 1e-6)
    splats = dict(pos=pos @ FLIP.T, quat_wxyz=q, log_scale=np.log(np.maximum(np.abs(g[:, 4:7]) * k, 1e-12)),
                  opacity_logit=np.log(op / (1 - op)), dc=(np.clip(g[:, 11:14], 0, 1) - 0.5) / SH_C0)
    idx = np.argsort(-W, 1)[:, :4]; w = np.take_along_axis(W, idx, 1); w /= np.maximum(w.sum(1, keepdims=True), 1e-12)

    clips = [tuple(s.split('=', 1)) for s in a.clip]
    for c in a.clips:   # later folders replace clips of the same name
        clips += [(os.path.basename(p)[2:-4], p) for p in sorted(glob.glob(os.path.join(c, 'K_*.npy')))]
    clips = list(dict(clips).items())
    anims = []
    for n, p in clips:
        K = np.load(p).astype(np.float64); assert K.shape[1:] == (nb, 4, 4), f'{p}: {K.shape}'
        anims.append((n, norm(K @ T(Jr)[None]))); print(f'  clip {n}: {len(K)} frames')

    # head top as an unweighted leaf under the head (Mixamo's HeadTop_End): gives the head's up direction, which the
    # head joint alone does not, so the retargeter can match the head's pitch (retarget_common.retargeter)
    if 'Head' in src_names and 'joints_tail' in d:
        h = src_names.index('Head'); end = Ref[h].copy(); end[:3, 3] = k * (d['joints_tail'][h].astype(np.float64) - o)
        rel = np.linalg.inv(Ref[h]) @ end
        Ref = np.concatenate([Ref, end[None]]); anims = [(n, np.concatenate([M, (M[:, h] @ rel)[:, None]], 1)) for n, M in anims]
        names = names + ['head_end' if a.bone_names == 'mannequin' else 'HeadTop_End']; parents = np.append(parents, h)

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    write_ply(a.out + '.ply', splats, idx, w, Ref[:, :3, 3] @ FLIP.T, names, parents)
    write_glb(a.out + '.glb', names, parents, Ref, anims, a.fps, 'root' if a.bone_names == 'mannequin' else 'Root')
    for ext in ('.ply', '.glb'):
        print(f'wrote {a.out}{ext} ({os.path.getsize(a.out + ext) / 1e6:.1f} MB)')


if __name__ == '__main__':
    main()
