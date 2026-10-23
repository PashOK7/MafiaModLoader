"""still_test.py: a repeatable test of the picture's stillness: the camera held still on a still scene, a few frames
taken in each mode, each frame against the one before. Without DLSS they are the same; with it, a temporal upscaler
that does not undo its jitter makes the picture shake by its pixels (seen by eye, not in a single shot).

    python tools/still_test.py                                   # the modes off, sr_ultra, sr_ultra_pipe
    python tools/still_test.py --modes off,sr_quality --shots 12 --game 1.2

Each mode: its settings in dlss_nr, 1.5 s for the history to settle, `shots` frames one after another; printed: the
mean difference of two frames in a row (0..255) and the share of pixels that changed by more than 8. The game runs
with the loader (dlss_nr on at the start for the DLSS modes); the frames are kept in --out.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pass_test import MODES, SCENES, call, norm, read_bgra, tool   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STILL_MODES = dict(MODES)
STILL_MODES['sr_ultra_pipe'] = dict(MODES['sr_ultra'], pipeline=True)
STILL_MODES['sr_quality_pipe'] = dict(MODES['sr_quality'], pipeline=True)


def shot(game):
    sid = call(game, 'screenshot')['id']
    t0 = time.time()
    while True:
        st = call(game, 'screenshot.state', {'id': sid})
        if st.get('done'):
            if not st.get('ok'):
                raise RuntimeError(st.get('error'))
            return read_bgra(st['path'])
        if time.time() - t0 > 10:
            raise RuntimeError('no shot in 10 s')
        time.sleep(0.03)


def main():
    from PIL import ImageChops, ImageStat
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default='1.2')
    ap.add_argument('--scene', default='taxi', help='a name in pass_test.SCENES (its eye and fwd)')
    ap.add_argument('--modes', default='off,sr_ultra,sr_ultra_pipe', help=','.join(STILL_MODES))
    ap.add_argument('--shots', type=int, default=8)
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'still_test'))
    a = ap.parse_args()
    eye, fwd = SCENES[a.scene]['eye'], norm(SCENES[a.scene]['fwd'])
    os.makedirs(a.out, exist_ok=True)
    call(a.game, 'hud.game', {'show': False})
    call(a.game, 'camera.set', {'eye': eye, 'at': [eye[i] + fwd[i] * 20 for i in range(3)], 'ms': 600000})
    try:
        for mode in [m.strip() for m in a.modes.split(',') if m.strip()]:
            for k, v in STILL_MODES[mode].items():
                if k == 'on':
                    call(a.game, 'dlss_nr.toggle', {'on': v})
                else:
                    call(a.game, 'config.set', {'mod': 'dlss_nr', 'key': k, 'value': v})
            time.sleep(1.5)
            frames = [shot(a.game) for _ in range(a.shots)]
            means, moved = [], []
            for i in range(1, len(frames)):
                d = ImageChops.difference(frames[i - 1], frames[i]).convert('L')
                means.append(ImageStat.Stat(d).mean[0])
                hist = d.histogram()
                moved.append(sum(hist[9:]) / float(sum(hist)))
            for i, f in enumerate(frames[:3]):
                f.save(os.path.join(a.out, '%s_%d.png' % (mode, i)))
            print('%-16s mean diff %.2f (max %.2f), pixels changed >8: %.1f%%' %
                  (mode, sum(means) / len(means), max(means), 100 * sum(moved) / len(moved)))
    finally:
        call(a.game, 'camera.release')
        call(a.game, 'hud.game', {'show': True})


if __name__ == '__main__':
    main()
