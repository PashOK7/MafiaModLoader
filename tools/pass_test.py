"""pass_test.py: a repeatable test of the picture on a moving car: the player's car driven past a still camera, the shot
taken in the frame the car crosses the camera's line of sight (screenshot.when, in the game's own Present), once for
each mode asked; the frames and the car's part of them side by side.

    python tools/pass_test.py                          # the scene "taxi", the modes off, sr_quality, sr_ultra
    python tools/pass_test.py --modes off,sr_ultra,nr_sr_perf --game 1.2 --out build/pass_test
    python tools/pass_test.py --scene my.json          # a scene of one's own (the keys of SCENES below)
    python tools/pass_test.py --speed 1.4 --modes motion_check   # the car at a steady 5 km/h

The game runs with the loader and the player sits in a car (a freeride: game_launch {"go": "freeride"}); dlss_nr is on
at the start for the DLSS modes ("gfx": true). Each run: the car put at the scene's start, facing its way, stopped;
the camera held at the scene's eye; the mode set; 1.5 s for the picture to settle; the shot armed on the vertical plane
through the camera's line of sight; `accelerate_ms` of Accelerate. The car is in the middle of the frame, sideways, at
the same place of its drive every time (the frame's `crossing` says how far past the plane it was: one frame's travel at
most). Pillow makes the PNGs; the server built (tools/mcp_call.py) is the one used.
"""
import argparse
import json
import math
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# eye and fwd: the camera; car and dir: the car's start and the way it faces (it drives forward); the shot when the car
# crosses the camera's line of sight. "taxi": the owner's, in the 1.2 lab's city-small freeride (2026-10-23): the
# camera on the parking lot's edge looking north, the taxi 12 m ahead and 12 m to the right, facing west.
SCENES = {
    'taxi': {'eye': [-1967.555, -2.592, 5.258], 'fwd': [0.0174, -0.1054, 0.9943],
             'car': [-1955.188, -4.537, 17.641], 'dir': [-0.9996, -0.0048, -0.0278], 'accelerate_ms': 6000},
}

# what each mode sets in dlss_nr (and whether DLSS is on at all); the checks are its debug views at the frame's size:
# the frame before moved by the motion vectors against this one (black where they are right), and not moved
MODES = {
    'off': {'on': False, 'debug': 0, 'mv_scale': 1},
    'sr_dlaa': {'on': True, 'pipeline': False, 'debug': 0, 'nr': False, 'upscale': 1, 'mv_scale': 1},
    'sr_quality': {'on': True, 'pipeline': False, 'debug': 0, 'nr': False, 'upscale': 2, 'mv_scale': 1},
    'sr_balanced': {'on': True, 'pipeline': False, 'debug': 0, 'nr': False, 'upscale': 3, 'mv_scale': 1},
    'sr_perf': {'on': True, 'pipeline': False, 'debug': 0, 'nr': False, 'upscale': 4, 'mv_scale': 1},
    'sr_ultra': {'on': True, 'pipeline': False, 'debug': 0, 'nr': False, 'upscale': 5, 'mv_scale': 1},
    'nr': {'on': True, 'pipeline': False, 'debug': 0, 'nr': True, 'upscale': 0, 'mv_scale': 1},
    'nr_sr_perf': {'on': True, 'pipeline': False, 'debug': 0, 'nr': True, 'upscale': 4, 'mv_scale': 1},
    'nr_sr_ultra': {'on': True, 'pipeline': False, 'debug': 0, 'nr': True, 'upscale': 5, 'mv_scale': 1},
    'sr_ultra_mv0': {'on': True, 'pipeline': False, 'debug': 0, 'nr': False, 'upscale': 5, 'mv_scale': 0},
    'sr_ultra_mv1': {'on': True, 'pipeline': False, 'debug': 0, 'nr': False, 'upscale': 5, 'mv_scale': 1},
    'sr_ultra_mvneg': {'on': True, 'pipeline': False, 'debug': 0, 'nr': False, 'upscale': 5, 'mv_scale': -1},
    'motion': {'on': True, 'pipeline': False, 'debug': 1, 'mv_scale': 1},
    'motion_dlss_ultra': {'on': True, 'pipeline': False, 'debug': 7, 'nr': False, 'upscale': 5, 'mv_scale': 1},
    'motion_check': {'on': True, 'pipeline': False, 'debug': 5, 'mv_scale': 1},
    'motion_check_x0.5': {'on': True, 'pipeline': False, 'debug': 5, 'mv_scale': 0.5},
    'motion_check_x1.5': {'on': True, 'pipeline': False, 'debug': 5, 'mv_scale': 1.5},
    'motion_check_x2': {'on': True, 'pipeline': False, 'debug': 5, 'mv_scale': 2},
    'still_check': {'on': True, 'pipeline': False, 'debug': 6, 'mv_scale': 1},
}


