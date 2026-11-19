"""The four pipeline nodes: 1 Image -> 2 Views -> 3 Video -> 4 Splat.

Each one runs the stages under ProfSplat/advanced (with their stage cache: unchanged steps are read from disk) and
shows its result on the node itself. They pass one PROFSPLAT value along: run name, dry-run flag, the input image,
the views and the frames.

Dry run (switch on node 1): nothing is generated or trained. Every node logs what it would run and shows the stored
result of that stage of the finished professor run (DEMO_RUN), marked DRY RUN. For presenting the pipeline."""
import hashlib, json
from pathlib import Path

import numpy as np, torch
from PIL import Image, ImageDraw, ImageFont

import folder_paths

from .common import CAT, ROOT, contact_sheet, fingerprint, load_batch, log, out_root, to_tensor, to_uint8
from . import orbits
from .generate import PSGenerateImage, PSGreyKeyframes, PSOrbitVideo
from .process import (PSCameraPoses, PSCleanSplat, PSExportSplat, PSMatte, PSSegmentSAM3, PSTrainSplat,
                      PSUpscaleSeedVR2)

PIPE = 'PROFSPLAT'
SEED = ('INT', {'default': 1, 'min': 0, 'max': 2 ** 31, 'control_after_generate': True})
DEMO_RUN = 'professor_grey2'   # finished run shown by the dry run (its step folders still carry 01_ ... prefixes)
DEMO_GUIDES = [ROOT / 'inputs' / f'professor_grey2_guide{i}.png' for i in range(1, 7)]
# training preset: rounder splats that hold up off-axis (strong spike penalty, no push towards small scales, SH
# degree 1), edge loss and thin-line weights near LichtFeld's defaults so fingers seen in few frames survive
TRAIN = dict(strategy='mrnf', sh_degree=1, pose_opt=True, normal_prior=True, mip_filter=True,
             thin_structure_weight=0.5, extra_args='', spike_penalty=0.01, scale_penalty=0.0, edge_loss=1.2,
             densify_structure=1.0, view_color_penalty=0.01)


# ---------------------------------------------------------------- helpers

def step(cls, **kw):
    """Run an advanced stage node with its defaults, overridden by kw; returns its outputs tuple."""
    spec = cls.INPUT_TYPES()
    args = {}
    for group in ('required', 'optional'):
        for name, s in spec.get(group, {}).items():
            cfg = s[1] if len(s) > 1 else {}
            if isinstance(s[0], list):
                args[name] = cfg.get('default', s[0][0])
            elif 'default' in cfg:
                args[name] = cfg['default']
    args.update(kw)
    res = getattr(cls(), cls.FUNCTION)(**args)
    return res['result'] if isinstance(res, dict) else res


def pipe_key(*parts):
    return hashlib.sha1(json.dumps([fingerprint(p) for p in parts], sort_keys=True).encode()).hexdigest()[:16]


def ui_entry(p):
    rel = Path(p).resolve().relative_to(Path(folder_paths.get_output_directory()).resolve())
    return {'filename': rel.name, 'subfolder': str(rel.parent), 'type': 'output'}


def save(img, path):
    """IMAGE tensor or PIL image -> PNG, returns the path."""
    path = Path(path); path.parent.mkdir(parents=True, exist_ok=True)
    (img if isinstance(img, Image.Image) else Image.fromarray(to_uint8(img[0, ..., :3]))).save(path)
    return path


