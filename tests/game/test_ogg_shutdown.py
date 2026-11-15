"""Native archive-lock cancellation regression. No desktop input/activation; all music is muted.

The old engine killed the decoder while it owned rw_data's shared critical section. Hold that
exact scheduling window, request normal close, then release it: a cooperative close MUST wait,
let the reader unwind, and leave no orphan lock. Run only with the cooperative hook present.
"""
import ctypes as c
from ctypes import wintypes as w
import re
import struct
import threading
import time


def test_ogg_immediate_close(g):
    before = g.call('sound.compat')['ogg_shutdown']
    g.check('cooperative Ogg hooks installed', before['hooked'], before)
    if not before['hooked']:
        return
    for _ in range(32):
        music = g.call('music.play', file='13 - Game Over.ogg', volume=0)['music']
        g.call('music.stop', music=music, timeout_ms=3000)
    after = g.call('sound.compat')['ogg_shutdown']
    g.check('immediate closes join all new workers, including late thread entry',
            after['started'] - before['started'] == 32 and
            after['exited'] - before['exited'] == 32 and
            after['joined'] - before['joined'] == 32 and
            after['states'] == before['states'] and after['failures'] == before['failures'], after)


def test_ogg_close_during_archive_read(g):
    before = g.call('sound.compat')['ogg_shutdown']
    g.check('adversarial schedule requires cooperative hook', before['hooked'], before)
    if not before['hooked']:
        return
    kernel = c.WinDLL('kernel32', use_last_error=True)
    def api(name, ret, *args):
        fn = getattr(kernel, name)
        fn.restype, fn.argtypes = ret, args
        return fn
    open_process = api('OpenProcess', w.HANDLE, w.DWORD, w.BOOL, w.DWORD)
    open_thread = api('OpenThread', w.HANDLE, w.DWORD, w.BOOL, w.DWORD)
    thread_pid = api('GetProcessIdOfThread', w.DWORD, w.HANDLE)
    suspend = api('SuspendThread', w.DWORD, w.HANDLE)
    resume = api('ResumeThread', w.DWORD, w.HANDLE)
    get_exit = api('GetExitCodeThread', w.BOOL, w.HANDLE, c.POINTER(w.DWORD))
    read = api('ReadProcessMemory', w.BOOL, w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t))
    close = api('CloseHandle', w.BOOL, w.HANDLE)
    status = g.tool('game_status')
    pid, build = status['ping']['pid'], status['ping']['build']
    process = open_process(0x410, False, pid)
    assert process, c.WinError(c.get_last_error())
    thread = None
    held = False
    music = tap = closer = None
    outcome = {}

    def memory(address, size):
        data, got = c.create_string_buffer(size), c.c_size_t()
        assert read(process, address, data, size, c.byref(got)) and got.value == size
        return bytes(data)

    def exit_code():
        code = w.DWORD()
        assert get_exit(thread, c.byref(code))
        return code.value

    def stop():
        try:
            outcome['result'] = g.call('music.stop', music=music, timeout_ms=4000)
        except Exception as error:
            outcome['error'] = str(error)

    try:
        play = 'ls3d+70100' if build == '1.0' else 'ls3d+741C0'
        result = g.tool('tap', cmd=play + ' n=1 ptr=1+50:64 max=16')
        tap = int(re.search(r'tap (\d+) on', result).group(1))
        music = g.call('music.play', file='13 - Game Over.ogg', volume=0, loop=True)['music']
        trace = g.tool('tap', cmd=f'show {tap} 16')
        show = int(re.findall(r'#[^\n]*a1=([0-9A-Fa-f]+)', trace)[-1], 16)
        offset = 0x5c if build == '1.0' else 0x60
        tid, native_handle, event = struct.unpack('<III', memory(show + offset, 12))
        assert tid and native_handle and event
        thread = open_thread(0x42, False, tid)
        assert thread and thread_pid(thread) == pid
        lock = g.tool('mem_read', addr='rw_data.dll+EC6C', size=24)['abs']
        # Both configured builds use the same rw_data.dll. Recheck owner after suspension: it may
        # have left the section between the first read and SuspendThread.
        caught = False
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if struct.unpack_from('<I', memory(lock, 24), 12)[0] != tid:
                continue
            previous = suspend(thread)
            assert previous != 0xffffffff
            held = True
            assert previous == 0, 'another owner already suspended the worker'
            if struct.unpack_from('<I', memory(lock, 24), 12)[0] == tid:
                caught = True
                break
            resume(thread)
            held = False
        assert caught, 'decoder did not enter archive lock during the bounded probe'
        closer = threading.Thread(target=stop)
        closer.start()  # this is the only MCP request while close waits; Game's client is not shared
        time.sleep(.25)
        still_waiting = closer.is_alive() and exit_code() == 259
        resume(thread)
        held = False
        closer.join(6)
        assert not closer.is_alive(), 'close failed to return after decoder was resumed'
        g.check('close waits for the archive reader instead of terminating it', still_waiting, outcome)
        g.check('native close returns after the reader unwinds', 'result' in outcome and exit_code() == 0, outcome)
        if 'result' in outcome:
            music = None
        owner = struct.unpack_from('<I', memory(lock, 24), 12)[0]
        g.check('terminated-thread archive owner is absent', owner != tid, {'owner': owner, 'worker': tid})
        # The MCP client inserts timeout_ms into params; this strictly read-only method accepts {} only.
        after = g.call('sound.compat')['ogg_shutdown']
        g.check('game thread responds and cooperative join is counted',
                after['joined'] == before['joined'] + 1 and after['failures'] == before['failures'], after)
    finally:
        if held and thread:
            resume(thread)
        if closer and closer.is_alive():
            closer.join(6)
        # Never issue a competing request on this MCP client while its close request is pending.
        if not closer or not closer.is_alive():
            if music is not None:
                g.call('music.stop', music=music, timeout_ms=3000)
            if tap is not None:
                g.tool('tap', cmd=f'off {tap}')
        if thread:
            close(thread)
        close(process)
