"""Processing stages: frames -> upscale -> masks -> matte -> camera poses -> training -> clean -> export."""
import json, math, os, re, shutil, subprocess, time
from pathlib import Path

import numpy as np, torch
from PIL import Image

import comfy.model_management as mm
import comfy.utils
import nodes as comfy_nodes

from .common import (CAT, COLMAP, CUDA_ENV, FLIP, LFS, PY_GSPLAT, PY_MATTING, ROOT, TOOLS, contact_sheet,
                     frame_names, free_gpu, link_tree, load_batch, load_sam3, load_unet, load_vae, log, model_choice,
                     out_root, ring_of, rings, run_dir, run_tool, stage, to_uint8)
from .generate import select_frames


# ---------------------------------------------------------------- PS Add Ring

@stage(lambda kw: f'{Path(kw["frames"]["dir"]).name}+{kw["ring_name"]}')
class PSAddRing:
    DESCRIPTION = ('Adds a second (third, ...) orbit ring to the frames: an H3 orbit started from a re-angled hero '
                   '(e.g. 30 degrees above, or a close-up). One ring at eye level leaves the top, the underside and '
                   'close-ups unseen, and the splat breaks there. The ring\'s frames are named <ring>_NNNNN.png; '
                   'later steps treat each ring as its own sequence and give it its own camera.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'frames': ('PS_FRAMES',), 'ring': ('IMAGE',),
            'ring_name': ('STRING', {'default': 'e30', 'tooltip': 'short id, letters/digits only'}),
            'static_ssim': ('FLOAT', {'default': 0.98, 'min': 0.5, 'max': 1.0, 'step': 0.001}),
            'duplicate_ssim': ('FLOAT', {'default': 0.985, 'min': 0.5, 'max': 1.0, 'step': 0.001}),
        }}

    RETURN_TYPES = ('PS_FRAMES', 'IMAGE'); RETURN_NAMES = ('frames', 'preview')
    FUNCTION = 'run'; CATEGORY = CAT

    def run(self, frames, ring, ring_name, static_ssim, duplicate_ssim):
        if not re.fullmatch(r'[A-Za-z0-9]+', ring_name):
            raise ValueError('ring_name: letters and digits only')
        src = Path(frames['dir'])
        out = run_dir(frames['run'], f'{src.name}+{ring_name}')
        link_tree(src, out)
        kept, info = select_frames(ring, static_ssim, duplicate_ssim)
        for i in kept:
            Image.fromarray(to_uint8(ring[i, ..., :3])).save(out / 'frames' / f'{ring_name}_{i:05d}.png')
        (out / f'frames_{ring_name}.json').write_text(json.dumps(info, indent=2))
        log(f'ring {ring_name}: {info}')
        return (dict(frames, dir=str(out)),
                contact_sheet(sorted((out / 'frames').glob(f'{ring_name}_*.png'))))


# ---------------------------------------------------------------- PS Upscale SeedVR2

@stage('upscale')
class PSUpscaleSeedVR2:
    DESCRIPTION = ('SeedVR2 video super-resolution of the orbit frames (not the hero, which is already a sharp photo), '
                   'in chunks: each chunk starts with a few already-upscaled context frames so the temporal window '
                   'never restarts cold, and only the new frames are kept.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'frames': ('PS_FRAMES',),
            'model': model_choice('diffusion_models', 'seedvr2/seedvr2_7b_fp16.safetensors'),
            'vae': model_choice('vae', 'seedvr2_ema_vae_fp16.safetensors'),
            'scale': ('FLOAT', {'default': 2.0, 'min': 1.0, 'max': 4.0, 'step': 0.25}),
            'chunk': ('INT', {'default': 41, 'min': 5, 'max': 201, 'step': 4, 'tooltip': 'new frames per chunk (4n+1)'}),
            'context': ('INT', {'default': 4, 'min': 0, 'max': 16, 'tooltip': 'lead-in frames from the previous chunk'}),
            'color_correction': (['lab', 'wavelet', 'adain', 'none'], {'default': 'lab'}),
            'seed': ('INT', {'default': 1, 'min': 0, 'max': 2 ** 31, 'control_after_generate': True}),
            'tile_size': ('INT', {'default': 512, 'min': 256, 'max': 2048, 'step': 64}),
            'tile_overlap': ('INT', {'default': 128, 'min': 0, 'max': 512, 'step': 32}),
        }}

    RETURN_TYPES = ('PS_FRAMES', 'IMAGE'); RETURN_NAMES = ('frames', 'preview')
    FUNCTION = 'run'; CATEGORY = CAT

    def run(self, frames, model, vae, scale, chunk, context, color_correction, seed, tile_size, tile_overlap):
        from comfy_extras.nodes_seedvr import (SeedVR2Preprocess, SeedVR2PostProcessing, SeedVR2Conditioning,
                                               SeedVR2TemporalChunk, SeedVR2TemporalMerge)
        src = Path(frames['dir']); out = run_dir(frames['run'], 'upscale')
        free_gpu()
        model, vae = load_unet(model), load_vae(vae)
        (out / 'frames').mkdir()
        if (src / 'hero').exists():
            link_tree(src / 'hero', out / 'hero')
        all_names = frame_names(frames)
        w0, h0 = Image.open(src / 'frames' / all_names[0]).size
        W, H = round(w0 * scale / 16) * 16, round(h0 * scale / 16) * 16
        bar = comfy.utils.ProgressBar(len(all_names))
        done = 0
        t0 = time.time()
        for ring_id, names in rings(all_names).items():  # each ring is its own video
          start = 0
          while start < len(names):
            lo = max(0, start - context); hi = min(len(names), start + chunk)
            x = load_batch([src / 'frames' / n for n in names[lo:hi]])
            x = comfy.utils.common_upscale(x.movedim(-1, 1), W, H, 'lanczos', 'disabled').movedim(1, -1).clamp(0, 1)
            pre = SeedVR2Preprocess.execute(x).args[0]
            lat = comfy_nodes.VAEEncodeTiled().encode(vae, pre, tile_size, tile_overlap, 64, 8)[0]
            parts, ov = SeedVR2TemporalChunk.execute(lat, 2, {'chunking_mode': 'auto'}).args
            sampled = []
            for part in parts:
                pos, neg = SeedVR2Conditioning.execute(model, part).args
                sampled.append(comfy_nodes.common_ksampler(model, seed, 1, 1.0, 'euler', 'simple', pos, neg, part,
                                                           denoise=1.0)[0])
            merged = SeedVR2TemporalMerge.execute(sampled, [ov]).args[0] if len(sampled) > 1 else sampled[0]
            dec = comfy_nodes.VAEDecodeTiled().decode(vae, merged, tile_size, tile_overlap, 64, 8)[0]
            post = SeedVR2PostProcessing.execute(dec, x, color_correction).args[0]
            post = post.reshape(-1, *post.shape[-3:])
            assert post.shape[0] >= hi - lo, (post.shape, hi - lo)
            for k, i in enumerate(range(lo, hi)):
                if i >= start:
                    Image.fromarray(to_uint8(post[k, :H, :W, :3])).save(out / 'frames' / names[i])
            log(f'SeedVR2 ring {ring_id or "0"} frames {start}-{hi - 1} (context from {lo}) -> {W}x{H}, '
                f'{time.time() - t0:.0f}s')
            done += hi - start
            bar.update_absolute(done, len(all_names))
            start = hi
            del x, pre, lat, parts, sampled, merged, dec, post
            mm.soft_empty_cache()
        fr = dict(frames, dir=str(out))
        return (fr, contact_sheet(sorted((out / 'frames').glob('*.png'))))


