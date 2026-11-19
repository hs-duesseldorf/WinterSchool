#!/usr/bin/env python3
# Captury OSC recordings (MoCap Depot "<Take>_OSC.zip" / .osc) -> .glb on the Captury skeleton (Dab.glb), so every take
# imports animation only onto SKEL_Captury exactly like Dab does (Scripts/retarget_anims.sh calls this for .osc / .zip).
#
#   captury_osc_to_glb.py <skeleton.glb> <out dir> <take.osc | take_OSC.zip> ...     -> <out dir>/<Take>.glb
#
# Why OSC and not the Depot's FBX / GLB: those come from different Captury exports. Four takes (Hallo, Handstand, ...)
# match Dab, the other eleven have other joint axes, a bind pose with Spine2/3 rolled 180 deg and ~1/30 size. The OSC
# recording is Captury's own stream, the same for every take: one line per bone and frame,
#   <ms>;/<actor>/maya/<Bone>/matrix;<4x4 world matrix, row major, decimal comma, "|f" after each value>
# in mm, Y up. The rotation is the bone's world rotation relative to the actor's T-pose (all bones line up when the
# actor stands in it), which is also what Captury Live streams (CapturyLiveLinkSource.cpp, poseRot).
#
# On the skeleton: world rotation = A * D * Q * A^T * rest, where D is the OSC rotation, A the fixed axis change from
# Captury's world to glTF's, rest the skeleton's T-pose frame, and Q turns the skeleton's T-pose bone directions onto the
# recorded actor's (measured from the take's own joint positions; the actors' builds differ by up to ~25 deg at the
# arms). The hips path is scaled by the leg length ratio, so the feet stay on the floor. Captury streams 20 bones;
# the others (Spine3/4, fingers, toes, end effectors) keep their rest pose relative to their parent.
import io, os, struct, sys, zipfile
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glb_util

A = np.array([[0, 0, -1], [0, 1, 0], [1, 0, 0]], float)   # Captury world -> glTF world (Blender's FBX/glTF route, as Dab)
OSC_TO_SKEL = {"Root": "Hips"}


def read_osc(path):
    """(take name, frame times in s, {bone: (F, 4, 4) world matrices in mm})."""
    name = os.path.splitext(os.path.basename(path))[0]
    if path.lower().endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            member = next(m for m in z.namelist() if m.lower().endswith(".osc"))
            text = io.TextIOWrapper(z.open(member), encoding="latin-1").read()
        for s in ("_OSC", "OSC"):
            if name.endswith(s): name = name[:-len(s)]; break
    else:
        text = open(path, encoding="latin-1").read()
    frames, cur, t0 = [], {}, None
    for line in text.splitlines():
        q = line.strip().split(";")
        if len(q) < 18 or "/matrix" not in q[1]: continue
        bone = q[1].split("/")[-2]
        if bone in cur: frames.append((t0, cur)); cur = {}
        if not cur: t0 = int(q[0])
        cur[bone] = [float(v.split("|")[0].replace(",", ".")) for v in q[2:18]]
    if cur: frames.append((t0, cur))
    bones = max((set(f) for _, f in frames), key=len)
    frames = [(t, f) for t, f in frames if set(f) == bones]   # drop partial first / last frames
    times = np.array([t for t, _ in frames], float)
    dt = np.median(np.diff(times)) / 1000.0
    mats = {b: np.array([f[b] for _, f in frames]).reshape(-1, 4, 4) for b in bones}
    return name, np.arange(len(frames)) * dt, mats


def quat(R):
    """(..., 3, 3) rotation -> (..., 4) xyzw."""
    R = np.asarray(R); q = np.empty(R.shape[:-2] + (4,))
    tr = R[..., 0, 0] + R[..., 1, 1] + R[..., 2, 2]
    for idx in np.ndindex(R.shape[:-2]):
        m = R[idx]; t = tr[idx]
        if t > 0:
            s = 2 * np.sqrt(t + 1); q[idx] = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, s / 4]
        else:
            i = int(np.argmax([m[0, 0], m[1, 1], m[2, 2]])); j, k = (i + 1) % 3, (i + 2) % 3
            s = 2 * np.sqrt(1 + m[i, i] - m[j, j] - m[k, k]); v = np.empty(4)
            v[i] = s / 4; v[j] = (m[j, i] + m[i, j]) / s; v[k] = (m[k, i] + m[i, k]) / s; v[3] = (m[k, j] - m[j, k]) / s
            q[idx] = v
    return q


def fit_rotation(src, dst):
    """Rotation turning unit vectors src (N, 3) onto dst (N, 3): the shortest arc for one vector, least squares else."""
    if len(src) == 1:
        a, b = src[0], dst[0]; v = np.cross(a, b); c = a @ b
        if np.linalg.norm(v) < 1e-9: return np.eye(3)
        K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
        return np.eye(3) + K + K @ K / (1 + c)
    U, _, Vt = np.linalg.svd(dst.T @ src)
    return U @ np.diag([1, 1, np.linalg.det(U @ Vt)]) @ Vt


