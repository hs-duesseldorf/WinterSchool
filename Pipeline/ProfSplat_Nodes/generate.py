"""Generative stages: run name, input image (FLUX.2 Klein), grey keyframes, guide photos, H3 orbit video."""
import hashlib, json
from pathlib import Path

import numpy as np, torch
from PIL import Image

from .common import (CAT, call, contact_sheet, fingerprint, free_gpu, load_clip, load_lora, load_sam3, load_unet,
                     load_vae, log, model_choice, out_root, run_dir, ssim_gray, stage, to_tensor, to_uint8)
from . import orbits

SEED = ('INT', {'default': 1, 'min': 0, 'max': 2 ** 31, 'control_after_generate': True})


# ---------------------------------------------------------------- PS Run

class PSRun:
    DESCRIPTION = ('Names the run. Every step writes into ComfyUI/output/profsplat/<run>/<step>/ and reuses that '
                   'result next time as long as its settings and inputs are unchanged.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {'run': ('STRING', {'default': 'professor'})}}

    RETURN_TYPES = ('PS_RUN',); RETURN_NAMES = ('run',)
    FUNCTION = 'run'; CATEGORY = CAT

    def run(self, run):
        run = run.strip()
        if not run or '/' in run or run.startswith('.'):
            raise ValueError('run: a plain folder name')
        return ({'run': run},)


# ---------------------------------------------------------------- PS Generate Image

STYLE = ("Full-body photo from head to toe, straight front view at eye level, the person centered and filling most of "
         "the frame height. Relaxed, mirror-symmetrical A-pose: arms straight and held about 40 degrees away from the "
         "body, hands open, palms forward, fingers apart, legs straight, feet slightly apart, head level, looking into "
         "the camera, neutral expression. Plain seamless light-gray photo studio, soft even diffuse light from all "
         "sides, no hard shadows. Sharp, photorealistic, high detail.")
KEEP = ("Keep the exact identity of the person from image 1: face, facial proportions, skin tone, hair, body build, "
        "clothing, shoes and accessories.")


@stage(lambda kw: f'image_{kw["label"]}')
class PSGenerateImage:
    DESCRIPTION = ('The input image of the pipeline: a full-body person in an A-pose on a plain grey studio backdrop, '
                   'generated with FLUX.2 Klein from a description. With a reference image the person is taken from '
                   'it (identity, clothes) and re-posed/re-lit as described. use_reference_as_is passes the reference '
                   'through unchanged (a real photo that already fits).')
    VERSION = 1

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'run': ('PS_RUN',),
            'description': ('STRING', {'multiline': True, 'default': 'A man in his sixties.',
                                       'tooltip': 'who: person, clothes, hair, accessories'}),
            'style': ('STRING', {'multiline': True, 'default': STYLE,
                                 'tooltip': 'what the pipeline needs: pose, framing, backdrop, light'}),
            'seed': SEED,
            'width': ('INT', {'default': 1152, 'min': 512, 'max': 2560, 'step': 16}),
            'height': ('INT', {'default': 1536, 'min': 512, 'max': 2560, 'step': 16}),
            'steps': ('INT', {'default': 4, 'min': 1, 'max': 50}),
            'model': model_choice('diffusion_models', 'flux-2-klein-9b-fp8-official.safetensors'),
            'text_encoder': model_choice('text_encoders', 'qwen_3_8b_fp8mixed.safetensors'),
            'vae': model_choice('vae', 'flux2-vae.safetensors'),
            'label': ('STRING', {'default': 'hero', 'tooltip': 'step folder: image_<label>'}),
        }, 'optional': {
            'reference': ('IMAGE', {'tooltip': 'optional photo of the person'}),
            'use_reference_as_is': ('BOOLEAN', {'default': False}),
        }}

    RETURN_TYPES = ('IMAGE', 'STRING'); RETURN_NAMES = ('image', 'prompt')
    FUNCTION = 'run'; CATEGORY = CAT

    def run(self, run, description, style, seed, width, height, steps, model, text_encoder, vae, label,
            reference=None, use_reference_as_is=False):
        out = run_dir(run['run'], f'image_{label}')
        if reference is not None and use_reference_as_is:
            img = reference[:1, ..., :3]
            prompt = '(reference image used as is)'
        else:
            prompt = (f'The person from image 1. {description.strip()} {style.strip()} {KEEP}' if reference is not None
                      else f'{description.strip()} {style.strip()}')
            free_gpu()
            unet, clip, v = load_unet(model), load_clip(text_encoder, 'flux2'), load_vae(vae)
            pos = call('CLIPTextEncode', clip=clip, text=prompt)[0]
            neg = call('CLIPTextEncode', clip=clip, text='')[0]
            if reference is not None:
                ref = call('ImageScaleToTotalPixels', image=reference[:1, ..., :3], upscale_method='lanczos',
                           megapixels=1.5, resolution_steps=16)[0]
                pos = call('ReferenceLatent', conditioning=pos, latent=call('VAEEncode', pixels=ref, vae=v)[0])[0]
            sample = call('SamplerCustomAdvanced',
                          noise=call('RandomNoise', noise_seed=seed)[0],
                          guider=call('CFGGuider', model=unet, positive=pos, negative=neg, cfg=1.0)[0],
                          sampler=call('KSamplerSelect', sampler_name='euler')[0],
                          sigmas=call('Flux2Scheduler', steps=steps, width=width, height=height)[0],
                          latent_image=call('EmptyFlux2LatentImage', width=width, height=height, batch_size=1)[0])[0]
            img = call('VAEDecode', samples=sample, vae=v)[0][:1, ..., :3]
            del unet, clip, v
            free_gpu()
        Image.fromarray(to_uint8(img[0])).save(out / 'image.png')
        (out / 'prompt.txt').write_text(prompt + '\n')
        log(f'image_{label}: {img.shape[2]}x{img.shape[1]}')
        return (img, prompt)


