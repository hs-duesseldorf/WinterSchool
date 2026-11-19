#!/usr/bin/env python3
"""Turns the collision mesh of PlayCanvas splat-transform (--collision-mesh) into one Unreal can use for a splat.

  collision_from_splat_transform.py <in.collision.glb> <out.glb> [--tris 300000]

splat-transform voxelises the splats (its --collision-mesh writes a .collision.glb next to the .voxel.json) and meshes the
voxels. That mesh is in splat-transform's engine frame, i.e. the PLY (COLMAP: X right, Y down, Z forward) with X and Y
negated (checked: nearest-splat distance 0.33 m vs > 1.7 m for the other sign choices). It has millions of small
voxel faces, so this script:
  1. moves it into a glTF frame in which Unreal's glTF import puts it exactly on top of the splat imported by Epic's
     GaussianSplat importer (both at the same transform): Epic's importer maps PLY (x, y, z) to UE (z, x, -y) * 100,
     UE's glTF import maps glTF (x, y, z) to UE (x, z, y) * 100. Only a rotation, so winding is kept,
  2. decimates it to --tris triangles,
  3. makes sure the triangles face outwards (Unreal traces triangle meshes single-sided): a few vertical probes must hit
     an upward-facing floor first.
Needs numpy, trimesh, fast-simplification.
"""
import argparse, time
import numpy as np
import trimesh, fast_simplification


def st_to_gltf(v):
    # PLY p = (-sx, -sy, sz); glTF = (pz, -py, px) = (sz, sy, -sx)
    return np.stack([v[:, 2], v[:, 1], -v[:, 0]], 1)


def first_hit_normal_up(verts, faces, x, z):
    """Normal y of the highest triangle above/below the vertical line through (x, z) (glTF: Y up), None if none."""
    a, b, c = verts[faces[:, 0]], verts[faces[:, 1]], verts[faces[:, 2]]
    # barycentric test in the XZ plane
    d = (b[:, 2] - c[:, 2]) * (a[:, 0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (a[:, 2] - c[:, 2])
    ok = np.abs(d) > 1e-12
    l1 = np.where(ok, ((b[:, 2] - c[:, 2]) * (x - c[:, 0]) + (c[:, 0] - b[:, 0]) * (z - c[:, 2])) / np.where(ok, d, 1), -1)
    l2 = np.where(ok, ((c[:, 2] - a[:, 2]) * (x - c[:, 0]) + (a[:, 0] - c[:, 0]) * (z - c[:, 2])) / np.where(ok, d, 1), -1)
    l3 = 1 - l1 - l2
    hit = ok & (l1 >= 0) & (l2 >= 0) & (l3 >= 0)
    if not hit.any(): return None
    y = l1 * a[:, 1] + l2 * b[:, 1] + l3 * c[:, 1]
    i = np.flatnonzero(hit)[np.argmax(y[hit])]
    n = np.cross(b[i] - a[i], c[i] - a[i])
    return n[1] / (np.linalg.norm(n) + 1e-12)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('glb'); ap.add_argument('out')
    ap.add_argument('--tris', type=int, default=300000, help='target triangle count')
    a = ap.parse_args()
    t0 = time.time()
    m = trimesh.load(a.glb, force='mesh', process=False)
    verts, faces = st_to_gltf(np.asarray(m.vertices, np.float64)), np.asarray(m.faces)
    print(f'{a.glb}: {len(verts)} verts, {len(faces)} tris')
    if len(faces) > a.tris:
        verts, faces = fast_simplification.simplify(verts.astype(np.float32), faces.astype(np.int32), 1 - a.tris / len(faces))
        print(f'decimated: {len(faces)} tris ({time.time() - t0:.0f}s)')
    verts = np.asarray(verts, np.float64)
    # winding: the topmost surface under a few points spread over the middle of the scene must face up
    lo, hi = np.percentile(verts, 20, 0), np.percentile(verts, 80, 0)
    ups = []
    for fx in np.linspace(0, 1, 5):
        for fz in np.linspace(0, 1, 5):
            ny = first_hit_normal_up(verts, faces, lo[0] + fx * (hi[0] - lo[0]), lo[2] + fz * (hi[2] - lo[2]))
            if ny is not None and abs(ny) > 0.5: ups.append(ny > 0)
    if ups and np.mean(ups) < 0.5:
        faces = faces[:, ::-1].copy(); print(f'flipped the winding ({np.mean(ups):.2f} of the top surfaces faced down)')
    else: print(f'winding ok ({np.mean(ups) if ups else float("nan"):.2f} of {len(ups)} top surfaces face up)')
    out = trimesh.Trimesh(verts, faces, process=True)
    out.remove_unreferenced_vertices()
    out.visual = trimesh.visual.ColorVisuals(out, face_colors=[255, 0, 255, 255])  # magenta if anyone shows it
    out.export(a.out)
    print(f'wrote {a.out}: {len(out.faces)} tris, bounds (glTF m) {out.bounds.round(2).tolist()}, {time.time() - t0:.0f}s')


if __name__ == '__main__':
    main()
