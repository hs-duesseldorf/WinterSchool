"""ProfSplat: prompt (+ optional photo) -> Gaussian splat of the person, as four ComfyUI nodes.

  1 Image   prompt (+ photo)  FLUX.2 Klein: the person in an A-pose in a grey studio
  2 Views   image             side/back views on the same grey (scout H3 orbit, cut out, re-lit)
  3 Video   image + views     MiniMax H3 360° orbit video
  4 Splat   video             SeedVR2, SAM 3.1, MatAnyone 2, COLMAP, LichtFeld Studio, clean, export

Every step stores its result in output/profsplat/<run>/<step>/ and reuses it while its settings and inputs are
unchanged, also after a restart (common.py): change a setting, press Queue, and only that step and the ones after it
run again. dry_run on node 1 shows the whole pipeline on stored results without the GPU (pipeline.py).

The single steps are under ProfSplat/advanced (each with all of its settings and models).
"""
from .pipeline import PSImage, PSViews, PSVideo, PSSplat
from .generate import PSRun, PSGenerateImage, PSOrbitVideo, PSGreyKeyframes, PSAddGuide
from .process import (PSAddRing, PSSaveFrames, PSUpscaleSeedVR2, PSSegmentSAM3, PSMatte, PSCameraPoses, PSTrainSplat,
                      PSCleanSplat, PSExportSplat, PSPreviewFrames, PSLoadFrames, PSLoadSplat)

_MAIN = [(PSImage, '1 · Image'), (PSViews, '2 · Views'), (PSVideo, '3 · 360° Video'), (PSSplat, '4 · Gaussian Splat')]
_ADVANCED = [
    (PSRun, 'PS Run'), (PSGenerateImage, 'PS Generate Image (FLUX.2 Klein)'),
    (PSOrbitVideo, 'PS Orbit Video (MiniMax H3)'), (PSGreyKeyframes, 'PS Grey Keyframes'),
    (PSAddGuide, 'PS Add Guide'), (PSSaveFrames, 'PS Save Frames'), (PSAddRing, 'PS Add Orbit Ring'),
    (PSUpscaleSeedVR2, 'PS Upscale SeedVR2'), (PSSegmentSAM3, 'PS Segment SAM3'), (PSMatte, 'PS Matte (MatAnyone 2)'),
    (PSCameraPoses, 'PS Camera Poses (COLMAP)'), (PSTrainSplat, 'PS Train Splat (LichtFeld)'),
    (PSCleanSplat, 'PS Clean Splat'), (PSExportSplat, 'PS Export Splat'), (PSPreviewFrames, 'PS Preview Frames'),
    (PSLoadFrames, 'PS Load Frames'), (PSLoadSplat, 'PS Load Splat'),
]
for _c, _ in _ADVANCED:
    _c.CATEGORY = 'ProfSplat/advanced'
NODE_CLASS_MAPPINGS = {c.__name__: c for c, _ in _MAIN + _ADVANCED}
NODE_DISPLAY_NAME_MAPPINGS = {c.__name__: n for c, n in _MAIN + _ADVANCED}