# ---------------------------------------------------------------- PS Orbit Video

def select_frames(frames, static_ssim, duplicate_ssim):
    """Indices of an H3 orbit worth keeping: drop the pinned (static) ends and near-duplicates."""
    import torch.nn.functional as F
    g = frames[..., :3].mean(-1)[:, None]
    g = F.interpolate(g, size=(g.shape[2] * 160 // g.shape[3], 160), mode='area')[:, 0]
    n = len(frames)
    start, end = 0, n - 1
    while start < n - 1 and ssim_gray(g[start], g[0]) > static_ssim:
        start += 1
    while end > start and ssim_gray(g[end], g[-1]) > static_ssim:
        end -= 1
    kept = [start]
    for i in range(start + 1, end + 1):
        if ssim_gray(g[i], g[kept[-1]]) < duplicate_ssim:
            kept.append(i)
    return kept, {'n_raw': n, 'trimmed_start': start, 'trimmed_end': n - 1 - end, 'n_frames': len(kept),
                  'near_duplicates': (end - start + 1) - len(kept)}


@stage(lambda kw: kw['label'])
class PSOrbitVideo:
    DESCRIPTION = ('One MiniMax H3 orbit video around the person (orbit360 LoRA + 8-step turbo), starting and ending '
                   'on the input image. Guides (grey keyframes, more photos) are pinned at their frames. Writes all '
                   'frames to raw/, the useful ones (pinned ends and near-duplicates dropped) to frames/, and the '
                   'full-resolution input image to hero/.')
    VERSION = 1

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'run': ('PS_RUN',), 'hero': ('IMAGE',),
            'orbit': (list(orbits.PROMPTS), {'default': 'climb',
                      'tooltip': 'climb: 10 s, eye-level circle then one over the head | level: 5 s eye level | '
                                 'wave: 5 s, rises over the head halfway | rings: 15 s, two rings'}),
            'seed': SEED,
            'steps': ('INT', {'default': 8, 'min': 1, 'max': 50}),
            'backdrop_prompt': ('BOOLEAN', {'default': True, 'tooltip': 'append the grey-studio text to the prompt'}),
            'prompt': ('STRING', {'multiline': True, 'default': '',
                                  'tooltip': 'empty: the preset prompt of the chosen orbit'}),
            'label': ('STRING', {'default': 'orbit', 'tooltip': 'step folder (e.g. scout / orbit)'}),
            'static_ssim': ('FLOAT', {'default': 0.98, 'min': 0.5, 'max': 1.0, 'step': 0.001,
                                      'tooltip': 'frames this similar to the first/last frame are the pinned ends'}),
            'duplicate_ssim': ('FLOAT', {'default': 0.985, 'min': 0.5, 'max': 1.0, 'step': 0.001,
                                         'tooltip': 'an interior frame this similar to the last kept one is dropped'}),
            'every_nth': ('INT', {'default': 0, 'min': 0, 'max': 8, 'tooltip': '0 = auto (2 for 10 s and longer)'}),
            'model': model_choice('diffusion_models', 'minimax_h3_fl2va_pruned_int8_convrot.safetensors'),
            'orbit_lora': model_choice('loras', 'minimax_h3_flf2v_orbit360_pablodawson_v1.safetensors'),
            'turbo_lora': model_choice('loras', 'minimax_h3_fl2v_turbo_8step_v1.2_768p_comfyui_bf16.safetensors'),
            'text_encoder': model_choice('text_encoders', 'qwen3vl_32b_minimax_h3_nvfp4_awq.safetensors'),
            'vae': model_choice('vae', 'minimax_h3_video_vae_fp16.safetensors'),
        }, 'optional': {
            'guides': ('PS_GUIDES', {'tooltip': 'images pinned at frames of the orbit (PS Grey Keyframes, PS Add Guide)'}),
        }}

    RETURN_TYPES = ('PS_FRAMES', 'IMAGE'); RETURN_NAMES = ('frames', 'preview')
    FUNCTION = 'run'; CATEGORY = CAT

    def run(self, run, hero, orbit, seed, steps, backdrop_prompt, prompt, label, static_ssim, duplicate_ssim,
            every_nth, model, orbit_lora, turbo_lora, text_encoder, vae, guides=None):
        out = run_dir(run['run'], label)
        length = orbits.LENGTHS.get(orbit, 124)
        text = (prompt.strip() or orbits.PROMPTS[orbit]) + (orbits.BACKDROP if backdrop_prompt else '')
        (out / 'prompt.txt').write_text(text + '\n')
        pins = [{'image': hero, 'frame': f} for f in orbits.PINS.get(orbit, ())]
        pins += [{'image': to_tensor(Image.open(g['file'])), 'frame': g['frame']} for g in (guides or {}).get('guides', [])]
        free_gpu()
        m = load_lora(load_lora(load_unet(model), orbit_lora), turbo_lora)
        clip, v = load_clip(text_encoder, 'minimax'), load_vae(vae)
        cond, lat = call('MiniMaxH3ImageToVideo', clip=clip, vae=v, prompt=text, width=768, height=1024, length=length,
                         first_frame=hero[:1, ..., :3], last_frame=hero[:1, ..., :3])
        for p in pins:
            cond = call('MiniMaxH3AddGuide', positive=cond, vae=v, latent=lat, image=p['image'][:1, ..., :3],
                        frame_idx=int(p['frame']))[0]
        log(f'{label}: H3 {orbit}, {length} frames, pinned at {[p["frame"] for p in pins]}')
        sample = call('SamplerCustomAdvanced',
                      noise=call('RandomNoise', noise_seed=seed)[0],
                      guider=call('BasicGuider', model=m, conditioning=cond)[0],
                      sampler=call('KSamplerSelect', sampler_name='res_multistep')[0],
                      sigmas=call('BasicScheduler', model=m, scheduler='simple', steps=steps, denoise=1.0)[0],
                      latent_image=lat)[0]
        video = call('VAEDecode', samples=sample, vae=v)[0]
        del m, clip, v, sample
        free_gpu()
        (out / 'raw').mkdir(); (out / 'frames').mkdir(); (out / 'hero').mkdir()
        for i in range(len(video)):
            Image.fromarray(to_uint8(video[i, ..., :3])).save(out / 'raw' / f'{i:05d}.png')
        kept, info = select_frames(video, static_ssim, duplicate_ssim)
        nth = every_nth or (2 if length > 200 else 1)
        if nth > 1:
            kept = kept[::nth]; info['every_nth'] = nth; info['n_frames'] = len(kept)
        for i in kept:
            Image.fromarray(to_uint8(video[i, ..., :3])).save(out / 'frames' / f'{i:05d}.png')
        Image.fromarray(to_uint8(hero[0, ..., :3])).save(out / 'hero' / 'hero.png')
        info.update(orbit=orbit, length=length, seed=seed, pins=[p['frame'] for p in pins])
        (out / 'frames.json').write_text(json.dumps(info, indent=2))
        log(f'{label}: {info}')
        fr = {'run': run['run'], 'dir': str(out), 'orbit': orbit}
        return (fr, contact_sheet(sorted((out / 'frames').glob('*.png'))))