def banner(im, text='DRY RUN  ·  stored result of the professor run'):
    im = im.convert('RGB').copy()
    im.thumbnail((2048, 1536), Image.LANCZOS)
    d = ImageDraw.Draw(im)
    size = max(14, im.width // 40)
    font = ImageFont.load_default(size=size)
    d.rectangle([0, 0, im.width, int(size * 1.8)], fill=(170, 40, 40))
    d.text((size // 2, int(size * 0.35)), text, fill=(255, 255, 255), font=font)
    return im


def dry_dir():
    d = out_root() / '_dry'; d.mkdir(parents=True, exist_ok=True)
    return d


def demo(*parts):
    p = out_root() / DEMO_RUN
    for x in parts:
        p = p / x
    if not p.exists():
        raise FileNotFoundError(f'dry run: {p} is missing (the stored professor run)')
    return p


def orbit_webp(src, dst, fps=24, width=384, mark=False):
    """All frames of an orbit as one animated WebP (shown on the node)."""
    if dst.exists():
        return dst
    files = sorted(src.glob('*.png'))
    ims = []
    for f in files:
        im = Image.open(f).convert('RGB')
        im = im.resize((width, round(width * im.height / im.width)), Image.LANCZOS)
        ims.append(banner(im) if mark else im)
    ims[0].save(dst, save_all=True, append_images=ims[1:], duration=round(1000 / fps), loop=0, quality=80, method=4)
    return dst


def matte_sheet(d, n=24):
    """Up to n evenly spaced matted frames on green (the full sheet of 100+ frames is too tall to read)."""
    names = sorted(p.name for p in (Path(d) / 'frames').glob('*.png'))
    names = [names[round(i * (len(names) - 1) / (n - 1))] for i in range(n)] if len(names) > n else names
    return contact_sheet([Path(d) / 'frames' / x for x in names], [Path(d) / 'masks' / 'frames' / x for x in names])


def show(paths, out, animated=False):
    ui = {'images': [ui_entry(p) for p in paths]}
    if animated:
        ui['animated'] = (True,)
    return {'ui': ui, 'result': out}


# ---------------------------------------------------------------- 1 Image

class PSImage:
    DESCRIPTION = ('1 · Input image: a full-body photo of the person in an A-pose in a plain grey studio, generated '
                   'with FLUX.2 Klein from the prompt. With a photo connected, the person (face, clothes) is taken '
                   'from it and only pose, framing, backdrop and light are generated; "as is" uses the photo '
                   'unchanged. run names the output folder; dry_run walks the whole pipeline on stored results.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'run': ('STRING', {'default': 'professor', 'tooltip': 'output folder: ComfyUI/output/profsplat/<run>/'}),
            'prompt': ('STRING', {'multiline': True, 'default': 'A man in his sixties.',
                                  'tooltip': 'who: person, clothes, hair, accessories (pose, backdrop and light '
                                             'are added automatically)'}),
            'photo_use': (['reference', 'as is'], {'default': 'reference',
                          'tooltip': 'reference: keep the person from the photo, generate the image | as is: use '
                                     'the photo unchanged'}),
            'seed': SEED,
            'steps': ('INT', {'default': 4, 'min': 1, 'max': 50}),
            'dry_run': ('BOOLEAN', {'default': False, 'tooltip': 'generate and train nothing; show the stored result '
                                                                 'of each stage of the professor run'}),
        }, 'optional': {'photo': ('IMAGE', {'tooltip': 'optional photo of the person'})}}

    RETURN_TYPES = (PIPE, 'IMAGE'); RETURN_NAMES = ('pipe', 'image')
    FUNCTION = 'run'; CATEGORY = CAT; OUTPUT_NODE = True

    def run(self, run, prompt, photo_use, seed, steps, dry_run, photo=None):
        run = run.strip()
        if not run or '/' in run or run.startswith(('.', '_')):
            raise ValueError('run: a plain folder name')
        as_is = photo is not None and photo_use == 'as is'
        if dry_run:
            what = 'the photo as is' if as_is else (
                f'FLUX.2 Klein, {steps} steps, seed {seed}, ' + ('photo as reference, ' if photo is not None else '') +
                f'prompt "{prompt.strip()}" + A-pose/grey-studio text')
            log(f'dry run · 1 Image: would use {what}')
            img = photo[:1, ..., :3] if as_is else to_tensor(Image.open(demo('01_frames', 'hero', 'hero.png')))
            p = save(banner(Image.fromarray(to_uint8(img[0]))) if not as_is else img, dry_dir() / '1_image.png')
            return show([p], ({'run': run, 'dry': True, 'hero': img, 'key': pipe_key('dry', img)}, img))
        img, _ = step(PSGenerateImage, run={'run': run}, description=prompt, seed=seed, steps=steps, label='hero',
                      reference=photo, use_reference_as_is=as_is)
        p = out_root() / run / 'image_hero' / 'image.png'
        return show([p], ({'run': run, 'dry': False, 'hero': img, 'key': pipe_key(img)}, img))


# ---------------------------------------------------------------- 2 Views

class PSViews:
    DESCRIPTION = ('2 · Views: side and back views of the person, in the same grey studio. A first (scout) H3 orbit '
                   'is generated from the image; its side/back frames are cut out and put onto the image\'s grey '
                   'backdrop with matched light and a floor shadow. The video (3) is pinned to them, which keeps '
                   'H3 from swapping the studio for a black void. orbit sets the camera path for both.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'pipe': (PIPE,),
            'orbit': (list(orbits.PROMPTS), {'default': 'climb',
                      'tooltip': 'climb: 10 s, eye-level circle then one over the head | level: 5 s eye level | '
                                 'wave: 5 s, rises over the head halfway | rings: 15 s, two rings'}),
            'views': (['grey keyframes', 'none'], {'default': 'grey keyframes',
                      'tooltip': 'none: the video runs from the image alone'}),
            'seed': SEED,
            'frames': ('STRING', {'default': 'auto', 'tooltip': 'orbit frames to take views at, e.g. 31,61,91 '
                                                                '(auto: the preset of the orbit)'}),
            'match_light': ('FLOAT', {'default': 0.7, 'min': 0.0, 'max': 1.0, 'step': 0.05}),
            'shadow': ('FLOAT', {'default': 0.3, 'min': 0.0, 'max': 1.0, 'step': 0.05}),
        }}

    RETURN_TYPES = (PIPE, 'IMAGE'); RETURN_NAMES = ('pipe', 'views')
    FUNCTION = 'run'; CATEGORY = CAT; OUTPUT_NODE = True

    def run(self, pipe, orbit, views, seed, frames, match_light, shadow):
        hero = pipe['hero']
        if views == 'none':
            out = dict(pipe, orbit=orbit, guides=None, key=pipe_key(pipe, orbit))
            return show([save(hero, (dry_dir() if pipe['dry'] else out_root() / pipe['run']) / '2_views.png')],
                        (out, hero))
        if pipe['dry']:
            idx = orbits.KEYS['climb']
            at = orbits.KEYS.get(orbit, orbits.KEYS_DEFAULT) if frames.strip() == 'auto' else frames
            log(f'dry run · 2 Views: would generate a scout {orbit} orbit (MiniMax H3, seed {seed}) and cut views '
                f'out of its frames {at} (SAM 3.1)')
            guides = [{'file': str(f), 'frame': i} for f, i in zip(DEMO_GUIDES, idx)]
            sheet = contact_sheet([g['file'] for g in guides], cols=6)
            p = save(banner(Image.fromarray(to_uint8(sheet[0]))), dry_dir() / '2_views.png')
            out = dict(pipe, orbit='climb', guides={'guides': guides}, key=pipe_key(pipe, 'dry views'))
            return show([p], (out, load_batch([g['file'] for g in guides])))
        run = {'run': pipe['run']}
        scout = step(PSOrbitVideo, run=run, hero=hero, orbit=orbit, seed=seed, label='scout')[0]
        guides, _ = step(PSGreyKeyframes, scout=scout, hero=hero, frames=frames, match_light=match_light,
                         shadow=shadow)
        files = [g['file'] for g in guides['guides']]
        sheet = Path(guides['dir']) / 'views.png'
        if not sheet.exists():
            save(contact_sheet(files, cols=min(len(files), 6)), sheet)
        out = dict(pipe, orbit=orbit, guides=guides, key=pipe_key(pipe, guides))
        return show([sheet], (out, load_batch(files)))


# ---------------------------------------------------------------- 3 Video

class PSVideo:
    DESCRIPTION = ('3 · 360° video: MiniMax H3 (orbit360 LoRA, 8-step turbo) circles the person along the orbit '
                   'chosen in 2, starting and ending on the image and passing through the views at their frames. '
                   'Shown as an animation; the usable frames (pinned ends and near-duplicates dropped) go on.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'pipe': (PIPE,),
            'seed': SEED,
            'steps': ('INT', {'default': 8, 'min': 1, 'max': 50}),
        }}

    RETURN_TYPES = (PIPE,); RETURN_NAMES = ('pipe',)
    FUNCTION = 'run'; CATEGORY = CAT; OUTPUT_NODE = True

    def run(self, pipe, seed, steps):
        if pipe['dry']:
            n = len((pipe['guides'] or {}).get('guides', []))
            log(f'dry run · 3 Video: would generate a {pipe["orbit"]} orbit, {orbits.LENGTHS.get(pipe["orbit"], 124)} '
                f'frames, {steps} steps, seed {seed}, pinned to {n} views')
            webp = orbit_webp(demo('raw'), dry_dir() / '3_video.webp', mark=True)
            frames = {'run': pipe['run'], 'dir': str(demo('01_frames')), 'orbit': 'climb', 'key': 'dry'}
            return show([webp], (dict(pipe, frames=frames, key=pipe_key(pipe, 'dry video')),), animated=True)
        frames = step(PSOrbitVideo, run={'run': pipe['run']}, hero=pipe['hero'], orbit=pipe['orbit'], seed=seed,
                      steps=steps, label='orbit', guides=pipe['guides'])[0]
        d = Path(frames['dir'])
        webp = orbit_webp(d / 'raw', d / 'orbit.webp')
        return show([webp], (dict(pipe, frames=frames, key=pipe_key(pipe, frames)),), animated=True)