def tool(name, args):
    out = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'mcp_call.py'), name, json.dumps(args)],
                         capture_output=True, text=True, cwd=ROOT).stdout
    i = min([k for k in (out.find('{'), out.find('[')) if k >= 0], default=-1)
    if i < 0:
        raise RuntimeError('%s: %s' % (name, out.strip()[:400]))
    return json.loads(out[i:])


def call(game, method, params=None):
    return tool('call', {'game': game, 'method': method, 'params': params or {}})


def norm(v):
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


def read_bgra(path):
    from PIL import Image
    data = open(path, 'rb').read()
    w, h = int.from_bytes(data[4:8], 'little'), int.from_bytes(data[8:12], 'little')
    return Image.frombuffer('RGBA', (w, h), data[16:16 + w * h * 4], 'raw', 'BGRA', 0, 1).convert('RGB')


def run(game, scene, mode, out, speed=0.0):
    eye, fwd = scene['eye'], norm(scene['fwd'])
    right = norm([fwd[2], 0.0, -fwd[0]])   # the horizontal across the line of sight: the plane's normal
    for k, v in MODES[mode].items():
        if k == 'on':
            call(game, 'dlss_nr.toggle', {'on': v})
        else:
            call(game, 'config.set', {'mod': 'dlss_nr', 'key': k, 'value': v})
    tool('input', {'game': game, 'steps': [{'action': 'Brake', 'ms': 50}]})
    tool('teleport', {'game': game, 'actor': 'car', 'to': scene['car'], 'dir': scene['dir']})
    call(game, 'camera.set', {'eye': eye, 'at': [eye[i] + fwd[i] * 20 for i in range(3)], 'ms': 60000})
    time.sleep(1.5)   # (the car settled, DLSS's history made with the new mode)
    shot = call(game, 'screenshot.when', {'actor': 'car', 'at': eye, 'normal': right, 'timeout_ms': 15000})['id']
    if speed > 0:   # (a steady speed, the game's physics left out: vehicle.move)
        call(game, 'vehicle.move', {'car': 'car', 'velocity': [x * speed for x in norm(scene['dir'])], 'turn': [0, 0, 0],
                                    'ms': int(1000 * 20 / speed) + 2000})
    else:
        tool('input', {'game': game, 'steps': [{'action': 'Accelerate', 'ms': scene['accelerate_ms']}], 'no_wait': True})
    t0 = time.time()
    while True:
        st = call(game, 'screenshot.state', {'id': shot})
        if st.get('done'):
            break
        if time.time() - t0 > 20:
            raise RuntimeError('%s: no shot in 20 s' % mode)
        time.sleep(0.1)
    call(game, 'input.release')
    if speed > 0:
        call(game, 'vehicle.move', {'car': 'car', 'ms': 0})
    if not st.get('ok'):
        raise RuntimeError('%s: %s' % (mode, st.get('error')))
    img = read_bgra(st['path'])
    img.save(os.path.join(out, mode + '.png'))
    w, h = img.size
    crop = img.crop((w * 3 // 10, h * 3 // 10, w * 7 // 10, h * 7 // 10))   # the car's part: the middle of the frame
    crop.save(os.path.join(out, mode + '_car.png'))
    c = st.get('crossing', {})
    print('%-12s the car %.2f m past the line of sight at %s' % (mode, c.get('past', 0), [round(x, 2) for x in c.get('pos', [])]))
    return crop


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default='1.2')
    ap.add_argument('--scene', default='taxi', help='a name in SCENES or a JSON file of its keys')
    ap.add_argument('--modes', default='off,sr_quality,sr_ultra', help=','.join(MODES))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'pass_test'))
    ap.add_argument('--speed', type=float, default=0, help='m/s, steady (vehicle.move); 0: the scene\'s Accelerate')
    a = ap.parse_args()
    scene = SCENES[a.scene] if a.scene in SCENES else json.load(open(a.scene, encoding='utf-8'))
    modes = [m.strip() for m in a.modes.split(',') if m.strip()]
    for m in modes:
        if m not in MODES:
            sys.exit('unknown mode %s (%s)' % (m, ', '.join(MODES)))
    os.makedirs(a.out, exist_ok=True)
    crops = [(m, run(a.game, scene, m, a.out, a.speed)) for m in modes]
    call(a.game, 'camera.release')
    from PIL import Image, ImageDraw
    w, h = crops[0][1].size
    sheet = Image.new('RGB', (w * len(crops), h + 24), 'black')
    for i, (m, c) in enumerate(crops):
        sheet.paste(c, (i * w, 24))
        ImageDraw.Draw(sheet).text((i * w + 6, 5), m, fill='white')
    path = os.path.join(a.out, 'compare.png')
    sheet.save(path)
    print(path)


if __name__ == '__main__':
    main()