# ---------------------------------------------------------------- PS Grey Keyframes

def person_mask(model, clip, img, threshold):
    """SAM3 'person' mask (largest region and what is attached), float HxW in [0,1]."""
    from scipy import ndimage
    cond = call('CLIPTextEncode', clip=clip, text='person')[0]
    m = call('SAM3_Detect', model=model, image=img, conditioning=cond, threshold=threshold, refine_iterations=2,
             individual_masks=False)[0][0].cpu().numpy() > 0.5
    lab, n = ndimage.label(m)
    if n > 1:
        sizes = ndimage.sum_labels(m, lab, range(1, n + 1))
        m = np.isin(lab, np.flatnonzero(sizes >= 0.02 * sizes.max()) + 1)
    return m.astype(np.float32)


def backdrop(hero, W, H):
    """The hero photo's backdrop as a smooth vertical gradient, from its outermost columns."""
    from scipy.ndimage import gaussian_filter1d, median_filter
    h = np.asarray(Image.fromarray(hero).resize((W, H), Image.LANCZOS)).astype(np.float32)
    rows = np.median(np.concatenate([h[:, :max(6, W // 40)], h[:, -max(6, W // 40):]], 1), 1)
    rows = gaussian_filter1d(median_filter(rows, size=(H // 7 | 1, 1), mode='nearest'), H / 50, axis=0)
    return np.repeat(rows[:, None], W, 1)


def composite(im, m, bg, hero_stats=None, edge_px=1.5, light=0.7, shadow=0.3):
    """Person of a scout frame onto the grey backdrop, so it reads as one photo and not a cut-out:
    - edge colors re-estimated from the inside (the scout's black void otherwise leaves a dark rim),
    - brightness/contrast pulled towards the hero's (the void relights the person darker and harder),
    - a soft contact shadow under the feet (a figure without one floats)."""
    from scipy import ndimage
    H, W = m.shape
    a = ndimage.gaussian_filter(m, edge_px)
    # foreground colors: normalized convolution of the solid interior out into the edge band
    core = ndimage.binary_erosion(m > 0.5, iterations=max(2, round(edge_px * 2))).astype(np.float32)
    s = max(2.0, edge_px * 3)
    fill = ndimage.gaussian_filter(im * core[..., None], (s, s, 0)) / (ndimage.gaussian_filter(core, s)[..., None] + 1e-6)
    fg = np.where(core[..., None] > 0, im, fill)
    if hero_stats is not None and light > 0:
        lum = fg @ np.array([0.299, 0.587, 0.114], np.float32)
        sel = m > 0.5
        mu, sd = lum[sel].mean(), lum[sel].std() + 1e-6
        target = (lum - mu) / sd * hero_stats[1] + hero_stats[0]
        gain = ((1 - light) * lum + light * target + 1e-3) / (lum + 1e-3)
        fg = np.clip(fg * np.clip(gain, 0.5, 2.0)[..., None], 0, 255)
    out = bg.copy()
    if shadow > 0:
        ys, xs = np.nonzero(m > 0.5)
        if len(ys):
            y1 = ys.max(); foot = xs[ys > y1 - 0.04 * (y1 - ys.min())]
            cx, rx = (foot.min() + foot.max()) / 2, max(8, (foot.max() - foot.min()) * 0.75)
            ry = max(4, 0.02 * H)
            yy, xx = np.mgrid[0:H, 0:W]
            e = np.exp(-(((xx - cx) / rx) ** 2 + ((yy - (y1 - ry * 0.3)) / ry) ** 2))
            out *= (1 - shadow * e)[..., None]
    return np.clip(fg * a[..., None] + out * (1 - a[..., None]), 0, 255)


@stage(lambda kw: f'{Path(kw["scout"]["dir"]).name}_keyframes')
class PSGreyKeyframes:
    DESCRIPTION = ('Grey keyframes: H3 swaps the photo\'s grey studio for a black void once the camera leaves the '
                   'front, which relights the person and fringes the matte. So a first (scout) orbit runs without '
                   'guides; this node cuts the person out of its side/back frames (SAM3) and puts them onto the '
                   'photo\'s grey, and the real orbit gets those pinned at the same frames. They only steer H3 '
                   'towards a grey studio and even light; H3 renders the actual frames.')
    VERSION = 2

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'scout': ('PS_FRAMES',), 'hero': ('IMAGE',),
            'frames': ('STRING', {'default': 'auto', 'tooltip': 'frame numbers, e.g. 31,61,91 ("auto": preset of the '
                                                                 'scout\'s orbit)'}),
            'sam3': model_choice('checkpoints', 'sam3.1_multiplex_fp16.safetensors'),
            'threshold': ('FLOAT', {'default': 0.4, 'min': 0.05, 'max': 0.95, 'step': 0.01}),
            'edge_softness': ('FLOAT', {'default': 1.5, 'min': 0.0, 'max': 8.0, 'step': 0.25, 'tooltip': 'px'}),
            'match_light': ('FLOAT', {'default': 0.7, 'min': 0.0, 'max': 1.0, 'step': 0.05,
                                      'tooltip': 'pull the person\'s brightness/contrast towards the hero photo'}),
            'shadow': ('FLOAT', {'default': 0.3, 'min': 0.0, 'max': 1.0, 'step': 0.05,
                                 'tooltip': 'darkness of the soft contact shadow under the feet'}),
        }}

    RETURN_TYPES = ('PS_GUIDES', 'IMAGE'); RETURN_NAMES = ('guides', 'preview')
    FUNCTION = 'run'; CATEGORY = CAT

    def run(self, scout, hero, frames, sam3, threshold, edge_softness, match_light, shadow):
        src = Path(scout['dir'])
        out = run_dir(scout['run'], f'{src.name}_keyframes')
        if frames.strip().lower() == 'auto':
            idx = orbits.KEYS.get(scout.get('orbit'), orbits.KEYS_DEFAULT)
        else:
            idx = [int(x) for x in frames.replace(';', ',').split(',') if x.strip()]
        raw = src / 'raw'
        missing = [f for f in idx if not (raw / f'{f:05d}.png').exists()]
        if missing:
            raise ValueError(f'frames {missing} are not in {raw}')
        free_gpu()
        model, clip = load_sam3(sam3)
        W, H = Image.open(raw / f'{idx[0]:05d}.png').size
        hero_u8 = to_uint8(hero[0, ..., :3])
        bg = backdrop(hero_u8, W, H)
        h_small = np.asarray(Image.fromarray(hero_u8).resize((W, H), Image.LANCZOS)).astype(np.float32)
        hm = person_mask(model, clip, torch.from_numpy(h_small / 255)[None].float(), threshold) > 0.5
        hl = h_small @ np.array([0.299, 0.587, 0.114], np.float32)
        stats = (float(hl[hm].mean()), float(hl[hm].std())) if hm.any() else None
        guides, paths = [], []
        for f in idx:
            im = np.asarray(Image.open(raw / f'{f:05d}.png').convert('RGB')).astype(np.float32)
            m = person_mask(model, clip, torch.from_numpy(im / 255)[None].float(), threshold)
            Image.fromarray((m * 255).astype(np.uint8)).save(out / f'mask{f:03d}.png')
            res = composite(im, m, bg, stats, edge_softness, match_light, shadow)
            p = out / f'key{f:03d}.png'
            Image.fromarray(res.round().astype(np.uint8)).save(p)
            guides.append({'file': str(p), 'frame': f}); paths.append(p)
        del model, clip
        free_gpu()
        log(f'grey keyframes at {idx}')
        return ({'run': scout['run'], 'dir': str(out), 'guides': guides}, contact_sheet(paths, cols=min(len(paths), 6)))


# ---------------------------------------------------------------- PS Add Guide

class PSAddGuide:
    DESCRIPTION = ('Pins one more image (e.g. a real back photo of the same person in the same pose) at a frame of '
                   'the orbit, where the camera passes that viewpoint. Chain several; connect to PS Orbit Video.')

    @classmethod
    def INPUT_TYPES(cls):
        return {'required': {
            'image': ('IMAGE',),
            'frame': ('INT', {'default': 61, 'min': 1, 'max': 361}),
        }, 'optional': {'guides': ('PS_GUIDES',)}}

    RETURN_TYPES = ('PS_GUIDES',); RETURN_NAMES = ('guides',)
    FUNCTION = 'run'; CATEGORY = CAT

    def run(self, image, frame, guides=None):
        h = fingerprint(image[:1]).split(':')[1][:16]
        d = out_root() / '_guides'; d.mkdir(parents=True, exist_ok=True)
        p = d / f'{h}.png'
        if not p.exists():
            Image.fromarray(to_uint8(image[0, ..., :3])).save(p)
        prev = (guides or {}).get('guides', [])
        key = hashlib.sha1(json.dumps([(guides or {}).get('key'), h, frame]).encode()).hexdigest()[:16]
        return ({'guides': prev + [{'file': str(p), 'frame': frame}], 'key': key},)