def convert(skeleton, osc_path, out_dir):
    name, times, mats = read_osc(osc_path)
    doc, binary = glb_util.clean_captury(*glb_util.read(skeleton))
    nodes = doc["nodes"]; par = glb_util.parents(doc)
    joints = doc["skins"][0]["joints"]
    idx = {nodes[j]["name"]: j for j in joints}
    W = {j: np.array(glb_util.world_matrix(doc, j, par)) for j in joints}
    Rr = {j: W[j][:3, :3] / np.cbrt(np.linalg.det(W[j][:3, :3])) for j in joints}   # rest frames, scale removed
    D = {idx[OSC_TO_SKEL.get(b, b)]: m[:, :3, :3] for b, m in mats.items() if OSC_TO_SKEL.get(b, b) in idx}
    P = {idx[OSC_TO_SKEL.get(b, b)]: m[:, :3, 3] for b, m in mats.items() if OSC_TO_SKEL.get(b, b) in idx}
    missing = sorted(set(mats) - {nodes[j]["name"] if nodes[j]["name"] != "Hips" else "Root" for j in D})
    if missing: print(f"  {name}: OSC bones not on the skeleton: {missing}")
    hips = idx["Hips"]

    def driven_parent(j):
        p = par.get(j)
        while p is not None and p not in D: p = par.get(p)
        return p
    kids = {j: [c for c in D if driven_parent(c) == j] for j in D}

    # Q: the skeleton's T-pose bone directions -> the recorded actor's (both in Captury's world)
    Q = {}
    for j in D:
        if not kids[j]: Q[j] = np.eye(3); continue
        src, dst = [], []
        for c in kids[j]:
            r = A.T @ (W[c][:3, 3] - W[j][:3, 3]); src.append(r / np.linalg.norm(r))
            d = np.einsum("fji,fj->fi", D[j], P[c] - P[j])       # D^T (p_c - p_j): direction in the actor's T-pose
            d /= np.linalg.norm(d, axis=1, keepdims=True); d = d.mean(0); dst.append(d / np.linalg.norm(d))
        Q[j] = fit_rotation(np.array(src), np.array(dst))

    def leg(pos):
        return np.mean([np.linalg.norm(pos[idx[s + "Leg"]] - pos[idx[s + "UpLeg"]], axis=-1).mean()
                        + np.linalg.norm(pos[idx[s + "Foot"]] - pos[idx[s + "Leg"]], axis=-1).mean() for s in ("Left", "Right")])
    k = leg({j: W[j][:3, 3] for j in joints}) / (leg(P) / 1000.0)   # skeleton m per recorded m

    # world rotations per frame, hierarchy order (parents first)
    order = []
    def visit(j):
        order.append(j)
        for c in nodes[j].get("children", []):
            if c in W: visit(c)
    visit(hips)
    G, L = {}, {}
    for j in order:
        p = par.get(j) if par.get(j) in G else None
        if j in D: G[j] = A @ D[j] @ Q[j] @ A.T @ Rr[j]
        else: G[j] = G[p] @ (Rr[p].T @ Rr[j])
        L[j] = G[j] if p is None else np.einsum("fji,fjk->fik", G[p], G[j])
    # self check: the skeleton's bone directions against the recorded joint positions
    err = []
    for j in D:
        for c in kids[j]:
            v = np.einsum("fij,j->fi", G[j] @ Rr[j].T, W[c][:3, 3] - W[j][:3, 3])
            u = (A @ (P[c] - P[j]).T).T
            cos = np.sum(v * u, 1) / np.linalg.norm(v, axis=1) / np.linalg.norm(u, axis=1)
            err.append(np.degrees(np.arccos(np.clip(cos, -1, 1))))
    err = np.concatenate(err)

    doc["animations"] = []
    t_acc = glb_util.add_accessor(doc, binary, times.astype("<f4").tobytes(), len(times), "SCALAR", minmax=([float(times[0])], [float(times[-1])]))
    samplers, channels = [], []
    def channel(node, path, values, type_):
        samplers.append(dict(input=t_acc, interpolation="LINEAR",
                             output=glb_util.add_accessor(doc, binary, values.astype("<f4").tobytes(), len(values), type_)))
        channels.append(dict(sampler=len(samplers) - 1, target=dict(node=node, path=path)))
    for j in order:
        if j not in D: continue   # keeps its rest pose
        q = quat(L[j])
        for f in range(1, len(q)): q[f] *= np.sign(q[f] @ q[f - 1]) or 1   # one hemisphere, for interpolation
        channel(j, "rotation", q, "VEC4")
    channel(hips, "translation", (A @ P[hips].T).T / 1000.0 * k, "VEC3")
    doc["animations"].append(dict(name=name, samplers=samplers, channels=channels))
    out = os.path.join(out_dir, name + ".glb")
    glb_util.write(out, doc, binary)
    print(f"{osc_path} -> {out}: {len(times)} frames, {times[-1]:.1f} s at {1 / (times[1] - times[0]):.0f} fps, "
          f"{len(D)} bones, size x{k:.3f}, bone direction error median {np.median(err):.1f} / max {err.max():.1f} deg")
    return out


if __name__ == "__main__":
    if len(sys.argv) < 4: sys.exit(__doc__ or "usage: captury_osc_to_glb.py <skeleton.glb> <out dir> <take.osc|zip> ...")
    os.makedirs(sys.argv[2], exist_ok=True)
    for p in sys.argv[3:]: convert(sys.argv[1], p, sys.argv[2])
