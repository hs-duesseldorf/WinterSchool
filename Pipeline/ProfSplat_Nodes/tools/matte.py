"""Soft alpha mattes for an orbit's frames (and the hero) with MatAnyone 2, fused with the SAM masks.

SAM3 finds the person reliably but segments at 1008 px, so its edges are coarse at full resolution.
MatAnyone 2 gives soft, temporally stable alpha (hair, fingertips) from the first frame's mask on.
The fusion keeps SAM as the authority away from the edge: inside the eroded SAM mask alpha is 1,
outside the dilated one it is 0, and only the band in between takes the matte. A matte that drifts
cannot eat the body or pull in background.

Foreground colors: at a soft edge a pixel mixes the person with the (white) studio background, so
the RGB is re-estimated (pymatting's multilevel foreground estimation) before training composites
it over random backgrounds; without it the edges get a white fringe.

usage: envs/matting/bin/python profsplat/tools/matte.py <in dir> <out dir> [--band 12] [--warmup 10]
  <in dir>: frames/*.png, masks/frames/*.png, optionally hero/hero.png + masks/hero/hero.png
  <out dir>: the same layout; frames/ hold foreground-corrected RGB, masks/ the fused alpha (8 bit)"""
import argparse, json, os, sys, time
from pathlib import Path

import numpy as np, torch
from PIL import Image
from scipy import ndimage

ROOT = Path(os.environ.get('PROFSPLAT_ROOT') or Path(__file__).resolve().parents[2] / 'work')
CKPT = ROOT / 'models' / 'matanyone2' / 'matanyone2.pth'


def log(msg):
    print(msg, flush=True)


def load_model():
    from matanyone2.utils.get_default_model import get_matanyone2_model
    from matanyone2.inference.inference_core import InferenceCore
    net = get_matanyone2_model(str(CKPT), 'cuda')
    return net


@torch.inference_mode()
def matte_sequence(net, frames, first_mask, warmup, erode_dilate=10):
    """frames: list of HxWx3 uint8; first_mask: HxW uint8 (0/255). Returns list of HxW float alpha."""
    from matanyone2.inference.inference_core import InferenceCore
    from matanyone2.utils.inference_utils import gen_dilate, gen_erosion
    proc = InferenceCore(net, cfg=net.cfg)
    m = first_mask.astype(np.float32)
    m = gen_dilate(m, erode_dilate, erode_dilate)
    m = gen_erosion(m, erode_dilate, erode_dilate)
    mask = torch.from_numpy(m).float().cuda()
    seq = [frames[0]] * warmup + list(frames)
    out = []
    with torch.autocast('cuda', dtype=torch.float16):
        for ti, f in enumerate(seq):
            img = torch.from_numpy(f).permute(2, 0, 1).float().cuda() / 255.0
            if ti == 0:
                proc.step(img, mask, objects=[1])
                prob = proc.step(img, first_frame_pred=True)
            elif ti <= warmup:
                prob = proc.step(img, first_frame_pred=True)
            else:
                prob = proc.step(img)
            a = proc.output_prob_to_mask(prob)
            if ti >= warmup:
                out.append(a.float().clamp(0, 1).cpu().numpy())
    return out


def fuse(sam, matte, band):
    """sam: HxW bool; matte: HxW float. Inside the eroded SAM mask 1, outside the dilated one 0."""
    core = ndimage.binary_erosion(sam, iterations=band)
    reach = ndimage.binary_dilation(sam, iterations=band)
    a = np.where(core, 1.0, np.where(reach, matte, 0.0))
    return a.astype(np.float32)


def foreground(img, alpha):
    from pymatting import estimate_foreground_ml
    fg = estimate_foreground_ml(img.astype(np.float64) / 255.0, alpha.astype(np.float64))
    return (fg.clip(0, 1) * 255 + 0.5).astype(np.uint8)


def save(img, alpha, img_path, mask_path):
    img_path.parent.mkdir(parents=True, exist_ok=True); mask_path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(img).save(img_path)
    Image.fromarray((alpha * 255 + 0.5).astype(np.uint8)).save(mask_path)


def main():
    p = argparse.ArgumentParser(); p.add_argument('src', type=Path); p.add_argument('out', type=Path)
    p.add_argument('--band', type=int, default=12); p.add_argument('--warmup', type=int, default=10)
    p.add_argument('--no-foreground', action='store_true')
    a = p.parse_args()
    t0 = time.time()
    net = load_model()
    names = sorted(x.name for x in (a.src / 'frames').glob('*.png'))
    frames = [np.asarray(Image.open(a.src / 'frames' / n).convert('RGB')) for n in names]
    sams = [np.asarray(Image.open(a.src / 'masks' / 'frames' / n).convert('L')) > 127 for n in names]
    # one sequence per orbit ring: frames of extra rings carry a prefix ("e30_00012.png"), ring 0 none
    seqs = {}
    for i, n in enumerate(names):
        seqs.setdefault(n.split('_')[0] if '_' in n else '', []).append(i)
    log(f'matting {len(frames)} frames in {len(seqs)} ring(s) at {frames[0].shape[1]}x{frames[0].shape[0]}')
    mattes = [None] * len(names)
    for idx in seqs.values():
        for i, m in zip(idx, matte_sequence(net, [frames[i] for i in idx], sams[idx[0]].astype(np.uint8) * 255,
                                            a.warmup)):
            mattes[i] = m
    stats = []
    for i, (n, f, s, m) in enumerate(zip(names, frames, sams, mattes)):
        alpha = fuse(s, m, a.band)
        rgb = f if a.no_foreground else foreground(f, alpha)
        save(rgb, alpha, a.out / 'frames' / n, a.out / 'masks' / 'frames' / n)
        soft = ((alpha > 0.02) & (alpha < 0.98)).mean()
        stats.append(float(soft))
        if i % 10 == 0:
            log(f'PROGRESS {i + 1}/{len(names)}')
    hero = a.src / 'hero' / 'hero.png'
    if hero.exists():
        f = np.asarray(Image.open(hero).convert('RGB'))
        s = np.asarray(Image.open(a.src / 'masks' / 'hero' / 'hero.png').convert('L')) > 127
        log(f'matting hero at {f.shape[1]}x{f.shape[0]}')
        m = matte_sequence(net, [f], s.astype(np.uint8) * 255, a.warmup)[0]
        alpha = fuse(s, m, round(a.band * f.shape[0] / frames[0].shape[0]))
        rgb = f if a.no_foreground else foreground(f, alpha)
        save(rgb, alpha, a.out / 'hero' / 'hero.png', a.out / 'masks' / 'hero' / 'hero.png')
    (a.out / 'matte.json').write_text(json.dumps({'frames': len(names), 'soft_edge_share_mean': float(np.mean(stats)),
                                                  'seconds': round(time.time() - t0)}, indent=2))
    log(f'DONE in {time.time() - t0:.0f}s')


if __name__ == '__main__':
    main()
