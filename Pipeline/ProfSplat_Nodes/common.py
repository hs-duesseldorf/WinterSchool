"""Shared helpers of the ProfSplat nodes: paths, model loading, frame folders, and the stage cache.

Stage cache: every pipeline node writes its result into output/profsplat/<run>/<step>/ together with a stage.json
holding a fingerprint of everything that went into it (its settings plus the fingerprints of its inputs). When the
workflow runs again (also after a ComfyUI restart) and a node's fingerprint matches its stage.json, it returns the
stored result instead of recomputing. Change one setting and press Queue: that step and everything after it run
again, everything before it is read back from disk. Delete a step folder to force it."""
import hashlib, json, math, os, re, shutil, subprocess, time
from pathlib import Path

import numpy as np, torch
from PIL import Image

import comfy.model_management as mm
import comfy.utils
import folder_paths
import nodes as comfy_nodes

# Pipeline/work (or $PROFSPLAT_ROOT): envs/, models/, third_party/ (ComfyUI, LichtFeld), inputs/; this package is
# linked into ComfyUI's custom_nodes
ROOT = Path(os.environ.get('PROFSPLAT_ROOT') or Path(__file__).resolve().parents[1] / 'work')
TOOLS = Path(__file__).resolve().parent / 'tools'
PY_MATTING = ROOT / 'envs' / 'matting' / 'bin' / 'python'
PY_GSPLAT = ROOT / 'envs' / 'gsplat' / 'bin' / 'python'
LFS = ROOT / 'third_party' / 'LichtFeld-Studio' / 'build-linux-release' / 'LichtFeld-Studio'
COLMAP = shutil.which('colmap') or '/usr/local/bin/colmap'
FLIP = np.diag([-1.0, -1.0, 1.0])  # viewer frame (y up) <-> 3DGS PLY file frame (y down); its own inverse
CUDA_ENV = {'CUDA_HOME': '/usr/local/cuda-13.2', 'TORCH_CUDA_ARCH_LIST': '8.6',
            'NVCC_PREPEND_FLAGS': '-ccbin /usr/bin/g++-15'}
CAT = 'ProfSplat'


def log(msg):
    print(f'[ProfSplat] {msg}', flush=True)


# ---------------------------------------------------------------- calling ComfyUI's own nodes from Python

def call(name, **kw):
    """Run a registered ComfyUI node by class name and return its outputs as a tuple (V1 and V3 nodes)."""
    cls = comfy_nodes.NODE_CLASS_MAPPINGS[name]
    if hasattr(cls, 'define_schema'):
        return tuple(cls.execute(**kw).args)
    return tuple(getattr(cls(), cls.FUNCTION)(**kw))


def models(folder):
    """Dropdown entries for a model folder (diffusion_models, vae, ...)."""
    try:
        return folder_paths.get_filename_list(folder)
    except Exception:
        return []


def model_choice(folder, default):
    names = models(folder)
    return (names or [default], {'default': default if default in names or not names else names[0]})


def load_unet(name):
    return call('UNETLoader', unet_name=name, weight_dtype='default')[0]


def load_vae(name):
    return call('VAELoader', vae_name=name)[0]


def load_clip(name, kind):
    return call('CLIPLoader', clip_name=name, type=kind, device='default')[0]


def load_lora(model, name, strength=1.0):
    return call('LoraLoaderModelOnly', model=model, lora_name=name, strength_model=strength)[0]


def load_sam3(name):
    model, clip = call('CheckpointLoaderSimple', ckpt_name=name)[:2]
    return model, clip


def free_gpu():
    """Move every Comfy model off the GPU before a subprocess (or the next big model) needs all of it."""
    mm.unload_all_models()
    mm.soft_empty_cache()
    if torch.cuda.is_available():
        torch.cuda.empty_cache()


# ---------------------------------------------------------------- folders and images

def out_root():
    return Path(folder_paths.get_output_directory()) / 'profsplat'


