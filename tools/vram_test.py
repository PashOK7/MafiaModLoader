"""vram_test.py: the GPU's video memory in use (Windows' "GPU Adapter Memory" counters, dedicated and shared) before and
after DLSS's mode is switched round many times: what a remake does not give back shows as growth.

    python tools/vram_test.py                     # 15 rounds of DLAA, Quality, Ultra Performance, frame generation
    python tools/vram_test.py --rounds 5 --only sr

The adapter's counters, not the processes': "GPU Process Memory" gave the sidecar 13 GB of "dedicated" memory on an
8 GB card after a few hundred mode switches while the adapter's stayed at 2.1 GB (2026-10-24): a shared texture's
allocation stays counted in the process that made it, it seems, after it is gone. The game runs with the loader and
dlss_nr on.
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pass_test import call   # noqa: E402


def adapter():
    """(dedicated MB, shared MB) in use on the GPU with the most dedicated memory in use."""
    ps = ("$c = (Get-Counter '\\GPU Adapter Memory(*)\\Dedicated Usage','\\GPU Adapter Memory(*)\\Shared Usage').CounterSamples;"
          "$c | ForEach-Object { $_.InstanceName + '|' + $_.Path + '=' + $_.CookedValue }")
    out = subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True, text=True).stdout.splitlines()
    got = {}
    for line in out:
        if '=' not in line or '|' not in line:
            continue
        inst, rest = line.split('|', 1)
        path, value = rest.rsplit('=', 1)
        got.setdefault(inst, [0.0, 0.0])[0 if 'dedicated' in path.lower() else 1] = float(value) / 1e6
    return tuple(max(got.values(), key=lambda v: v[0])) if got else (0.0, 0.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default='1.2')
    ap.add_argument('--rounds', type=int, default=15)
    ap.add_argument('--only', choices=('sr', 'fg'), help='switch Super Resolution\'s modes alone, or frame generation alone')
    a = ap.parse_args()
    call(a.game, 'dlss_nr.toggle', {'on': True})
    steps = [('upscale', 1), ('upscale', 2), ('upscale', 5), ('frame_gen', 1), ('frame_gen', 0)]
    if a.only:
        steps = [s for s in steps if (s[0] == 'upscale') == (a.only == 'sr')]
    call(a.game, 'config.set', {'mod': 'dlss_nr', 'key': steps[0][0], 'value': steps[0][1]})
    time.sleep(3)
    before = adapter()
    for r in range(a.rounds):
        for k, v in steps:
            call(a.game, 'config.set', {'mod': 'dlss_nr', 'key': k, 'value': v})
            time.sleep(0.8)
    call(a.game, 'config.set', {'mod': 'dlss_nr', 'key': steps[0][0], 'value': steps[0][1]})
    call(a.game, 'config.set', {'mod': 'dlss_nr', 'key': 'frame_gen', 'value': 0})
    time.sleep(3)
    after = adapter()
    print('the GPU: dedicated %.0f -> %.0f MB (%+.0f), shared %.0f -> %.0f MB (%+.0f) over %d switches' % (
        before[0], after[0], after[0] - before[0], before[1], after[1], after[1] - before[1], a.rounds * len(steps)))


if __name__ == '__main__':
    main()