# ---------------------------------------------------------------- PS Segment SAM3

@stage('masks')
class PSSegmentSAM3:
    DESCRIPTION = ('Person masks for every frame and the hero with SAM 3.1 (text prompt). Keeps the largest '
                   'region and everything attached to it, so stray detections in the background drop out.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'frames': ('PS_FRAMES',),
            'sam3': model_choice('checkpoints', 'sam3.1_multiplex_fp16.safetensors'),
            'prompt': ('STRING', {'default': 'person'}),
            'threshold': ('FLOAT', {'default': 0.5, 'min': 0.05, 'max': 0.95, 'step': 0.01}),
            'refine_iterations': ('INT', {'default': 3, 'min': 0, 'max': 5}),
            'batch': ('INT', {'default': 8, 'min': 1, 'max': 64}),
        }}

    RETURN_TYPES = ('PS_FRAMES', 'IMAGE'); RETURN_NAMES = ('frames', 'preview')
    FUNCTION = 'run'; CATEGORY = CAT

    @staticmethod
    def _clean(m):
        from scipy import ndimage
        lab, n = ndimage.label(m)
        if n <= 1:
            return m
        sizes = ndimage.sum_labels(m, lab, range(1, n + 1))
        keep = np.flatnonzero(sizes >= 0.02 * sizes.max()) + 1
        return np.isin(lab, keep)

    def run(self, frames, sam3, prompt, threshold, refine_iterations, batch):
        from comfy_extras.nodes_sam3 import SAM3_Detect
        src = Path(frames['dir']); out = run_dir(frames['run'], 'masks')
        free_gpu()
        model, clip = load_sam3(sam3)
        link_tree(src, out)
        cond = comfy_nodes.CLIPTextEncode().encode(clip, prompt)[0]
        groups = [('frames', frame_names(frames))]
        if (src / 'hero' / 'hero.png').exists():
            groups.append(('hero', ['hero.png']))
        total = sum(len(g[1]) for g in groups); done = 0
        bar = comfy.utils.ProgressBar(total)
        areas = []
        for group, names in groups:
            (out / 'masks' / group).mkdir(parents=True, exist_ok=True)
            for b in range(0, len(names), batch):
                part = names[b:b + batch]
                x = load_batch([src / group / n for n in part])
                masks = SAM3_Detect.execute(model, x, conditioning=cond, threshold=threshold,
                                            refine_iterations=refine_iterations, individual_masks=False).args[0]
                for n, m in zip(part, masks):
                    m = self._clean(m.cpu().numpy() > 0.5)
                    if group == 'frames':
                        areas.append(m.mean())
                    Image.fromarray((m * 255).astype(np.uint8)).save(out / 'masks' / group / n)
                done += len(part); bar.update_absolute(done, total)
        parts = self.hero_parts(model, clip, src / 'hero' / 'hero.png', out / 'hero_parts.json')
        areas = np.array(areas)
        bad = int((areas < 0.25 * np.median(areas)).sum())
        info = {'n_masks': total, 'mean_area': float(areas.mean()), 'area_cv': float(areas.std() / areas.mean()),
                'suspicious_small': bad}
        (out / 'masks.json').write_text(json.dumps(info, indent=2)); log(f'masks: {info}')
        names = frame_names(frames)
        fr = dict(frames, dir=str(out))
        if parts:
            fr['hero_parts'] = str(out / 'hero_parts.json')
        return (fr, contact_sheet([out / 'frames' / n for n in names],
                                  [out / 'masks' / 'frames' / n for n in names]))

    @staticmethod
    def hero_parts(model, clip, hero, out_json, prompts=(('face', 'face'), ('hand', 'hand:2')), threshold=0.4):
        """Boxes of the face and hands in the hero photo (pixels), for full-resolution detail crops."""
        from comfy_extras.nodes_sam3 import SAM3_Detect
        if not hero.exists():
            return None
        x = load_batch([hero]); H, W = x.shape[1:3]
        boxes = []
        for label, prompt in prompts:
            cond = comfy_nodes.CLIPTextEncode().encode(clip, prompt)[0]
            res = SAM3_Detect.execute(model, x, conditioning=cond, threshold=threshold, refine_iterations=1,
                                      individual_masks=True).args
            boxes += [dict(b, label=label) for b in res[1][0]]
        out_json.write_text(json.dumps({'width': W, 'height': H, 'parts': boxes}, indent=2))
        log(f'hero parts: {[(b["label"], round(b["width"]), round(b["height"]), round(b["score"], 2)) for b in boxes]}')
        return boxes


# ---------------------------------------------------------------- PS Matte

