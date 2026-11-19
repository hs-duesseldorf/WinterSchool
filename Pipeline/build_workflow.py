"""Write the ProfSplat workflow for ComfyUI, in both formats:
  <name>.json      UI workflow (Workflows sidebar, or drag into ComfyUI)
  <name>_api.json  API prompt (what run.py submits)

Four nodes left to right, one group each: 1 Image -> 2 Views -> 3 360° Video -> 4 Gaussian Splat, plus an optional
photo (Load Image) and a note. Each node shows its result on itself. The UI format needs each node's widget order,
read from a running ComfyUI (/object_info) or from object_info.json next to this file (refreshed whenever a server
answers).

usage: python build_workflow.py [--name Professor] [--run professor] [--prompt "..."] [--image Professor_hero.png]
       [--as-is] [--orbit climb] [--seed 1] [--height 1.78] [--no-views] [--no-upscale] [--dry-run] [--port 8188]"""
import argparse, json, urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent

PROFESSOR = ('A man in his sixties with short, side-parted dark-blond hair going grey, a friendly lined face and '
             'blue eyes. He wears a navy-blue short-sleeved polo shirt, loose light-grey chino trousers, beige-grey '
             'sneakers and a black wristwatch on his left wrist.')
GROUP_COLORS = ['#3f789e', '#8a6d3b', '#a1309b', '#3f8f5f', '#b06634']


def object_info(port):
    cache = HERE / 'object_info.json'
    try:
        with urllib.request.urlopen(f'http://127.0.0.1:{port}/object_info', timeout=10) as r:
            info = json.loads(r.read())
        keep = {k: v for k, v in info.items() if k.startswith('PS') or k in ('LoadImage', 'PreviewImage')}
        cache.write_text(json.dumps(keep, indent=1))
        return keep
    except Exception:
        if cache.exists():
            return json.loads(cache.read_text())
        raise SystemExit(f'no ComfyUI on port {port} and no {cache}: start it with comfyui.sh first')


class Graph:
    def __init__(self):
        self.nodes, self.groups = [], []

    def add(self, cls, title, pos, group, **inputs):
        """inputs: widget values, or (node, slot) tuples for links."""
        size = inputs.pop('_size', [400, 200])
        n = {'id': len(self.nodes) + 1, 'cls': cls, 'title': title, 'pos': pos, 'size': size, 'group': group,
             'inputs': inputs}
        self.nodes.append(n)
        return n

    def group(self, title, x, y, w, h):
        self.groups.append({'title': title, 'bounding': [x, y, w, h],
                            'color': GROUP_COLORS[len(self.groups) % len(GROUP_COLORS)], 'font_size': 24, 'flags': {}})

    def api(self, info):
        out = {}
        for n in self.nodes:
            if n['cls'] == 'Note':
                continue
            inputs = {}
            spec = info.get(n['cls'], {}).get('input', {})
            for name, s in {**spec.get('required', {}), **spec.get('optional', {})}.items():
                cfg = s[1] if len(s) > 1 else {}
                if name in n['inputs']:
                    v = n['inputs'][name]
                    inputs[name] = [str(v[0]['id']), v[1]] if isinstance(v, tuple) else v
                elif name in spec.get('required', {}) and (isinstance(s[0], list) or 'default' in cfg):
                    inputs[name] = cfg.get('default', s[0][0] if isinstance(s[0], list) else None)
                elif 'default' in cfg:
                    inputs[name] = cfg['default']
            out[str(n['id'])] = {'class_type': n['cls'], '_meta': {'title': n['title']}, 'inputs': inputs}
        return out

    def ui(self, info):
        links, nodes = [], []
        outs = {}  # (node id, slot) -> [link ids]
        order = {n['id']: i for i, n in enumerate(self.nodes)}
        for n in self.nodes:
            spec = info.get(n['cls'], {})
            req, opt = spec.get('input', {}).get('required', {}), spec.get('input', {}).get('optional', {})
            names = spec.get('input_order', {}).get('required', list(req)) + \
                spec.get('input_order', {}).get('optional', list(opt))
            allspec = {**req, **opt}
            ins, widgets = [], []
            for name in names:
                t, cfg = allspec[name][0], (allspec[name][1] if len(allspec[name]) > 1 else {})
                is_widget = isinstance(t, list) or t in ('INT', 'FLOAT', 'STRING', 'BOOLEAN', 'COMBO')
                v = n['inputs'].get(name)
                if isinstance(v, tuple):
                    lid = len(links) + 1
                    src, slot = v
                    ltype = info[src['cls']]['output'][slot]
                    links.append([lid, src['id'], slot, n['id'], len(ins), ltype])
                    outs.setdefault((src['id'], slot), []).append(lid)
                    ins.append({'name': name, 'type': ltype if not is_widget else (t if isinstance(t, str) else 'COMBO'),
                                'link': lid, **({'widget': {'name': name}} if is_widget else {})})
                    if is_widget:
                        widgets.append(cfg.get('default'))
                elif is_widget:
                    widgets.append(v if v is not None else cfg.get('default'))
                    if cfg.get('control_after_generate') or (t == 'INT' and name in ('seed', 'noise_seed')):
                        widgets.append('fixed')
                    if cfg.get('image_upload'):
                        widgets.append('image')
                else:
                    ins.append({'name': name, 'type': t, 'link': None})
            if n['cls'] == 'Note':
                widgets = [n['inputs']['text']]
            nodes.append({'id': n['id'], 'type': n['cls'], 'pos': n['pos'], 'size': n['size'],
                          'flags': {}, 'order': order[n['id']], 'mode': 0, 'title': n['title'], 'inputs': ins,
                          'outputs': [], 'properties': {'Node name for S&R': n['cls']}, 'widgets_values': widgets})
            nodes[-1]['outputs'] = [{'name': nm, 'type': t, 'links': outs.get((n['id'], i), []), 'slot_index': i}
                                    for i, (t, nm) in enumerate(zip(spec.get('output', []),
                                                                    spec.get('output_name', spec.get('output', []))))]
        # outputs reference links created by later nodes: fill them in after all links exist
        for nd in nodes:
            for i, o in enumerate(nd['outputs']):
                o['links'] = outs.get((nd['id'], i), [])
        return {'last_node_id': len(nodes), 'last_link_id': len(links), 'nodes': nodes, 'links': links,
                'groups': self.groups, 'config': {}, 'extra': {'ds': {'scale': 0.6, 'offset': [40, 40]}},
                'version': 0.4}


