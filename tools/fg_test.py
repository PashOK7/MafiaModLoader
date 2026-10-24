"""fg_test.py: frame generation measured: for each mode, the game's own frames a second, the things shown a second
(its frames and the pictures between them), their spacing and where they are shown from (gfx.state's show).

    python tools/fg_test.py                              # off, 2x, 3x, 4x with the stand-in (any GPU)
    python tools/fg_test.py --modes 0,1,3 --real         # DLSS-FG itself (an RTX 40 or 50)

The game runs with the loader and dlss_nr on (DLSS on); each mode: its settings, 2 s to settle, then `seconds` measured.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pass_test import call   # noqa: E402


def show(game):
    return call(game, 'gfx.state').get('show', {})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default='1.2')
    ap.add_argument('--modes', default='0,1,2,3', help='frame_gen values: the frames made between two (0 off, 1 2x, 2 3x, 3 4x)')
    ap.add_argument('--seconds', type=float, default=3)
    ap.add_argument('--real', action='store_true', help='DLSS-FG itself, not the stand-in')
    a = ap.parse_args()
    call(a.game, 'dlss_nr.toggle', {'on': True})
    call(a.game, 'config.set', {'mod': 'dlss_nr', 'key': 'fg_stand_in', 'value': not a.real})
    try:
        for m in [int(x) for x in a.modes.split(',') if x.strip()]:
            call(a.game, 'config.set', {'mod': 'dlss_nr', 'key': 'frame_gen', 'value': m})
            time.sleep(2)
            s0, t0 = show(a.game), time.time()
            time.sleep(a.seconds)
            s1, t1 = show(a.game), time.time()
            dt = t1 - t0
            game = (s1.get('shows', 0) - s0.get('shows', 0)) / dt
            shown = (s1.get('frames', 0) - s0.get('frames', 0) + s1.get('pictures', 0) - s0.get('pictures', 0)) / dt
            if not m:
                shown = game
            print('%dx: the game %.1f fps, shown %.1f a second, gap %.2f ms (error %.2f), thread: %s%s' %
                  (m + 1, game, shown, s1.get('gap_ms', 0), s1.get('gap_error_ms', 0), s1.get('thread') or '-',
                   ', error: ' + s1['error'] if s1.get('error') else ''))
    finally:
        call(a.game, 'config.set', {'mod': 'dlss_nr', 'key': 'frame_gen', 'value': 0})


if __name__ == '__main__':
    main()