def run_dir(run, step):
    """A fresh step folder (an old result of the same step is replaced)."""
    d = out_root() / run / step
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    return d


def link_tree(src: Path, dst: Path):
    """Hard-link a folder tree (no extra disk space)."""
    for p in src.rglob('*'):
        if p.is_file() and p.name != 'stage.json':
            q = dst / p.relative_to(src)
            q.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(p, q)
            except OSError:
                shutil.copy2(p, q)


def to_uint8(t):
    return (t.clamp(0, 1).cpu().numpy() * 255 + 0.5).astype(np.uint8)


def to_tensor(im):
    return torch.from_numpy(np.asarray(im.convert('RGB'))).float().div(255)[None]


def load_batch(paths):
    return torch.from_numpy(np.stack([np.asarray(Image.open(p).convert('RGB')) for p in paths])).float().div(255)


def frame_names(fr):
    return sorted(p.name for p in (Path(fr['dir']) / 'frames').glob('*.png'))


def ring_of(name):
    """Orbit ring of a frame: extra rings carry a prefix ("e30_00012.png"), the first ring none ('')."""
    stem = Path(name).stem
    return stem.split('_')[0] if '_' in stem else ''


def rings(names):
    out = {}
    for n in names:
        out.setdefault(ring_of(n), []).append(n)
    return out