def build(run='professor', prompt=PROFESSOR, image='Professor_hero.png', as_is=False, orbit='climb', seed=1,
          height_m=1.78, views=True, upscale=True, dry_run=False):
    """image: photo in ComfyUI/input (None: from the prompt alone). as_is: use the photo unchanged."""
    g = Graph()
    W, GAP, Y = 460, 60, 0
    x = lambda i: 60 + i * (W + 2 * GAP)
    g.add('Note', 'Person -> Gaussian splat', [x(0), -460], 0, _size=[4 * (W + 2 * GAP) - 2 * GAP, 320], text=(
        'Prompt (+ optional photo) -> Gaussian splat, in four steps. Press Queue (Run).\n\n'
        '1 Image   FLUX.2 Klein: the person in an A-pose in a grey studio (from the photo if one is connected)\n'
        '2 Views   side and back views on the same grey: scout orbit, cut out, re-lit\n'
        '3 Video   MiniMax H3 circles the person, pinned to those views\n'
        '4 Splat   upscale, masks, matte, camera poses, training, clean-up, export\n\n'
        'Each step keeps its result in ComfyUI/output/profsplat/<run>/ and is skipped while its settings and inputs '
        'are unchanged: change one setting and only that step and the ones after it run again.\n'
        'dry_run (node 1): generates nothing, shows the stored result of every step of the professor run.'))
    g.group('1  Image', x(0) - GAP / 2, Y - 60, W + GAP, 1380)
    photo = None
    if image:
        photo = g.add('LoadImage', 'Photo (optional)', [x(0), Y + 640], 1, _size=[W, 640], image=image)
    i1 = dict(run=run, prompt=prompt, photo_use='as is' if as_is else 'reference', seed=seed, dry_run=dry_run)
    if photo:
        i1['photo'] = (photo, 0)
    n1 = g.add('PSImage', '1 · Image', [x(0), Y], 1, _size=[W, 600], **i1)
    g.group('2  Views', x(1) - GAP / 2, Y - 60, W + GAP, 1380)
    n2 = g.add('PSViews', '2 · Views', [x(1), Y], 2, _size=[W, 900], pipe=(n1, 0), orbit=orbit, seed=seed,
               views='grey keyframes' if views else 'none')
    g.group('3  360° Video', x(2) - GAP / 2, Y - 60, W + GAP, 1380)
    n3 = g.add('PSVideo', '3 · 360° Video', [x(2), Y], 3, _size=[W, 900], pipe=(n2, 0), seed=seed)
    g.group('4  Gaussian Splat', x(3) - GAP / 2, Y - 60, W + GAP, 1380)
    g.add('PSSplat', '4 · Gaussian Splat', [x(3), Y], 4, _size=[W, 1300], pipe=(n3, 0), height_m=height_m,
          upscale=upscale)
    return g


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--name', default='Professor'); p.add_argument('--run', default='professor')
    p.add_argument('--prompt', default=PROFESSOR)
    p.add_argument('--image', default='Professor_hero.png', help='photo in ComfyUI/input ("" = none)')
    p.add_argument('--as-is', action='store_true', help='use the photo unchanged instead of generating from it')
    p.add_argument('--orbit', default='climb'); p.add_argument('--seed', type=int, default=1)
    p.add_argument('--height', type=float, default=1.78)
    p.add_argument('--no-views', action='store_true'); p.add_argument('--no-upscale', action='store_true')
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--port', type=int, default=8188); p.add_argument('--out', default=str(HERE))
    a = p.parse_args()
    g = build(a.run, a.prompt, a.image or None, a.as_is, a.orbit, a.seed, a.height, not a.no_views,
              not a.no_upscale, a.dry_run)
    out = Path(a.out)
    info = object_info(a.port)
    (out / f'{a.name}_api.json').write_text(json.dumps(g.api(info), indent=1))
    (out / f'{a.name}.json').write_text(json.dumps(g.ui(info), indent=1))
    print('wrote', out / f'{a.name}.json', 'and', out / f'{a.name}_api.json')


if __name__ == '__main__':
    main()
