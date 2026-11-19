"""Estimates the lighting baked into a 3DGS capture, so the scene's sun and splat shadows can match it.

  python fit_capture_lighting.py <capture.ply> [--ground-z 0] [--radius 15] [--out lighting.json]

- Sun direction: the splats form an occupancy grid; for a candidate sun direction, rays from ground splats toward the sun
  tell which ground points the capture's own geometry shadows. The direction whose predicted shadows best separate dark
  from bright ground (largest brightness gap) is the capture's sun. Coarse search, then refined around the best.
- Shadow color: per channel, the linear colour of shadowed ground divided by lit ground (median of matched brightness
  bands), i.e. what a full shadow turns a sunlit splat into: r.GaussianSplat.Lighting.ShadowColorR/G/B.
- Sky colours: mean linear colour of the far splats near the horizon and high up, for a sky dome behind the splats.
- Ground colour: typical linear colour of the ground splats, for the sky light's lower hemisphere (indirect light from
  the ground onto characters, also on mobile, which has no GI).
Coordinates are Unreal's (cm, Z up), converted the way the GaussianSplat importer converts a PLY (RH_RDF): (z, x, -y).
"""
import argparse, json, math, sys
import numpy as np

SH_C0 = 0.28209479177387814


def load_ply(path):
    with open(path, "rb") as f:
        header = b""
        while not header.endswith(b"end_header\n"):
            header += f.readline()
        lines = header.split(b"\n")
        props = [l.split()[-1].decode() for l in lines if l.startswith(b"property")]
        n = int([l for l in lines if l.startswith(b"element vertex")][0].split()[-1])
        data = np.fromfile(f, dtype=np.float32, count=n * len(props)).reshape(n, len(props))
    col = {p: i for i, p in enumerate(props)}
    x, y, z = (data[:, col[k]] for k in "xyz")
    pos = np.stack([z, x, -y], 1)  # meters, Unreal axes (X fwd, Y right, Z up)
    srgb = np.clip(0.5 + SH_C0 * data[:, [col["f_dc_0"], col["f_dc_1"], col["f_dc_2"]]], 0, 1)
    opacity = 1 / (1 + np.exp(-data[:, col["opacity"]]))
    scale = np.exp(data[:, [col["scale_0"], col["scale_1"], col["scale_2"]]]).max(1)
    return pos, srgb, opacity, scale


def to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def direction(elev, azim):
    e, a = math.radians(elev), math.radians(azim)
    return np.array([math.cos(e) * math.cos(a), math.cos(e) * math.sin(a), math.sin(e)])


class Occupancy:
    def __init__(self, pos, voxel):
        self.lo = pos.min(0) - voxel
        self.voxel = voxel
        idx = ((pos - self.lo) / voxel).astype(np.int64)
        self.shape = idx.max(0) + 2
        self.grid = np.zeros(self.shape, bool)
        self.grid[idx[:, 0], idx[:, 1], idx[:, 2]] = True

    def shadowed(self, pts, d, start, length, step):
        """True for the points whose ray toward d hits an occupied voxel between start and length meters"""
        hit = np.zeros(len(pts), bool)
        for t in np.arange(start, length, step):
            idx = ((pts + d * t - self.lo) / self.voxel).astype(np.int64)
            ok = np.all((idx >= 0) & (idx < self.shape), 1)
            live = ok & ~hit
            if not live.any():
                break
            hit[live] = self.grid[idx[live, 0], idx[live, 1], idx[live, 2]]
        return hit