@stage('matte')
class PSMatte:
    DESCRIPTION = ('Soft alpha with MatAnyone 2 (hair, fingertips) fused with the SAM masks: SAM decides away from '
                   'the edge, the matte decides in a narrow band around it. Edge colors are re-estimated so the '
                   'studio white does not train in as a fringe.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'frames': ('PS_FRAMES',),
            'band': ('INT', {'default': 12, 'min': 2, 'max': 64, 'tooltip': 'edge band (px at frame size) the matte may change'}),
            'warmup': ('INT', {'default': 10, 'min': 0, 'max': 50}),
            'estimate_foreground': ('BOOLEAN', {'default': True}),
        }}

    RETURN_TYPES = ('PS_FRAMES', 'IMAGE'); RETURN_NAMES = ('frames', 'preview')
    FUNCTION = 'run'; CATEGORY = CAT

    def run(self, frames, band, warmup, estimate_foreground):
        src = Path(frames['dir']); out = run_dir(frames['run'], 'matte')
        free_gpu()
        cmd = [PY_MATTING, TOOLS / 'matte.py', src, out, '--band', band, '--warmup', warmup]
        if not estimate_foreground:
            cmd.append('--no-foreground')
        run_tool(cmd, total=len(frame_names(frames)), logfile=out / 'matte.log')
        names = frame_names(frames)
        return (dict(frames, dir=str(out)), contact_sheet([out / 'frames' / n for n in names],
                                                         [out / 'masks' / 'frames' / n for n in names]))


# ---------------------------------------------------------------- PS Camera Poses

def read_cameras_txt(p):
    cams = {}
    for line in Path(p).read_text().splitlines():
        if line.startswith('#') or not line.strip():
            continue
        t = line.split(); cams[int(t[0])] = t
    return cams


def read_images_txt(p):
    lines = [l for l in Path(p).read_text().splitlines() if not l.startswith('#')]
    ims = {}
    for i in range(0, len(lines) - 1, 2):
        t = lines[i].split()
        if not t:
            continue
        ims[int(t[0])] = dict(q=np.array(list(map(float, t[1:5]))), t=np.array(list(map(float, t[5:8]))),
                              cam=int(t[8]), name=t[9], pts=lines[i + 1])
    return ims


def read_points_txt(p):
    pts = {}
    for line in Path(p).read_text().splitlines():
        if line.startswith('#') or not line.strip():
            continue
        t = line.split()
        pts[int(t[0])] = (np.array(list(map(float, t[1:4]))), t[4:7], t[7], t[8:])
    return pts


def qvec2rot(q):
    w, x, y, z = q / np.linalg.norm(q)
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                     [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                     [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])


def rot2qvec(R):
    from scipy.spatial.transform import Rotation
    x, y, z, w = Rotation.from_matrix(R).as_quat()
    q = np.array([w, x, y, z])
    return q if q[0] >= 0 else -q


