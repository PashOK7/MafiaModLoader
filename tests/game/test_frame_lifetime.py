"""Agent-owned native frames: Release and a native deleting destructor notify exactly once.

Do not infer destruction from world.remove: model/cache references can outlive its actor.
The empty model here belongs only to this fixture, so either native destruction path is valid.
"""
import struct
import time


def test_native_frame_destruction(g):
    # ExeBuild::i3dDriver / drvCreateFrame, verified by check_builds.py in both builds.
    driver_slot, create_slot = {'1.2': (6586072, 0x50), '1.0': (7312672, 0x4c)}[g.build]

    def pointer(at):
        return struct.unpack('<I', bytes.fromhex(g.call('mem.read', addr=hex(at), size=4)['hex']))[0]

    driver = pointer(driver_slot)
    create = pointer(pointer(driver) + create_slot)
    for direct in (False, True):
        frame = g.call('native.call', at=hex(create), conv='stdcall', args=[driver, 9], ret='ptr')['ret']
        addr = int(frame, 16)
        assert addr, 'native driver did not create the fixture model'
        vt = pointer(addr)
        target = pointer(vt + (0x40 if direct else 0))
        seq = g.latest()
        g.call('frame.watch', frame=frame)
        result = g.call('native.call', at=hex(target), conv='thiscall' if direct else 'stdcall',
                        args=[1] if direct else [addr], ret='void',
                        **({'this': frame} if direct else {}))
        assert result['stack']['ok'], result
        events = g.call('events.since', seq=seq, types='FRAME_RELEASED', limit=1000)['events']
        released = [e for e in events if int(e['data']['frame'], 16) == addr]
        path = 'native deleting destructor' if direct else 'virtual Release'
        g.check(path + ' notifies exactly once', len(released) == 1, released)
        watched = g.call('frames.watched')['frames']
        g.check(path + ' leaves no stale watch',
                not any(int(w['frame'], 16) == addr for w in watched), watched)


def test_scene_frame_destruction(g):
    driver_slot, create_slot = {'1.2': (6586072, 0x50), '1.0': (7312672, 0x4c)}[g.build]

    def pointer(at):
        return struct.unpack('<I', bytes.fromhex(g.call('mem.read', addr=hex(at), size=4)['hex']))[0]

    root = int(g.player()['frame'], 16)
    while pointer(root + 0x120):
        root = pointer(root + 0x120)
    driver = pointer(driver_slot)
    create = pointer(pointer(driver) + create_slot)
    g.call('scene.focus', at='player', radius=30)
    g.later('scene.focus', off=True)
    for cycle in range(3):
        frame = g.call('native.call', at=hex(create), conv='stdcall', args=[driver, 9], ret='ptr')['ret']
        addr = int(frame, 16)
        assert addr, 'native driver did not create the fixture model'
        vt = pointer(addr)
        destroy = pointer(vt + 0x40)
        alive = True
        try:
            linked = g.call('native.call', at=hex(pointer(vt + 0x2c)), conv='stdcall',
                            args=[addr, root, 0], ret='int')
            assert linked['stack']['ok'], linked
            deadline = time.monotonic() + 15
            tracked = False
            while time.monotonic() < deadline and not tracked:
                watches = g.call('frames.watched')['frames']
                tracked = any(int(w['frame'], 16) == addr and 'mml' in w['by'] for w in watches)
                if not tracked:
                    time.sleep(.05)
            g.check('scene walk tracks the linked frame lifetime', tracked, {'cycle': cycle})
            before = g.call('scene.pieces', limit=0)['scans']
            alive = False
            released = g.call('native.call', at=hex(destroy), conv='thiscall',
                              **{'this': frame}, args=[1], ret='void')
            assert released['stack']['ok'], released
            watches = g.call('frames.watched')['frames']
            g.check('destroyed scene frame leaves no stale watch',
                    not any(int(w['frame'], 16) == addr for w in watches), {'cycle': cycle})
            deadline = time.monotonic() + 15
            state = g.call('scene.pieces', limit=0)
            while time.monotonic() < deadline and state['scans'] <= before:
                time.sleep(.05)
                state = g.call('scene.pieces', limit=0)
            g.check('scene scan progresses after native frame destruction',
                    state['scans'] > before and state['pieces'] > 0, state)
        finally:
            if alive:
                g.call('native.call', at=hex(destroy), conv='thiscall',
                       **{'this': frame}, args=[1], ret='void')