def score(occ, pts, lum, d):
    s = occ.shadowed(pts, d, 0.35, 25.0, 0.1)
    frac = s.mean()
    if frac < 0.03 or frac > 0.97:
        return -1.0, s
    # brightness gap between predicted-lit and predicted-shadowed ground, in units of the spread (point-biserial)
    return (lum[~s].mean() - lum[s].mean()) / lum.std() * math.sqrt(frac * (1 - frac)), s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ply")
    ap.add_argument("--ground-z", type=float, default=0.0, help="height of the walkable ground in meters (Unreal Z)")
    ap.add_argument("--radius", type=float, default=15.0, help="ground samples within this many meters of the origin")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    rng = np.random.default_rng(1)

    pos, srgb, opacity, scale = load_ply(a.ply)
    lin = to_linear(srgb)
    lum = lin @ np.array([0.2126, 0.7152, 0.0722])
    solid = (opacity > 0.5) & (scale < 0.3)
    occ = Occupancy(pos[solid], 0.1)
    print(f"{len(pos)} splats, {solid.sum()} occluders, grid {occ.shape.tolist()}", file=sys.stderr)

    horiz = np.hypot(pos[:, 0], pos[:, 1])
    ground = solid & (np.abs(pos[:, 2] - a.ground_z) < 0.25) & (horiz < a.radius)
    gi = np.flatnonzero(ground)
    print(f"{len(gi)} ground splats", file=sys.stderr)

    def search(n, elevs, azims):
        sel = rng.choice(gi, min(n, len(gi)), replace=False)
        best = (-2, None, None)
        for e in elevs:
            for az in azims:
                sc, _ = score(occ, pos[sel], lum[sel], direction(e, az))
                if sc > best[0]:
                    best = (sc, e, az)
        return best

    sc, e, az = search(3000, range(10, 86, 5), range(0, 360, 10))
    print(f"coarse: elevation {e}, azimuth {az}, score {sc:.3f}", file=sys.stderr)
    sc, e, az = search(12000, np.arange(e - 5, e + 5.1, 1), np.arange(az - 10, az + 10.1, 2))
    print(f"fine:   elevation {e:.0f}, azimuth {az:.0f}, score {sc:.3f}", file=sys.stderr)

    sel = gi if len(gi) <= 60000 else rng.choice(gi, 60000, replace=False)
    _, shadow = score(occ, pos[sel], lum[sel], direction(e, az))
    lit_c, sh_c = lin[sel][~shadow], lin[sel][shadow]
    # trim the extremes (mispredicted samples), then the ratio of the typical shadowed to the typical lit colour
    def typical(c):
        l = c @ np.array([0.2126, 0.7152, 0.0722])
        lo, hi = np.percentile(l, [25, 75])
        return c[(l >= lo) & (l <= hi)].mean(0)
    shadow_color = np.clip(typical(sh_c) / np.maximum(typical(lit_c), 1e-4), 0, 1)

    far = (np.linalg.norm(pos, axis=1) > 15) & (opacity > 0.2)
    elev_of = np.degrees(np.arctan2(pos[:, 2] - a.ground_z, horiz))
    blue = lin[:, 2] > lin[:, 0]  # sky, not trees or roofs
    horizon = lin[far & blue & (elev_of > 5) & (elev_of < 25)].mean(0)
    zenith = lin[far & blue & (elev_of > 40)].mean(0) if (far & blue & (elev_of > 40)).any() else horizon

    # The light travels against the direction to the sun: Unreal rotator of a directional light (pitch < 0 points down)
    result = {
        "sun_elevation_deg": float(e), "sun_azimuth_deg": float(az),
        "light_rotation": {"pitch": -float(e), "yaw": float((az + 180) % 360), "roll": 0.0},
        "shadow_color_linear": [round(float(v), 3) for v in shadow_color],
        "sky_horizon_linear": [round(float(v), 4) for v in horizon],
        "sky_zenith_linear": [round(float(v), 4) for v in zenith],
        "ground_linear": [round(float(v), 4) for v in typical(lin[gi])],
        # bright end of the sunlit ground (per channel 95th percentile): the sun must be strong enough that this is still
        # a valid albedo, or the splat shadow clips lighter than the capture's (setup_lighting.py sets the intensity)
        "ground_lit_p95_linear": [round(float(v), 4) for v in np.percentile(lit_c, 95, axis=0)],
        "score": round(float(sc), 3), "shadowed_fraction": round(float(shadow.mean()), 3),
    }
    print(json.dumps(result, indent=1))
    if a.out:
        with open(a.out, "w") as f:
            json.dump(result, f, indent=1)


if __name__ == "__main__":
    main()