@stage('dataset')
class PSCameraPoses:
    DESCRIPTION = ('Camera poses with COLMAP (SIFT with affine shape + domain-size pooling, exhaustive guided '
                   'matching, incremental mapping), features only on the person. The model is then put in the '
                   'canonical frame: orbit axis up, hero view facing +Z, person standing at the origin at '
                   'height_m - written in the 3DGS PLY convention, so the trained splat needs no rotation later '
                   '(rotating a splat would also need its spherical harmonics rotated). The hero photo is added '
                   'hero_copies times in total, so training sees its detail (the face) more often.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'frames': ('PS_FRAMES',),
            'height_m': ('FLOAT', {'default': 1.78, 'min': 0.3, 'max': 2.5, 'step': 0.01}),
            'hero_copies': ('INT', {'default': 3, 'min': 1, 'max': 10}),
            'max_features': ('INT', {'default': 16384, 'min': 1024, 'max': 65536}),
        }, 'optional': {
            'detail_copies': ('INT', {'default': 2, 'min': 0, 'max': 6,
                                      'tooltip': 'full-resolution crops of the hero\'s face and hands (found by PS Segment '
                                                 'SAM3), each added this many times as extra views with the hero\'s pose'}),
        }}

    RETURN_TYPES = ('PS_DATASET',); RETURN_NAMES = ('dataset',)
    FUNCTION = 'run'; CATEGORY = CAT

    @staticmethod
    def detail_crops(frames, hero_cam, margin=0.45, min_side=640):
        """Crop windows (hero pixels) around the face and hands found by PS Segment SAM3."""
        p = frames.get('hero_parts')
        if hero_cam is None or not p or not Path(p).exists():
            return []
        info = json.loads(Path(p).read_text())
        model, W, H, params = hero_cam[1], int(hero_cam[2]), int(hero_cam[3]), [float(x) for x in hero_cam[4:]]
        if model != 'SIMPLE_PINHOLE':
            log(f'detail crops skipped: hero camera is {model}')
            return []
        sx, sy = W / info['width'], H / info['height']  # the matted hero has the photo's size; be safe anyway
        f, cx, cy = params[:3]
        out, n = [], {}
        for b in info['parts']:
            bw, bh = b['width'] * sx, b['height'] * sy
            side = max(bw, bh) * (1 + 2 * margin)
            w = max(min_side, side * 0.75 if bh >= bw else side); h = max(min_side, side if bh >= bw else side * 0.75)
            mx, my = (b['x'] + b['width'] / 2) * sx, (b['y'] + b['height'] / 2) * sy
            x0 = int(round(min(max(0, mx - w / 2), W - w))); y0 = int(round(min(max(0, my - h / 2), H - h)))
            x1 = int(min(W, x0 + round(w))); y1 = int(min(H, y0 + round(h)))
            k = n[b['label']] = n.get(b['label'], 0) + 1
            out.append({'name': f'{b["label"]}{k}', 'box': [max(0, x0), max(0, y0), x1, y1], 'f': f, 'cx': cx, 'cy': cy})
        log(f'detail crops: {[(c["name"], c["box"]) for c in out]}')
        return out

    def colmap(self, work, *args):
        r = subprocess.run([COLMAP, *map(str, args)], capture_output=True, text=True)
        with open(work / 'colmap.log', 'a') as f:
            f.write(f'\n==== colmap {" ".join(map(str, args))}\n{r.stdout}{r.stderr}')
        if r.returncode != 0:
            raise RuntimeError(f'colmap {args[0]} failed:\n' + '\n'.join((r.stderr or r.stdout).splitlines()[-10:]))
        return r.stdout + r.stderr

    def run(self, frames, height_m, hero_copies, max_features, detail_copies=2):
        src = Path(frames['dir']); out = run_dir(frames['run'], 'dataset')
        free_gpu()
        work = out / 'colmap'; (work / 'sparse').mkdir(parents=True)
        bar = comfy.utils.ProgressBar(5)
        # COLMAP sees the original pixels (the matted RGB is only for training) and binary masks
        # one COLMAP folder (= one camera) per ring: a re-angled ring may have its own focal length
        to_src = {}
        for n in frame_names(frames):
            to_src[f'frames{"_" + ring_of(n) if ring_of(n) else ""}/{n}'] = f'frames/{n}'
        if (src / 'hero' / 'hero.png').exists():
            to_src['hero/hero.png'] = 'hero/hero.png'
        names = list(to_src)
        for n in names:
            for sub, folder in (('images', ''), ('masks', 'masks/')):
                q = work / sub / (n + ('.png' if sub == 'masks' else ''))
                q.parent.mkdir(parents=True, exist_ok=True)
                if sub == 'images':
                    os.link(src / to_src[n], q)
                else:
                    m = np.asarray(Image.open(src / 'masks' / to_src[n]).convert('L')) > 127
                    Image.fromarray((m * 255).astype(np.uint8)).save(q)
        db = work / 'database.db'
        self.colmap(work, 'feature_extractor', '--database_path', db, '--image_path', work / 'images',
                    '--ImageReader.mask_path', work / 'masks', '--ImageReader.camera_model', 'SIMPLE_PINHOLE',
                    '--ImageReader.single_camera_per_folder', 1, '--FeatureExtraction.use_gpu', 0,
                    '--SiftExtraction.max_num_features', max_features, '--SiftExtraction.estimate_affine_shape', 1,
                    '--SiftExtraction.domain_size_pooling', 1)
        bar.update_absolute(1)
        try:
            self.colmap(work, 'exhaustive_matcher', '--database_path', db, '--FeatureMatching.use_gpu', 0,
                        '--FeatureMatching.guided_matching', 1)
        except RuntimeError:
            # COLMAP 4.1's guided matching aborts on some pairs (seen with three orbit rings); the pairs it
            # finished stay in the database and the rerun matches only the rest, unguided
            log('guided matching crashed, finishing the remaining pairs unguided')
            self.colmap(work, 'exhaustive_matcher', '--database_path', db, '--FeatureMatching.use_gpu', 0,
                        '--FeatureMatching.guided_matching', 0)
        bar.update_absolute(2)
        self.colmap(work, 'mapper', '--database_path', db, '--image_path', work / 'images', '--output_path', work / 'sparse')
        bar.update_absolute(3)
        models = [p for p in (work / 'sparse').iterdir() if p.is_dir()]
        best, best_n = None, -1
        for mdl in models:
            txt = work / f'txt_{mdl.name}'; txt.mkdir()
            self.colmap(work, 'model_converter', '--input_path', mdl, '--output_path', txt, '--output_type', 'TXT')
            n = len(read_images_txt(txt / 'images.txt'))
            if n > best_n:
                best, best_n = txt, n
        if best is None:
            raise RuntimeError('COLMAP produced no model')
        cams, ims, pts = read_cameras_txt(best / 'cameras.txt'), read_images_txt(best / 'images.txt'), read_points_txt(best / 'points3D.txt')
        hero_id = next((i for i, v in ims.items() if v['name'].startswith('hero/')), None)
        stats = self.colmap(work, 'model_analyzer', '--path', best)
        reproj = re.search(r'Mean reprojection error:\s*([\d.]+)', stats)
        bar.update_absolute(4)

        # canonical frame: up = normal of the camera ring signed by the images' up; front = toward the hero
        centers, rots = {}, {}
        for i, v in ims.items():
            R = qvec2rot(v['q']); rots[i] = R; centers[i] = -R.T @ v['t']
        fids = [i for i, v in ims.items() if v['name'].startswith('frames/')]  # the eye-level ring
        allf = [i for i, v in ims.items() if v['name'].startswith('frames')]
        C = np.array([centers[i] for i in allf]); A = np.array([rots[i][2] for i in allf])
        proj = np.eye(3)[None] - A[:, :, None] * A[:, None, :]
        center = np.linalg.lstsq(proj.sum(0), np.einsum('nij,nj->i', proj, C), rcond=None)[0]
        C0 = np.array([centers[i] for i in fids])
        up = np.linalg.svd(C0 - C0.mean(0))[2][2]
        # the generated camera never rolls, so its right axis stays horizontal at any pitch: up is also the direction
        # orthogonal to all of them. For a level ring both agree; a 'wave' orbit (climbing over the head behind the
        # person) is a tilted circle whose normal is not up - then trust the cameras' horizontal axes
        up_h = np.linalg.svd(np.array([rots[i][0] for i in allf]))[2][2]
        tilt = math.degrees(math.acos(min(1.0, abs(float(up @ up_h)))))
        if tilt > 10:
            up = up_h
            log(f'camera orbit is tilted {tilt:.0f} deg from level: up from the cameras\' horizontal axes')
        if up @ (-np.mean([rots[i][1] for i in fids], 0)) < 0:
            up = -up
        front = centers[hero_id if hero_id is not None else fids[0]] - center
        front -= (front @ up) * up; front /= np.linalg.norm(front)
        M = np.stack([np.cross(up, front), up, front])  # rows: viewer x, y (up), z (front)
        P = np.array([p[0] for p in pts.values()])
        local = (P - center) @ M.T
        lo, hi = np.percentile(local, 0.5, 0), np.percentile(local, 99.5, 0)
        s = height_m / (hi[1] - lo[1])
        offset = -s * np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])
        # file = FLIP @ (s M (X - c) + offset) = A X + b, A = s Q, Q = FLIP M
        Q = FLIP @ M; b = FLIP @ (offset - s * M @ center)
        ds = out / 'sparse' / '0'; ds.mkdir(parents=True)

        def rgba(name, dst):
            dst.parent.mkdir(parents=True, exist_ok=True)
            rgb = np.asarray(Image.open(src / to_src[name]).convert('RGB'))
            a = np.asarray(Image.open(src / 'masks' / to_src[name]).convert('L'))
            Image.fromarray(np.dstack([rgb, a])).save(dst)

        lines = []
        next_id = max(ims) + 1
        cam_lines = [l for l in (best / 'cameras.txt').read_text().splitlines() if l.strip() and not l.startswith('#')]
        crops = self.detail_crops(frames, cams[ims[hero_id]['cam']] if hero_id is not None else None) \
            if detail_copies > 0 else []
        next_cam = max(cams) + 1
        for i, v in sorted(ims.items()):
            R = rots[i] @ Q.T; T = s * v['t'] - R @ b
            q = rot2qvec(R)
            entries = [(i, v['name'], v['cam'])]
            if i == hero_id:
                entries += [(next_id + k, f'hero/hero_c{k + 1}.png', v['cam']) for k in range(hero_copies - 1)]
                next_id += hero_copies - 1
            for j, nm, cam in entries:
                lines.append(f'{j} {" ".join(f"{x:.12g}" for x in q)} {" ".join(f"{x:.12g}" for x in T)} {cam} {nm}')
                lines.append(v['pts'] if j == i else '')
                rgba(v['name'], out / 'images' / nm)
            if i == hero_id:
                # a crop is the same camera with a smaller window: same pose and focal length, shifted principal point
                hero_rgba = np.asarray(Image.open(out / 'images' / v['name']))
                for c in crops:
                    x0, y0, x1, y1 = c['box']
                    cam_lines.append(f'{next_cam} SIMPLE_PINHOLE {x1 - x0} {y1 - y0} {c["f"]:.12g} '
                                     f'{c["cx"] - x0:.12g} {c["cy"] - y0:.12g}')
                    for k in range(detail_copies):
                        nm = f'detail/{c["name"]}_{k + 1}.png'
                        (out / 'images' / nm).parent.mkdir(parents=True, exist_ok=True)
                        Image.fromarray(hero_rgba[y0:y1, x0:x1]).save(out / 'images' / nm)
                        lines.append(f'{next_id} {" ".join(f"{x:.12g}" for x in q)} {" ".join(f"{x:.12g}" for x in T)} '
                                     f'{next_cam} {nm}')
                        lines.append('')
                        next_id += 1
                    next_cam += 1
        (ds / 'images.txt').write_text('# Image list\n' + '\n'.join(lines) + '\n')
        (ds / 'cameras.txt').write_text('# Camera list\n' + '\n'.join(cam_lines) + '\n')
        with open(ds / 'points3D.txt', 'w') as f:
            f.write('# 3D point list\n')
            for pid, (xyz, rgb, err, track) in pts.items():
                X = s * Q @ xyz + b
                f.write(f'{pid} {X[0]:.9g} {X[1]:.9g} {X[2]:.9g} {" ".join(rgb)} {err} {" ".join(track)}\n')
        per_ring = {}
        for v in ims.values():
            k = v['name'].split('/')[0]; per_ring[k] = per_ring.get(k, 0) + 1
        info = {'n_images': len(names), 'n_registered': len(ims), 'registered_per_folder': per_ring,
                'hero_registered': hero_id is not None,
                'reproj_err_px': float(reproj.group(1)) if reproj else None, 'n_points': len(pts),
                'scale': s, 'hero_copies': hero_copies,
                'detail_crops': [dict(name=c['name'], box=c['box']) for c in crops], 'detail_copies': detail_copies,
                'transform': {'convention': 'file = FLIP @ (scale * R @ (world - center) + offset)',
                              'center': center.tolist(), 'R': M.tolist(), 'scale': s, 'offset': offset.tolist()}}
        (out / 'dataset.json').write_text(json.dumps(info, indent=2))
        log(f"poses: {len(ims)}/{len(names)} registered, hero {'yes' if hero_id is not None else 'NO'}, "
            f"reprojection {info['reproj_err_px']} px, {len(pts)} points")
        if len(ims) < 0.8 * len(names):
            raise RuntimeError(f'COLMAP registered only {len(ims)} of {len(names)} images')
        bar.update_absolute(5)
        return ({'run': frames['run'], 'dir': str(out), 'height_m': height_m},)


