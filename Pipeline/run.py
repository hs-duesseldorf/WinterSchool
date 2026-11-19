"""Person -> Gaussian splat without opening the UI: starts ComfyUI if needed, submits the workflow from
build_workflow.py, waits, prints where the results are. Same nodes and the same stage cache as the UI: rerun with a
changed option and only the affected stages run again.

usage: python run.py --run NAME [--prompt "..."] [--image PHOTO] [--as-is] [--height 1.78]
                     [--orbit climb|level|wave|rings] [--seed 1] [--no-views] [--no-upscale] [--dry-run] [--port 8188]

  --run      output folder: ComfyUI/output/profsplat/<NAME>/<step>/
  --prompt   who the person is (clothes, hair, ...)
  --image    optional photo of the person: the image is generated from it (--as-is: used unchanged)
  --dry-run  generate and train nothing, walk the pipeline on the stored professor results

examples:
  python run.py --run professor --image Eingabebilder/Professor_hero.png --as-is   # the professor as before
  python run.py --run alice --prompt "A woman in her thirties, red hoodie, jeans, white sneakers"
  python run.py --run bob --image bob.jpg --prompt "Bob, in a dark suit"
  python run.py --run demo --dry-run
"""
import argparse, json, os, shutil, subprocess, sys, time, urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ.get('PROFSPLAT_ROOT') or HERE / 'work')
COMFY = ROOT / 'third_party' / 'ComfyUI'
sys.path.insert(0, str(HERE))
from build_workflow import PROFESSOR, build, object_info  # noqa: E402


def get(url):
    with urllib.request.urlopen(url, timeout=10) as r:
        return json.loads(r.read())


def up(base):
    try:
        get(base + '/system_stats'); return True
    except Exception:
        return False


def submit(base, wf):
    req = urllib.request.Request(base + '/prompt', data=json.dumps({'prompt': wf}).encode(),
                                 headers={'Content-Type': 'application/json'})
    try:
        pid = json.loads(urllib.request.urlopen(req).read())['prompt_id']
    except urllib.error.HTTPError as e:
        sys.exit('rejected: ' + e.read().decode()[:4000])
    print('prompt', pid, flush=True)
    t0 = time.time()
    while True:
        time.sleep(10)
        h = get(f'{base}/history/{pid}')
        if pid in h:
            st = h[pid]['status']
            print('status', st.get('status_str'), f'{time.time() - t0:.0f}s', flush=True)
            for m in st.get('messages', []):
                if m[0] == 'execution_error':
                    print(json.dumps(m[1], indent=1)[:6000])
            return st.get('status_str') == 'success'


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--run', required=True)
    p.add_argument('--prompt', default=PROFESSOR); p.add_argument('--image', default=None)
    p.add_argument('--as-is', action='store_true'); p.add_argument('--dry-run', action='store_true')
    p.add_argument('--height', type=float, default=1.78)
    p.add_argument('--orbit', default='climb', choices=['climb', 'level', 'wave', 'rings'])
    p.add_argument('--seed', type=int, default=1); p.add_argument('--port', type=int, default=8188)
    p.add_argument('--no-views', action='store_true'); p.add_argument('--no-upscale', action='store_true')
    a = p.parse_args()
    if a.as_is and not a.image:
        sys.exit('--as-is needs --image')
    base = f'http://127.0.0.1:{a.port}'
    if not up(base):
        subprocess.run([str(HERE / 'comfyui.sh')], env={**os.environ, 'PORT': str(a.port), 'NO_BROWSER': '1'}, check=True)
    image = None
    if a.image:
        image = f'{a.run}_photo{Path(a.image).suffix}'
        shutil.copy(a.image, COMFY / 'input' / image)
    g = build(a.run, a.prompt, image, a.as_is, a.orbit, a.seed, a.height, not a.no_views, not a.no_upscale,
              a.dry_run)
    wf = g.api(object_info(a.port))
    (ROOT / 'logs' / f'{a.run}_api.json').write_text(json.dumps(wf, indent=1))
    ok = submit(base, wf)
    out = COMFY / 'output' / 'profsplat' / a.run
    if ok and a.dry_run:
        print(f'dry run done: previews in {COMFY}/output/profsplat/_dry/')
    elif ok:
        print(f'done: {out}/export_{a.run}/ (splat.ply .spz .sog .html, views.png)')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