# ---------------------------------------------------------------- 4 Splat

class PSSplat:
    DESCRIPTION = ('4 · Gaussian splat: SeedVR2 upscale -> SAM 3.1 person masks -> MatAnyone 2 soft matte -> COLMAP '
                   'camera poses (person standing at the origin, height_m tall) -> LichtFeld Studio training -> '
                   'clean-up (floaters, feet on the floor) -> PLY/SPZ/SOG/HTML in export_<run>/. Shows the matted '
                   'frames and novel views of the finished splat. Each step is cached on its own.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'pipe': (PIPE,),
            'height_m': ('FLOAT', {'default': 1.78, 'min': 0.3, 'max': 2.5, 'step': 0.01, 'tooltip': 'body height'}),
            'upscale': ('BOOLEAN', {'default': True, 'tooltip': 'SeedVR2 2x before masking and training'}),
            'max_splats': ('INT', {'default': 130000, 'min': 10000, 'max': 5000000, 'step': 10000}),
            'train_steps': ('FLOAT', {'default': 2.0, 'min': 0.25, 'max': 8.0, 'step': 0.25,
                                      'tooltip': '1 = 30k iterations'}),
        }}

    RETURN_TYPES = ('IMAGE', 'STRING'); RETURN_NAMES = ('views', 'folder')
    FUNCTION = 'run'; CATEGORY = CAT; OUTPUT_NODE = True

    def run(self, pipe, height_m, upscale, max_splats, train_steps):
        if pipe['dry']:
            log(f'dry run · 4 Splat: would run {"SeedVR2 2x, " if upscale else ""}SAM 3.1, MatAnyone 2, COLMAP '
                f'(height {height_m} m), LichtFeld ({max_splats} splats, {train_steps * 30000:.0f} iterations), '
                f'clean, export')
            matte = dry_dir() / '4_matte.png'
            if not matte.exists():
                save(banner(Image.fromarray(to_uint8(matte_sheet(demo('04_matte'))[0]))), matte)
            export = demo(f'08_export_{DEMO_RUN}')
            views = save(banner(Image.open(export / 'views.png')), dry_dir() / '4_views.png')
            return show([matte, views], (to_tensor(Image.open(views)), str(export)))
        fr = pipe['frames']
        if upscale:
            fr = step(PSUpscaleSeedVR2, frames=fr)[0]
        fr = step(PSSegmentSAM3, frames=fr)[0]
        fr = step(PSMatte, frames=fr)[0]
        ds = step(PSCameraPoses, frames=fr, height_m=height_m)[0]
        sp = step(PSTrainSplat, dataset=ds, max_splats=max_splats, steps_scaler=train_steps, **TRAIN)[0]
        sp = step(PSCleanSplat, splat=sp)[0]
        views, folder = step(PSExportSplat, splat=sp, name=pipe['run'])
        mp = Path(fr['dir']) / 'matte.png'
        if not mp.exists():
            save(matte_sheet(fr['dir']), mp)
        return show([mp, Path(folder) / 'views.png'], (views, folder))
