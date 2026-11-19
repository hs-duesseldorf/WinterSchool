# Small binary glTF (.glb) helpers, pure Python (used in the editor by retarget_common.py and outside it by
# captury_osc_to_glb.py).
import json, struct

GLB_MAGIC, JSON_CHUNK, BIN_CHUNK = 0x46546C67, 0x4E4F534A, 0x004E4942


def read(path):
    """(json doc, binary chunk as bytearray)."""
    b = open(path, "rb").read()
    n = struct.unpack_from("<I", b, 12)[0]
    doc = json.loads(b[20:20 + n])
    rest = b[20 + n:]
    binary = bytearray(rest[8:8 + struct.unpack_from("<I", rest, 0)[0]]) if len(rest) >= 8 else bytearray()
    return doc, binary


def write(path, doc, binary):
    binary = bytearray(binary); binary += b"\0" * (-len(binary) % 4)
    if doc.get("buffers"): doc["buffers"][0]["byteLength"] = len(binary)
    js = json.dumps(doc, separators=(",", ":")).encode(); js += b" " * (-len(js) % 4)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", GLB_MAGIC, 2, 12 + 8 + len(js) + 8 + len(binary)))
        f.write(struct.pack("<II", len(js), JSON_CHUNK) + js + struct.pack("<II", len(binary), BIN_CHUNK) + bytes(binary))


def add_accessor(doc, binary, data, count, type_, component=5126, minmax=None):
    """Append raw little-endian data as a new buffer view + accessor; returns the accessor index."""
    binary += b"\0" * (-len(binary) % 4)
    doc.setdefault("bufferViews", []).append(dict(buffer=0, byteOffset=len(binary), byteLength=len(data)))
    binary += data
    acc = dict(bufferView=len(doc["bufferViews"]) - 1, componentType=component, count=count, type=type_)
    if minmax: acc["min"], acc["max"] = minmax
    doc.setdefault("accessors", []).append(acc)
    return len(doc["accessors"]) - 1


def node_matrix(nd):
    """Local 4x4 (row-major nested lists) of a glTF node."""
    if "matrix" in nd: m = nd["matrix"]; return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
    t = nd.get("translation", [0, 0, 0]); x, y, z, w = nd.get("rotation", [0, 0, 0, 1]); s = nd.get("scale", [1, 1, 1])
    R = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return [[R[r][c] * s[c] for c in range(3)] + [t[r]] for r in range(3)] + [[0, 0, 0, 1]]


def parents(doc):
    return {c: i for i, nd in enumerate(doc.get("nodes", [])) for c in nd.get("children", [])}


def world_matrix(doc, i, par=None):
    par = parents(doc) if par is None else par
    M = node_matrix(doc["nodes"][i])
    while i in par:
        i = par[i]; P = node_matrix(doc["nodes"][i])
        M = [[sum(P[r][k] * M[k][c] for k in range(4)) for c in range(4)] for r in range(4)]
    return M


def read_floats(doc, binary, i):
    """Float accessor i as a list of tuples (one per element)."""
    acc = doc["accessors"][i]; bv = doc["bufferViews"][acc["bufferView"]]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[acc["type"]]
    off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0); stride = bv.get("byteStride", 4 * n)
    return [struct.unpack_from(f"<{n}f", binary, off + k * stride) for k in range(acc["count"])]