# ---------------------------------------------------------------- PS Train Splat

# keys LichtFeld insists on in a --config file, at its MRNF defaults (src/core/parameters.cpp mrnf_defaults); the rest
# of the file only overrides what we set
MRNF_REQUIRED = {'iterations': 30000, 'lambda_dssim': 0.22, 'means_lr': 2.17871e-5, 'min_opacity': 1 / 255,
                 'opacity_lr': 0.012, 'refine_every': 163, 'rotation_lr': 0.0015, 'scaling_lr': 0.00828016,
                 'shs_lr': -1.0, 'start_refine': 0, 'stop_refine': 28500}


@stage('train')
class PSTrainSplat:
    DESCRIPTION = ('Trains the splat with LichtFeld Studio, headless. Images carry the soft alpha, so every '
                   'step composites them over a random background: the person has to be opaque on its own and '
                   'edges keep their real transparency. MRNF densifies where the error map says detail is '
                   'missing; pose refinement absorbs the small camera errors of generated frames; per-photo '
                   'exposure correction absorbs their color drift; MoGe-2 normals keep surfaces from turning '
                   'into stacked, see-through layers in the views the orbit never saw.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'dataset': ('PS_DATASET',),
            'strategy': (['mrnf', 'mcmc', 'igs+'], {'default': 'mrnf'}),
            'max_splats': ('INT', {'default': 150000, 'min': 10000, 'max': 5000000, 'step': 10000}),
            'steps_scaler': ('FLOAT', {'default': 2.0, 'min': 0.25, 'max': 8.0, 'step': 0.25,
                                       'tooltip': '1 = 30k iterations'}),
            'sh_degree': ('INT', {'default': 3, 'min': 0, 'max': 3}),
            'pose_opt': ('BOOLEAN', {'default': True}),
            'normal_prior': ('BOOLEAN', {'default': True}),
            'mip_filter': ('BOOLEAN', {'default': True}),
            'thin_structure_weight': ('FLOAT', {'default': 1.0, 'min': 0.0, 'max': 2.0, 'step': 0.1,
                                                'tooltip': 'fingers, hair strands'}),
            'extra_args': ('STRING', {'default': '', 'multiline': True}),
        }, 'optional': {
            'spike_penalty': ('FLOAT', {'default': 0.001, 'min': 0.0, 'max': 0.1, 'step': 0.001,
                                        'tooltip': 'erank regularizer: penalizes needle-shaped splats that turn into '
                                                   'streaks from new angles (LichtFeld MRNF default 0.001)'}),
            'scale_penalty': ('FLOAT', {'default': 0.01, 'min': 0.0, 'max': 0.1, 'step': 0.001,
                                        'tooltip': 'pushes splats smaller (MRNF default 0.01); 0 lets them stay big'}),
            'edge_loss': ('FLOAT', {'default': 1.8, 'min': 0.0, 'max': 4.0, 'step': 0.1,
                                    'tooltip': 'gradient loss on photo edges (MRNF default 1.8); high values breed '
                                               'many tiny splats chasing texture'}),
            'densify_structure': ('FLOAT', {'default': 1.0, 'min': 0.0, 'max': 2.0, 'step': 0.1,
                                            'tooltip': 'priority of thin lines when adding splats (MRNF default 1)'}),
            'view_color_penalty': ('FLOAT', {'default': 0.001, 'min': 0.0, 'max': 0.1, 'step': 0.001,
                                             'tooltip': 'penalizes view-dependent color (SH bands > 0), which '
                                                        'overfits to the few generated views'}),
        }}

    RETURN_TYPES = ('PS_SPLAT',); RETURN_NAMES = ('splat',)
    FUNCTION = 'run'; CATEGORY = CAT

    def run(self, dataset, strategy, max_splats, steps_scaler, sh_degree, pose_opt, normal_prior, mip_filter,
            thin_structure_weight, extra_args, spike_penalty=0.001, scale_penalty=0.01, edge_loss=1.8,
            densify_structure=1.0, view_color_penalty=0.001):
        out = run_dir(dataset['run'], 'train')
        free_gpu()
        # regularizers without CLI flags go through a partial config: LFS starts from the strategy's defaults
        cfg = out / 'lfs_config.json'
        cfg.write_text(json.dumps(dict(MRNF_REQUIRED, strategy=strategy, sh_degree=sh_degree, erank_reg=spike_penalty,
                                       scale_reg=scale_penalty, sh_rest_reg=view_color_penalty), indent=1))
        cmd = [LFS, '--headless', '--train', '--no-splash', '-d', dataset['dir'], '-o', out, '--output-name', 'splat',
               '--images', 'images', '--strategy', strategy, '--max-cap', max_splats, '--steps-scaler', steps_scaler,
               '--sh-degree', sh_degree, '-r', 1, '--max-width', 0, '--bg-mode', 'random',
               '--thin-structure-weight', thin_structure_weight, '--gradient-loss-weight', edge_loss,
               '--densify-structure-weight', densify_structure] + (['--config', cfg] if strategy == 'mrnf' else []) + [
               '--export', 'ply', '--no-provenance']
        if pose_opt:
            cmd.append('--pose-opt')
        if mip_filter:
            cmd.append('--enable-mip')
        if normal_prior:
            cmd.append('--use-normal-loss')
        cmd += extra_args.split()
        run_tool(cmd, total=int(30000 * steps_scaler), logfile=out / 'train.log')
        plys = sorted(out.rglob('*.ply'), key=lambda p: p.stat().st_mtime)
        if not plys:
            raise RuntimeError(f'LichtFeld wrote no PLY into {out}')
        log(f'trained: {plys[-1]}')
        return ({'run': dataset['run'], 'ply': str(plys[-1]), 'height_m': dataset['height_m'], 'dataset': dataset['dir']},)


