"""Explicit foreground automation. Opt in with MML_TEST_FOCUS=1; ordinary suites keep their background policy."""
import os
from pathlib import Path
import sys
import threading
import time
from mml_game import Skip

_focus_lock = threading.Lock()

def test_explicit_focus_bypasses_launch_guard(g):
    if os.environ.get('MML_TEST_FOCUS') != '1':
        raise Skip('MML_TEST_FOCUS=1 explicitly allows foreground switching')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
    from run_tests import export_rva
    dll = os.path.join(os.environ['WINDIR'],'SysWOW64/user32.dll')
    get = export_rva(dll,'GetForegroundWindow')
    set_foreground = export_rva(dll,'SetForegroundWindow')
    def native(rva,argv=None):
        r=g.call('native.call',at='user32.dll+%X'%rva,conv='stdcall',args=argv or [],ret='uint')
        assert r['stack']['ok'],r
        return r['ret']
    with _focus_lock:
        window=g.call('window.state')['cursor_window']
        original=native(get)
        try:
            g.call('window.resize',state='minimize')
            time.sleep(.1)
            background=native(get)
            g.check('target is minimized before explicit activation',g.call('window.state')['minimized'])
            result=native(set_foreground,[window])
            time.sleep(.1)
            g.check('ordinary background launch guard still suppresses own activation',
                    result!=0 and native(get)==background,{'api_result':result,'foreground':native(get)})
            focus=g.call('window.focus')
            deadline=time.monotonic()+1
            while native(get)!=window and time.monotonic()<deadline:time.sleep(.02)
            g.check('explicit focus reports the native request and guard bypass',
                    focus.get('activation_result') and focus.get('launch_guard_bypassed'),focus)
            g.check('actual foreground window is the requested game',native(get)==window,focus)
            g.check('explicit focus restores a minimized game',not g.call('window.state')['minimized'])
        finally:
            if original:native(set_foreground,[original])