def _mul(A, B): return [[sum(A[r][k] * B[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def _inv_rigid(M):  # rotation (+ uniform scale) and translation
    s2 = sum(M[k][0] ** 2 for k in range(3)); Rt = [[M[c][r] / s2 for c in range(3)] for r in range(3)]
    return [Rt[r] + [-sum(Rt[r][k] * M[k][3] for k in range(3))] for r in range(3)] + [[0, 0, 0, 1]]


def clean_captury(doc, binary):
    """Makes a Captury export (via Blender) a plain skeleton for Unreal. Idempotent.
    - Bone names are "unknown:Hips": Interchange keeps the namespace for glTF (it drops it for FBX). Stripped.
    - The top bone (Hips) is left out of the skin: Interchange makes up a "<name>_ProxyTrueRootJoint" for it and the IK Rig
      finds no pelvis. The joints' missing ancestors go into the skin.
    - Hips is scaled 0.001 and everything below it is in mm (Captury's unit). Unreal keeps that scale on the Hips
      reference transform, and the IK Rig / IK Retargeter measure the skeleton without it: a reference pose ~1000x too
      big (pelvis height, chain lengths and IK goals all wrong). The scale is baked into the joints' translations (rest
      pose and animation), so every joint has scale 1 and the skeleton is in metres.
    The inverse bind matrices are made from the rest pose again (Captury binds in the rest pose; checked)."""
    nodes = doc.get("nodes", [])
    for nd in nodes:
        if "name" in nd: nd["name"] = nd["name"].split(":")[-1]
    par = parents(doc); changed = False
    for skin in doc.get("skins", []):  # bind pose == rest pose?
        if "inverseBindMatrices" not in skin: continue
        for j, m in zip(skin["joints"], read_floats(doc, binary, skin["inverseBindMatrices"])):
            I = _mul(world_matrix(doc, j, par), [[m[c * 4 + r] for c in range(4)] for r in range(4)])
            if max(abs(I[r][c] - (r == c)) for r in range(4) for c in range(4)) > 1e-3:
                raise ValueError(f"joint {nodes[j].get('name')}: bind pose differs from the rest pose")
    for skin in doc.get("skins", []):  # missing ancestors into the skin
        for j in list(skin["joints"]):
            p = par.get(j)
            while p is not None and p not in skin["joints"] and "mesh" not in nodes[p]:
                skin["joints"].append(p); changed = True; p = par.get(p)
    joints = {j for skin in doc.get("skins", []) for j in skin["joints"]}

    # bake joint scales: factor[j] = product of the (uniform) scales of j's joint ancestors
    def own(j): s = nodes[j].get("scale", [1, 1, 1]); return sum(s) / 3
    if any(abs(own(j) - 1) > 1e-4 for j in joints):
        for j in joints:
            s = nodes[j].get("scale", [1, 1, 1])
            if max(s) - min(s) > 1e-4 * max(abs(x) for x in s): raise ValueError(f"joint {nodes[j].get('name')}: non-uniform scale {s}")
        factor = {}
        for j in joints:
            f, p = 1.0, par.get(j)
            while p is not None and p in joints: f *= own(p); p = par.get(p)
            factor[j] = f
        for anim in doc.get("animations", []):
            keep = []
            for ch in anim["channels"]:
                j, path = ch["target"].get("node"), ch["target"]["path"]
                if j not in joints: keep.append(ch); continue
                smp = anim["samplers"][ch["sampler"]]
                if path == "scale":   # must stay at the rest scale, which is baked
                    vals = read_floats(doc, binary, smp["output"])
                    if max(abs(v - own(j)) for t in vals for v in t) > 1e-3 * abs(own(j)):
                        raise ValueError(f"joint {nodes[j].get('name')}: animated scale")
                    continue
                if path == "translation" and factor[j] != 1:
                    vals = read_floats(doc, binary, smp["output"])
                    data = struct.pack(f"<{3 * len(vals)}f", *[v * factor[j] for t in vals for v in t])
                    anim["samplers"].append(dict(smp, output=add_accessor(doc, binary, data, len(vals), "VEC3")))
                    ch = dict(ch, sampler=len(anim["samplers"]) - 1)
                keep.append(ch)
            anim["channels"] = keep
        for j in joints:
            nd = nodes[j]
            if "matrix" in nd: raise ValueError(f"joint {nd.get('name')}: matrix transform")
            if "translation" in nd: nd["translation"] = [v * factor[j] for v in nd["translation"]]
            nd.pop("scale", None)
        changed = True
    if changed:
        for skin in doc.get("skins", []):
            data = b"".join(struct.pack("<16f", *[_inv_rigid(world_matrix(doc, j, par))[r][c] for c in range(4) for r in range(4)])
                            for j in skin["joints"])
            skin["inverseBindMatrices"] = add_accessor(doc, binary, data, len(skin["joints"]), "MAT4")
    return doc, binary