# ---------------------------------------------------------------- PS Clean Splat

def read_ply(path):
    f = open(path, 'rb'); h = b''
    while not h.endswith(b'end_header\n'):
        h += f.readline()
    lines = h.decode().split('\n')
    n = int(next(l for l in lines if l.startswith('element vertex')).split()[-1])
    props = [l.split()[-1] for l in lines if l.startswith('property')]
    types = [l.split()[1] for l in lines if l.startswith('property')]
    assert all(t == 'float' for t in types), f'non-float PLY properties: {set(types)}'
    a = np.frombuffer(f.read(n * 4 * len(props)), np.float32).reshape(n, -1).copy()
    return props, a


def write_ply(path, props, a):
    with open(path, 'wb') as f:
        f.write(('ply\nformat binary_little_endian 1.0\n' + f'element vertex {len(a)}\n' +
                 ''.join(f'property float {p}\n' for p in props) + 'end_header\n').encode())
        f.write(a.astype(np.float32).tobytes())


@stage('clean')
class PSCleanSplat:
    DESCRIPTION = ('Puts the feet at y = 0 and the person at height_m (from the splat itself, more complete than '
                   'the sparse points), then removes: splats far outside the body (floaters), near-invisible ones, '
                   'and splats of the upper garment that hang over the lower one. A camera ring at one height sees '
                   'those edge-on and never corrects them; from above they show as a smear over the legs.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'splat': ('PS_SPLAT',),
            'min_opacity': ('FLOAT', {'default': 0.01, 'min': 0.0, 'max': 0.5, 'step': 0.005}),
            'outlier_m': ('FLOAT', {'default': 0.08, 'min': 0.01, 'max': 1.0, 'step': 0.01,
                                    'tooltip': 'remove splats this far outside the body silhouette envelope'}),
            'hanging_margin_m': ('FLOAT', {'default': 0.02, 'min': 0.0, 'max': 0.2, 'step': 0.005}),
            'remove_hanging': ('BOOLEAN', {'default': True}),
        }}

    RETURN_TYPES = ('PS_SPLAT', 'STRING'); RETURN_NAMES = ('splat', 'report')
    FUNCTION = 'run'; CATEGORY = CAT

    def run(self, splat, min_opacity, outlier_m, hanging_margin_m, remove_hanging):
        from scipy.spatial import cKDTree
        out = run_dir(splat['run'], 'clean')
        props, d = read_ply(splat['ply'])
        col = {k: i for i, k in enumerate(props)}
        n0 = len(d)
        op = 1 / (1 + np.exp(-d[:, col['opacity']]))
        keep = op >= min_opacity
        xyz = d[:, [col['x'], col['y'], col['z']]] @ FLIP  # viewer frame
        solid = keep & (op > 0.5)
        # floaters: far from the solid body (kNN distance to opaque splats)
        tree = cKDTree(xyz[solid])
        dist, _ = tree.query(xyz, k=1)
        far = dist > outlier_m
        # detached clumps: opaque splats far away are each other's neighbours and pass the test above, so keep only
        # the body itself, the largest group of splats chained together at most outlier_m apart
        from scipy.sparse import coo_matrix
        from scipy.sparse.csgraph import connected_components
        idx = np.flatnonzero(keep & ~far)
        pairs = cKDTree(xyz[idx]).query_pairs(outlier_m, output_type='ndarray')
        _, lab = connected_components(coo_matrix((np.ones(len(pairs)), (pairs[:, 0], pairs[:, 1])),
                                                 shape=(len(idx), len(idx))), directed=False)
        far[idx[lab != np.bincount(lab).argmax()]] = True
        keep &= ~far
        # metric height and feet at 0, from the solid splats (robust percentiles)
        y = xyz[:, 1]
        y0, y1 = np.percentile(y[keep & solid], [0.05, 99.95])
        s = splat['height_m'] / (y1 - y0)
        cx, cz = np.median(xyz[keep & solid][:, [0, 2]], 0)
        xyz = (xyz - np.array([cx, y0, cz])) * s
        report = {'n_in': n0, 'faint': int((op < min_opacity).sum()), 'floaters': int((far & (op >= min_opacity)).sum()),
                  'rescale': float(s)}
        if remove_hanging:
            hang, hem = self.hanging(d, col, xyz, keep, hanging_margin_m, s)
            keep &= ~hang
            report.update({'hem_m': hem, 'hanging': int(hang.sum())})
        d[:, [col['x'], col['y'], col['z']]] = xyz @ FLIP
        for i in range(3):
            d[:, col[f'scale_{i}']] += np.log(s)
        d = d[keep]
        report['n_out'] = int(len(d))
        path = out / 'splat.ply'
        write_ply(path, props, d)
        (out / 'clean.json').write_text(json.dumps(report, indent=2)); log(f'clean: {report}')
        return (dict(splat, ply=str(path)), json.dumps(report))

    @staticmethod
    def hanging(d, col, xyz, keep, margin, s):
        """The garment boundary (hem): between 35% and 65% of the height, the 1 cm slice where the mean color of
        the splats changes most. Hanging: centered above it, reaching below it by more than `margin`, and closer
        in color to the upper garment than to the lower one (a hand beside the hips is skin, not shirt)."""
        C0 = 0.28209479177387814
        rgb = 0.5 + C0 * d[:, [col['f_dc_0'], col['f_dc_1'], col['f_dc_2']]]
        y = xyz[:, 1]; h = np.percentile(y[keep], 99.9)
        r = np.linalg.norm(xyz[:, [0, 2]], axis=1)
        torso = keep & (r < 0.25)
        bins = np.arange(0.35 * h, 0.65 * h, 0.01)
        means = np.array([rgb[torso & (y >= b) & (y < b + 0.01)].mean(0) if (torso & (y >= b) & (y < b + 0.01)).sum() > 30
                          else np.full(3, np.nan) for b in bins])
        diff = np.array([np.linalg.norm(np.nanmean(means[max(0, i - 6):i], 0) - np.nanmean(means[i:i + 6], 0))
                         if 6 <= i <= len(bins) - 6 else 0 for i in range(len(bins))])
        diff = np.nan_to_num(diff)
        k = int(diff.argmax()); hem = float(bins[k])
        upper = np.nanmean(means[max(0, k - 8):k], 0); lower = np.nanmean(means[k:k + 8], 0)
        if diff[k] < 0.08:  # no clear garment boundary: nothing to do
            return np.zeros(len(d), bool), hem
        q = d[:, [col[f'rot_{i}'] for i in range(4)]]; q = q / np.linalg.norm(q, axis=1, keepdims=True)
        w, qx, qy, qz = q.T
        ry = np.stack([2 * (qx * qy + w * qz), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - w * qx)], 1)
        sc = np.exp(d[:, [col[f'scale_{i}'] for i in range(3)]]) * s  # xyz is metric already, the scales not yet
        reach = 3 * np.sqrt(((ry * sc) ** 2).sum(1))  # 3 sigma along the vertical (ry: the file y row, sign-free)
        like_upper = np.linalg.norm(rgb - upper, axis=1) < np.linalg.norm(rgb - lower, axis=1)
        hang = keep & (y >= hem - margin) & (y - reach < hem - margin) & like_upper & (y < hem + 0.25)
        return hang, hem


