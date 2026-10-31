"""Fast repeat of the real C/C++/Rust SDK ownership tests, after a normal x86 build.

    python tests/atmosphere_sdk_smoke.py

Uses only the stand-in game's build/x86/harness folder, never the labs. The full offline suite remains required
for release; this entry point shortens iteration on the public service without rebuilding temporary example mods.
"""
import json
from pathlib import Path
import subprocess
import sys
import time

import run_tests as suite

sys.path.insert(0,str(Path(suite.ROOT)/'tools'))
import lab
from migrate_atmosphere import guarded_write


def main():
    harness=Path(suite.HARNESS)
    executable=harness/'mml_harness.exe'
    if any(lab.same_dir(str(executable),p) for p in lab.process_paths()):
        raise RuntimeError('a stand-in game is already running in '+str(harness))
    if not executable.exists():
        raise RuntimeError('build the x86 harness and test mods first')
    def copy(source,destination):
        if not source.is_file():
            raise RuntimeError('build first: '+str(source))
        guarded_write(destination,destination.read_bytes() if destination.exists() else None,source.read_bytes())
    copy(Path(suite.OUT)/'MafiaModLoader.asi',harness/'MafiaModLoader.asi')
    source=Path(suite.OUT)/'mml/mods/atmosphere'
    for path in source.rglob('*'):
        if path.is_file():
            copy(path,harness/'mml/mods/atmosphere'/path.relative_to(source))
    process=subprocess.Popen([str(executable),'--seconds','30'],cwd=harness,stdout=subprocess.DEVNULL,stderr=subprocess.STDOUT)
    bridge=None
    try:
        info_path=harness/'mml/bridge.json'
        for _ in range(100):
            try:
                info=json.loads(info_path.read_text(encoding='utf-8'))
                if info.get('pid')==process.pid:
                    bridge=suite.Bridge(info)
                    break
            except (OSError,ValueError):
                pass
            time.sleep(0.05)
        if bridge is None:
            raise RuntimeError('stand-in bridge did not become ready')
        suite.atmosphere_sdk_tests(bridge)
        suite.atmosphere_migration_tests()
    finally:
        # The stand-in game exits through its own quit marker, as run_tests.py does; "quit" is not a host method.
        quit_path=harness/'quit'
        if not quit_path.exists():
            quit_path.touch(exist_ok=False)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    print('ALL PASSED' if not suite.failures and process.returncode==0 else 'FAILED (exit %s)'%process.returncode)
    return int(bool(suite.failures) or process.returncode!=0)


if __name__=='__main__':
    sys.exit(main())
