"""Real bridge regression: an executing job survives timeout; queued jobs are cancelled.

Build MafiaModLoader, mml_harness and testmod_kit first. Uses an isolated stand-in, no game folders.
"""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from run_tests import Bridge, ROOT


def exercise(b):
    before = b.call('kit.slow_reply',{'status':True})['result']['calls']
    payload = 'request-owned-buffer-'*120
    normal = b.call('kit.slow_reply',{'payload':payload,'timeout_ms':2000})
    assert normal['ok'] and normal['result']['payload']==payload,normal
    for i in range(12):
        result = b.call('kit.slow_reply',{'payload':payload,'timeout_ms':80})
        assert not result['ok'] and 'did not answer in 80 ms' in result['error'],result
        # The first command is still running; this one expires in the queue and must not execute.
        queued = b.call('kit.slow_reply',{'payload':'cancelled','timeout_ms':20})
        assert not queued['ok'] and 'did not answer in 20 ms' in queued['error'],queued
        status = b.call('kit.slow_reply',{'status':True,'timeout_ms':2000})
        assert status['ok'] and status['result']['calls']==before+i+2,status
        assert b.call('ping')['ok']


def main():
    root = Path(ROOT)
    harness = root / 'build/x86/harness'
    with tempfile.TemporaryDirectory(prefix='bridge-timeout-', dir=root/'build') as folder:
        folder = Path(folder)
        assert folder.resolve().is_relative_to((root/'build').resolve())
        for src in harness.iterdir():
            if src.is_file() and src.suffix.lower() in ('.exe','.dll','.asi'):
                shutil.copyfile(src,folder/src.name)
        shutil.copyfile(root/'build/x86/out/MafiaModLoader.asi',folder/'MafiaModLoader.asi')
        shutil.copytree(harness/'mml/mods/kit',folder/'mml/mods/kit')
        proc = subprocess.Popen([str(folder/'mml_harness.exe'),'--seconds','30'],cwd=folder,
                                stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        b = None
        try:
            info_path = folder/'mml/bridge.json'
            deadline = time.monotonic()+10
            while not info_path.exists() and time.monotonic()<deadline and proc.poll() is None:
                time.sleep(.02)
            b = Bridge(json.loads(info_path.read_text()))
            exercise(b)
            print('PASS: 12 executing timeouts complete safely; 12 queued timeouts never execute; long reply retained')
        finally:
            (folder/'quit').touch()
            out,_ = proc.communicate(timeout=10)
            if b:
                b.f.close()
                b.s.close()
            if proc.returncode:
                print(out.decode(errors='replace')[-2000:])
            assert proc.returncode==0,proc.returncode
    return 0


if __name__=='__main__':
    raise SystemExit(main())