# ---------------------------------------------------------------- PS Export Splat

@stage(lambda kw: f'export_{kw["name"]}')
class PSExportSplat:
    DESCRIPTION = ('Writes the splat as PLY (full SH), SPZ, SOG and a self-contained HTML viewer, and renders a '
                   'novel-view sheet (rings at 0, +35 and -25 degrees, a face ring, a legs ring) to check it.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {'splat': ('PS_SPLAT',), 'name': ('STRING', {'default': 'professor'}),
                             'views': ('STRING', {'default': 'ring0,ring35,ring60,ringm25,face,face45,facem20,hands,legs'})}}

    RETURN_TYPES = ('IMAGE', 'STRING'); RETURN_NAMES = ('views', 'folder')
    FUNCTION = 'run'; CATEGORY = CAT; OUTPUT_NODE = True

    def run(self, splat, name, views):
        out = run_dir(splat['run'], f'export_{name}')
        free_gpu()
        shutil.copy(splat['ply'], out / 'splat.ply')
        for fmt in ('spz', 'sog', 'html'):
            run_tool([LFS, 'convert', out / 'splat.ply', out / f'splat.{fmt}'], logfile=out / f'convert_{fmt}.log')
        sheet = out / 'views.png'
        run_tool([PY_GSPLAT, TOOLS / 'splat_views.py', sheet, out / 'splat.ply', '--rows', views,
                  '--scale', '0.5'], env=CUDA_ENV)
        img = torch.from_numpy(np.asarray(Image.open(sheet).convert('RGB'))).float().div(255)[None]
        return {'ui': {'text': [str(out)]}, 'result': (img, str(out))}