def contact_sheet(paths, mask_paths=None, cols=8, width=1536):
    paths = list(paths)
    if not paths:
        return torch.zeros(1, 64, 64, 3)
    n = len(paths)
    rows = math.ceil(n / cols)
    im0 = Image.open(paths[0])
    w = width // cols; h = round(w * im0.height / im0.width)
    sheet = Image.new('RGB', (cols * w, rows * h), (128, 128, 128))
    for i, p in enumerate(paths):
        im = Image.open(p).convert('RGB').resize((w, h), Image.LANCZOS)
        if mask_paths:
            m = Image.open(mask_paths[i]).convert('L').resize((w, h), Image.BILINEAR)
            im = Image.composite(im, Image.new('RGB', (w, h), (40, 160, 60)), m)
        sheet.paste(im, ((i % cols) * w, (i // cols) * h))
    return torch.from_numpy(np.asarray(sheet)).float().div(255)[None]


def ssim_gray(a, b):
    """SSIM of two small grayscale images (torch, HxW in [0,1])."""
    import torch.nn.functional as F
    a, b = a[None, None], b[None, None]
    k = torch.ones(1, 1, 7, 7) / 49
    mu_a, mu_b = F.conv2d(a, k), F.conv2d(b, k)
    s_a = F.conv2d(a * a, k) - mu_a ** 2; s_b = F.conv2d(b * b, k) - mu_b ** 2; s_ab = F.conv2d(a * b, k) - mu_a * mu_b
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    return float((((2 * mu_a * mu_b + c1) * (2 * s_ab + c2)) / ((mu_a ** 2 + mu_b ** 2 + c1) * (s_a + s_b + c2))).mean())


def run_tool(cmd, total=None, env=None, cwd=None, logfile=None):
    """Run a subprocess, stream its output to the console (and a log), drive the Comfy progress bar
    from 'PROGRESS i/n' lines or LichtFeld's iteration log."""
    log(' '.join(str(c) for c in cmd))
    e = os.environ.copy(); e.update(env or {})
    bar = comfy.utils.ProgressBar(total) if total else None
    lf = open(logfile, 'w') if logfile else None
    p = subprocess.Popen([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         env=e, cwd=cwd, bufsize=1)
    tail = []
    for line in p.stdout:
        line = line.rstrip()
        if lf:
            lf.write(line + '\n'); lf.flush()
        tail = (tail + [line])[-40:]
        m = re.search(r'PROGRESS (\d+)/(\d+)', line) or re.search(r'[Ii]ter(?:ation)?\s*[:=]?\s*(\d[\d,]*)\s*/\s*(\d[\d,]*)', line)
        if m and bar:
            bar.update_absolute(int(m.group(1).replace(',', '')), int(m.group(2).replace(',', '')))
        if not m or int(m.group(1).replace(',', '')) % 1000 == 0:
            print(line, flush=True)
    p.wait()
    if lf:
        lf.close()
    if p.returncode != 0:
        raise RuntimeError(f'{Path(str(cmd[0])).name} failed (exit {p.returncode}):\n' + '\n'.join(tail[-15:]))


# ---------------------------------------------------------------- stage cache

def fingerprint(v):
    """JSON-able identity of a node input: pipeline values by their key, images by a hash of their pixels."""
    if isinstance(v, dict):
        if 'key' in v:
            return {'key': v['key']}
        return {k: fingerprint(x) for k, x in sorted(v.items())}
    if isinstance(v, (list, tuple)):
        return [fingerprint(x) for x in v]
    if isinstance(v, torch.Tensor):
        return 'tensor:' + hashlib.sha1(to_uint8(v).tobytes() + str(tuple(v.shape)).encode()).hexdigest()
    if v is None or isinstance(v, (str, int, float, bool)):
        return v
    return repr(v)


def stage(step):
    """Class decorator: cache the node's result in output/profsplat/<run>/<step>/stage.json.

    step: a folder name, or a function of the node's inputs returning one. The run name comes from the 'run' input
    (PS_RUN) or from any pipeline input dict that carries one. Node outputs may be pipeline dicts (stored as JSON),
    IMAGE tensors (stored as PNGs next to stage.json), or strings. A pipeline dict returned by the node gets the
    stage's key added, so the steps after it know when it changed. Bump the class VERSION when the code changes in
    a way that should invalidate old results."""
    def deco(cls):
        inner = getattr(cls, cls.FUNCTION)

        def run(self, **kw):
            run_name = next((v['run'] for v in kw.values() if isinstance(v, dict) and 'run' in v), None)
            if run_name is None:
                raise ValueError(f'{cls.__name__}: no run name (connect a PS Run node)')
            name = step(kw) if callable(step) else step
            ident = {'node': cls.__name__, 'version': getattr(cls, 'VERSION', 1), 'step': name,
                     'inputs': fingerprint(kw)}
            key = hashlib.sha1(json.dumps(ident, sort_keys=True).encode()).hexdigest()[:16]
            folder = out_root() / run_name / name
            meta = folder / 'stage.json'
            if meta.exists():
                try:
                    stored = json.loads(meta.read_text())
                    if stored.get('key') == key:
                        log(f'{run_name}/{name}: unchanged, using the stored result')
                        return decode(stored['outputs'], folder)
                    log(f'{run_name}/{name}: settings or inputs changed, running again')
                except Exception as e:
                    log(f'{run_name}/{name}: unreadable stage.json ({e}), running again')
            t0 = time.time()
            res = inner(self, **kw)
            ui = None
            if isinstance(res, dict):
                ui, res = res.get('ui'), res['result']
            res = tuple(dict(r, key=key) if isinstance(r, dict) and 'run' in r else r for r in res)
            folder.mkdir(parents=True, exist_ok=True)
            meta.write_text(json.dumps({'key': key, 'inputs': ident, 'seconds': round(time.time() - t0),
                                        'outputs': encode(res, folder)}, indent=1))
            return {'ui': ui, 'result': res} if ui else res

        setattr(cls, cls.FUNCTION, run)
        return cls
    return deco


def encode(res, folder):
    out = []
    for i, r in enumerate(res):
        if isinstance(r, torch.Tensor):
            files = []
            for k in range(r.shape[0]):
                f = f'_out{i}_{k}.png'
                Image.fromarray(to_uint8(r[k, ..., :3])).save(folder / f)
                files.append(f)
            out.append({'image': files})
        else:
            out.append({'value': r})
    return out


def decode(outputs, folder):
    res = []
    for o in outputs:
        if 'image' in o:
            res.append(load_batch([folder / f for f in o['image']]))
        else:
            res.append(o['value'])
    return tuple(res)
