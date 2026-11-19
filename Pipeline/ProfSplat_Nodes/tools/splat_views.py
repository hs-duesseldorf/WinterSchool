"""Render exported splats (canonical frame: y up, feet at 0, front +Z; PLY file = diag(-1,-1,1) @ viewer)
from identical novel views with gsplat, to compare variants: rings at several elevations, a face close-up ring
and a leg close-up ring.

usage: envs/gsplat/bin/python scripts/splat_views.py <out.png> <a.ply> [<b.ply> ...] [--rows ring0,ring35,ringm25,face,legs]"""
import argparse, math
import numpy as np, torch
from PIL import Image
from gsplat import rasterization

D = np.diag([-1.0, -1.0, 1.0, 1.0])  # viewer frame <-> PLY file frame (its own inverse)


def load_ply(path):
    f = open(path, 'rb'); h = b''
    while not h.endswith(b'end_header\n'):
        h += f.readline()
    lines = h.decode().split('\n')
    n = int(next(l for l in lines if l.startswith('element vertex')).split()[-1])
    props = [l.split()[-1] for l in lines if l.startswith('property')]
    a = np.frombuffer(f.read(n * 4 * len(props)), np.float32).reshape(n, -1)
    d = {k: torch.tensor(a[:, i].copy(), device='cuda') for i, k in enumerate(props)}
    rest = sorted([k for k in props if k.startswith('f_rest_')], key=lambda k: int(k[7:]))
    dc = torch.stack([d[f'f_dc_{i}'] for i in range(3)], 1)[:, None]
    sh = torch.cat([dc, torch.stack([d[k] for k in rest], 1).reshape(n, 3, -1).transpose(1, 2)], 1) if rest else dc
    return dict(means=torch.stack([d['x'], d['y'], d['z']], 1),
                quats=torch.stack([d[f'rot_{i}'] for i in range(4)], 1),
                scales=torch.exp(torch.stack([d[f'scale_{i}'] for i in range(3)], 1)),
                opacities=torch.sigmoid(d['opacity']), sh=sh, deg=int(math.sqrt(sh.shape[1]) - 1))


def look_at(eye, target, up=(0, 1, 0)):
    """World-to-camera (OpenCV: x right, y down, z forward) in the viewer frame."""
    eye, target, up = map(np.asarray, (eye, target, up))
    z = target - eye; z = z / np.linalg.norm(z)
    x = np.cross(z, up); x /= np.linalg.norm(x); y = np.cross(z, x)
    R = np.stack([x, y, z]); V = np.eye(4); V[:3, :3] = R; V[:3, 3] = -R @ eye
    return V


ROWS = {  # name: (elevation deg, distance m, target height m, vertical fov deg)
    'ring0': (0, 3.6, 0.9, 36), 'ring35': (35, 3.6, 0.9, 36), 'ringm25': (-25, 3.6, 0.9, 36),
    'face': (5, 0.75, 1.62, 30), 'legs': (25, 1.6, 0.55, 40),
    'face45': (45, 0.6, 1.62, 34), 'side': (0, 1.7, 1.2, 50), 'sidehi': (12, 1.7, 1.2, 50), 'ring60': (60, 3.2, 0.9, 40), 'facem20': (-20, 0.6, 1.6, 34),
}


def hand_targets(g):
    """The two hands (viewer frame): the most lateral opaque splats between 0.5 m and 1.4 m height."""
    p = (g['means'] * torch.tensor([-1.0, -1.0, 1.0], device='cuda')).cpu().numpy()
    op = g['opacities'].cpu().numpy()
    sel = (op > 0.5) & (p[:, 1] > 0.5) & (p[:, 1] < 1.4)
    q = p[sel]
    out = []
    for side in (-1, 1):
        x = side * q[:, 0]
        tip = q[x >= np.percentile(x, 99.7)]
        out.append(tip.mean(0) - np.array([side * 0.04, 0, 0]))  # a little inward from the tips: the whole hand
    return out


def render_rows(g, rows, W=512, H=683, bg=0.5):
    out = []
    for name in rows:
        if name == 'hands':  # 4 views of each hand: front, front-outside, outside, back (inside is in the body)
            el, dist, fov = 10, 0.5, 34
            views = [(tgt, az) for tgt, side in zip(hand_targets(g), (-1, 1)) for az in (0, side * 45, side * 90, 180)]
        else:
            el, dist, ty, fov = ROWS[name]
            views = [(np.array([0, ty, 0.0]), az) for az in range(0, 360, 45)]
        f = H / 2 / math.tan(math.radians(fov) / 2)
        K = torch.tensor([[f, 0, W / 2], [0, f, H / 2], [0, 0, 1]], device='cuda', dtype=torch.float32)[None]
        row = []
        for tgt, az in views:
            t, e = math.radians(az), math.radians(el)
            eye = tgt + dist * np.array([math.sin(t) * math.cos(e), math.sin(e), math.cos(t) * math.cos(e)])
            V = torch.tensor(look_at(eye, tgt) @ D, device='cuda', dtype=torch.float32)[None]
            img, alpha, _ = rasterization(g['means'], g['quats'], g['scales'], g['opacities'], g['sh'], V, K, W, H,
                                          sh_degree=g['deg'])
            row.append(((img[0] + (1 - alpha[0]) * bg).clamp(0, 1).cpu().numpy() * 255).astype(np.uint8))
        out.append(np.concatenate(row, 1))
    return np.concatenate(out, 0)


def main():
    p = argparse.ArgumentParser(); p.add_argument('out'); p.add_argument('plys', nargs='+')
    p.add_argument('--rows', default='ring0,ring35,ringm25,face,legs'); p.add_argument('--scale', type=float, default=0.5)
    a = p.parse_args()
    rows = a.rows.split(',')
    sheets = []
    for path in a.plys:
        sheets.append(render_rows(load_ply(path), rows))
        sheets.append(np.full((12, sheets[-1].shape[1], 3), 255, np.uint8))
    s = np.concatenate(sheets[:-1], 0)
    im = Image.fromarray(s)
    im.resize((round(im.width * a.scale), round(im.height * a.scale)), Image.LANCZOS).save(a.out)
    print('saved', a.out, im.size)


if __name__ == '__main__':
    main()