# ---------------------------------------------------------------- PS Preview Frames

class PSPreviewFrames:
    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {'frames': ('PS_FRAMES',), 'with_masks': ('BOOLEAN', {'default': True})}}

    RETURN_TYPES = ('IMAGE',); FUNCTION = 'run'; CATEGORY = CAT

    def run(self, frames, with_masks):
        d = Path(frames['dir']); names = frame_names(frames)
        masks = [d / 'masks' / 'frames' / n for n in names] if with_masks and (d / 'masks').exists() else None
        return (contact_sheet([d / 'frames' / n for n in names], masks),)




# ---------------------------------------------------------------- PS Save Frames (any video)

@stage('frames')
class PSSaveFrames:
    DESCRIPTION = ('Starts the pipeline from any orbit video (IMAGE batch, e.g. Load Video) instead of PS Orbit Video: '
                   'drops the static ends and near-duplicates and writes frames/ and hero/.')
    VERSION = 1

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'run': ('PS_RUN',), 'frames': ('IMAGE',), 'hero': ('IMAGE',),
            'static_ssim': ('FLOAT', {'default': 0.98, 'min': 0.5, 'max': 1.0, 'step': 0.001}),
            'duplicate_ssim': ('FLOAT', {'default': 0.985, 'min': 0.5, 'max': 1.0, 'step': 0.001}),
            'every_nth': ('INT', {'default': 1, 'min': 1, 'max': 8}),
        }}

    RETURN_TYPES = ('PS_FRAMES', 'IMAGE'); RETURN_NAMES = ('frames', 'preview')
    FUNCTION = 'run'; CATEGORY = CAT

    def run(self, run, frames, hero, static_ssim, duplicate_ssim, every_nth):
        out = run_dir(run['run'], 'frames')
        kept, info = select_frames(frames, static_ssim, duplicate_ssim)
        kept = kept[::every_nth]; info['n_frames'] = len(kept)
        (out / 'raw').mkdir(); (out / 'frames').mkdir(); (out / 'hero').mkdir()
        for i in range(len(frames)):
            Image.fromarray(to_uint8(frames[i, ..., :3])).save(out / 'raw' / f'{i:05d}.png')
        for i in kept:
            os.link(out / 'raw' / f'{i:05d}.png', out / 'frames' / f'{i:05d}.png')
        Image.fromarray(to_uint8(hero[0, ..., :3])).save(out / 'hero' / 'hero.png')
        (out / 'frames.json').write_text(json.dumps(info, indent=2))
        return ({'run': run['run'], 'dir': str(out)}, contact_sheet(sorted((out / 'frames').glob('*.png'))))


# ---------------------------------------------------------------- PS Load Step (start from an older result)

def _folder_key(p):
    meta = p / 'stage.json' if p.is_dir() else p.parent / 'stage.json'
    k = json.loads(meta.read_text())['key'] if meta.exists() else str(p.stat().st_mtime_ns)
    return 'load:' + str(p) + ':' + k


class PSLoadFrames:
    DESCRIPTION = ('Uses an existing frames folder (orbit, upscale, masks or matte step of any run, also from before '
                   'the stage cache) as PS_FRAMES; the following steps write into this run.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {'run': ('PS_RUN',), 'folder': ('STRING', {'default': 'professor_grey2/04_matte',
                                                                       'tooltip': 'relative to output/profsplat'})}}

    RETURN_TYPES = ('PS_FRAMES', 'IMAGE'); RETURN_NAMES = ('frames', 'preview')
    FUNCTION = 'run'; CATEGORY = CAT

    def run(self, run, folder):
        p = Path(folder) if Path(folder).is_absolute() else out_root() / folder
        if not (p / 'frames').is_dir():
            raise ValueError(f'{p} has no frames/ folder')
        fr = {'run': run['run'], 'dir': str(p), 'key': _folder_key(p)}
        if (p / 'hero_parts.json').exists():
            fr['hero_parts'] = str(p / 'hero_parts.json')
        elif (p.parent / '03_masks' / 'hero_parts.json').exists():
            fr['hero_parts'] = str(p.parent / '03_masks' / 'hero_parts.json')
        names = frame_names(fr)
        masks = [p / 'masks' / 'frames' / n for n in names] if (p / 'masks' / 'frames').is_dir() else None
        return (fr, contact_sheet([p / 'frames' / n for n in names], masks))


class PSLoadSplat:
    DESCRIPTION = 'Uses an existing splat PLY (trained or cleaned, any run) as PS_SPLAT.'

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {'run': ('PS_RUN',),
                             'ply': ('STRING', {'default': 'professor_grey2/07_clean/splat.ply',
                                                'tooltip': 'relative to output/profsplat'}),
                             'height_m': ('FLOAT', {'default': 1.78, 'min': 0.3, 'max': 2.5, 'step': 0.01})}}

    RETURN_TYPES = ('PS_SPLAT',); RETURN_NAMES = ('splat',)
    FUNCTION = 'run'; CATEGORY = CAT

    def run(self, run, ply, height_m):
        p = Path(ply) if Path(ply).is_absolute() else out_root() / ply
        if not p.is_file():
            raise ValueError(f'{p} not found')
        return ({'run': run['run'], 'ply': str(p), 'height_m': height_m, 'key': _folder_key(p) + str(height_m)},)
