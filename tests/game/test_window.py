"""Resize the actual native window and verify all consumers adapt without restarting the mission."""
import struct
import time
from pathlib import Path

START = 'city'


def test_activation_message_cycles(g):
    """Exercise the native focus-message path repeatedly; no real mouse/keyboard or foreground change."""
    import os
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from run_tests import export_rva
    user32 = os.path.join(os.environ['WINDIR'], 'SysWOW64/user32.dll')
    send = export_rva(user32, 'SendMessageW')
    foreground = export_rva(user32, 'GetForegroundWindow')
    window = g.call('window.state')['cursor_window']
    if not window:
        raise AssertionError('ordinary windowed game required')
    before = g.call('ping')
    active = g.call('native.call', at='user32.dll+%X' % foreground, conv='stdcall', ret='uint')['ret'] == window
    def message(msg, value=0, param=0):
        return g.call('native.call', at='user32.dll+%X' % send, conv='stdcall',
                      args=[window,msg,value,param],ret='int')
    try:
        for i in range(30):
            message(0x1C,0)
            message(6,0)
            message(0x1C,1)
            message(6,1)
            message(0x21,window,0x02010001)  # WM_MOUSEACTIVATE, client/left-button activation, no button sent
        g.sleep(100)
        after=g.call('ping')
        g.check('30 native activation-message cycles preserve process and frame progression',
                before['pid']==after['pid'] and after['frames']>before['frames'],after)
        g.check('native focus messages leave device ready',g.call('window.state')['ready'])
    finally:
        message(0x1C,int(active))
        message(6,int(active))


def test_cursor_confinement(g):
    """Real Win32 confinement and release messages; no mouse/keyboard input or cursor warps.

    Launch this focused-window check with background:false. Background launches must remain unconfined.
    """
    import ctypes
    import os
    import sys
    from ctypes import wintypes
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from run_tests import export_rva
    user = ctypes.WinDLL('user32', use_last_error=True)
    user.GetForegroundWindow.restype = wintypes.HWND
    user.GetClipCursor.argtypes = [ctypes.POINTER(wintypes.RECT)]
    user.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    state = g.call('window.state')
    window = state['cursor_window']
    if not window or user.GetForegroundWindow() != window:
        from mml_game import Skip
        raise Skip('foreground window required; do not activate it or move the real pointer from this test')
    bounds = wintypes.RECT()
    assert user.GetWindowRect(window, ctypes.byref(bounds))
    send = export_rva(os.path.join(os.environ['WINDIR'], 'SysWOW64/user32.dll'), 'SendMessageW')

    def message(msg, value=0):
        g.call('native.call', at='user32.dll+%X' % send, conv='stdcall',
               args=[window, msg, value, 0], ret='int')

    def confined():
        r = wintypes.RECT()
        assert user.GetClipCursor(ctypes.byref(r))
        # Window can be partly outside the desktop. The effective clip must stay within its frame.
        return bounds.left <= r.left < r.right <= bounds.right and bounds.top <= r.top < r.bottom <= bounds.bottom

    g.check('foreground pointer confined to game frame', state['cursor_confined'] and confined(), state)
    try:
        for off, on, value, label in [(0x1C, 0x1C, 1, 'app activation'),
                                     (0x6, 0x6, 1, 'window activation'),
                                     (0x231, 0x232, 0, 'move/size loop'),
                                     (0x211, 0x212, 0, 'system menu loop')]:
            message(off)
            released = g.call('window.state')
            g.check(label + ': released immediately', not released['cursor_confined'] and not confined(), released)
            message(on, value)
            restored = g.call('window.state')
            g.check(label + ': confined again', restored['cursor_confined'] and confined(), restored)
    finally:
        message(0x232)
        message(0x212)
        message(0x1C, 1)
        message(0x6, 1)


def settled(g, wanted=None):
    end = time.monotonic() + 20
    while time.monotonic() < end:
        state = g.call('window.state')
        if not state['pending'] and (wanted is None or state['render'] == list(wanted)):
            return state
        time.sleep(.05)
    raise AssertionError('resize did not settle: %s' % state)


def picture(g, w, h, label):
    shot = g.call('screenshot')
    end = time.monotonic() + 15
    while time.monotonic() < end:
        image = g.call('screenshot.state', id=shot['id'])
        if image.get('done'):
            break
        time.sleep(.05)
    g.check(label + ': actual back-buffer size', image.get('ok') and image.get('width') == w and image.get('height') == h, image)
    raw = Path(image['path']).read_bytes()
    magic, iw, ih, _ = struct.unpack_from('<4I', raw)
    assert magic == 0x534C4D4D and (iw, ih) == (w, h) and len(raw) == 16+w*h*4
    # A successful Reset and busy DLSS counters previously accepted completely black frames. Check the pixels
    # in nine interior tiles as well: a stale smaller viewport leaves whole tiles undrawn after enlargement.
    coverage = []
    colors = set()
    for ty in range(3):
        for tx in range(3):
            lit = total = 0
            for y in range((ty*3+1)*h//10, (ty*3+3)*h//10, max(1,h//100)):
                for x in range((tx*3+1)*w//10, (tx*3+3)*w//10, max(1,w//100)):
                    bgr = raw[16+(y*w+x)*4:19+(y*w+x)*4]
                    lit += max(bgr) > 12
                    total += 1
                    colors.add(bgr)
            coverage.append(round(lit/total,3))
    g.check(label + ': image drawn throughout frame', min(coverage) > .05 and len(colors) > 128,
            {'lit_tiles':coverage,'colors':len(colors),'path':image['path']})
    render = g.call('render.state')
    g.check(label + ': scene fits buffer and camera aspect', render.get('scene_viewport') == [0,0,w,h] and
            abs(render.get('scene_aspect',0)-w/h) < .001, render.get('scene_viewport'))


def test_resize_cursors(g):
    if not any(m['id']=='kit' and m.get('loaded') for m in g.call('mods.list')):
        from mml_game import Skip
        raise Skip('load the kit cursor observer')
    state = g.call('kit.resize_cursor')
    for edge in state['edges']:
        g.check('border %s hit/shape/visibility' % edge['expected_hit'],
                edge['hit']==edge['expected_hit'] and edge['shape'] and edge['handled'] and edge['visible'],edge)
    g.check('game cursor visibility restored on nonclient leave',state['visibility_restored'],state)
    g.check('game cursor visibility restored on client entry',state['client_restored'],state)
    g.check('cursor check did not move input or change focus',state['input_untouched'],state)


def test_client_system_cursor(g):
    if not any(m['id'] == 'kit' and m.get('loaded') for m in g.call('mods.list')):
        from mml_game import Skip
        raise Skip('load the kit cursor observer')
    state = g.call('kit.client_cursor')
    if state['foreground']:
        g.check('visible Windows cursor hidden on every client request', state['hidden'] == state['cycles'], state)
    else:
        g.check('background client does not hide the Windows cursor', state['hidden'] == 0, state)
    g.check('client cursor policy leaves the display counter unchanged', state['count_balanced'], state)
    g.check('cursor fixture returns its visibility loan exactly', state['count_restored'], state)
    g.check('client cursor check leaves pointer position and foreground unchanged', state['input_untouched'], state)


def test_live_resize(g):
    initial = g.call('window.state')
    if not initial['resizable']:
        raise AssertionError('launch this test in an ordinary window (fullscreen:false)')
    original = initial['render']
    ping = g.call('ping')
    seq = g.latest()
    mods = g.call('mods.list')
    if isinstance(mods, dict):
        mods = mods['mods']
    loaded = {m['id'] for m in mods if m.get('loaded')}
    dlss_on = 'dlss_nr' in loaded and g.call('dlss_nr.state')['on']
    page_open = False
    try:
        for w, h in [(960, 720), (1280, 720), (800, 900), (1024, 768), (960, 720)]:
            g.call('window.resize', width=w, height=h)
            s = settled(g, (w, h))
            g.check('client/native/render agree at %sx%s' % (w, h), s['client'] == s['native'] == [w, h] and not s['last_error'], s)
            picture(g,w,h,'%sx%s' % (w,h))
            gfx = g.call('gfx.state')
            if gfx['passes'] or gfx['upscale']['mod']:
                g.check('graphics consumers recover at %sx%s' % (w, h), gfx['state'] == 'on' and gfx['size'] == [w, h], gfx['state'])
            if dlss_on:
                end = time.monotonic() + 20
                before = g.call('dlss_nr.state')['drawn']
                while time.monotonic() < end:
                    dlss = g.call('dlss_nr.state')
                    if dlss['frame'] == [w,h] and dlss['drawn'] > before and not dlss['last_error']:
                        break
                    time.sleep(.1)
                g.check('DLSS continues at %sx%s' % (w,h), dlss['frame'] == [w,h] and dlss['drawn'] > before and not dlss['last_error'], dlss)
        count = g.call('window.state')['resets']
        g.call('window.resize', width=960, height=720)
        time.sleep(.2)
        g.check('same size does not reset again', g.call('window.state')['resets'] == count)
        try:
            g.call('window.resize', width=0, height=720)
            g.check('zero size refused', False)
        except Exception as error:
            if 'window.resize' not in str(error):
                raise
            g.check('zero size refused', True)
        g.call('window.resize', state='minimize')
        time.sleep(.2)
        minimized = g.call('window.state')
        g.check('minimize retains device size and reset count', minimized['minimized'] and minimized['render'] == [960,720] and minimized['resets'] == count, minimized)
        g.call('window.resize', state='restore')
        s = settled(g)
        g.check('restore resumes the same device', not s['minimized'] and not s['last_error'], s)
        g.call('window.resize', state='maximize')
        time.sleep(.2)
        s = settled(g)
        g.check('maximize adapts native rendering', s['maximized'] and s['client'] == s['native'] == s['render'], s)
        picture(g,*s['render'],'maximized')
        g.call('window.resize', width=960, height=720)
        settled(g, (960,720))
        g.call('menu.page', name='resize_test', controls=[
            {'id':100,'kind':'wind','x':100,'y':100,'w':600,'h':400,'text':'Resize'},
            {'id':101,'kind':'butt','x':40,'y':60,'w':240,'h':30,'text':'Layout check'},
            {'id':102,'kind':'chck','x':40,'y':110,'w':240,'h':30,'text':'Keep value','value':1}])
        page_open = True
        time.sleep(.2)
        before = g.call('menu.state')
        g.call('window.resize', width=1280, height=960)
        settled(g, (1280,960))
        after = g.call('menu.state')
        g.check('open native page survives resize', 'resize_test' in str(after), after)
        page = next((m for m in after['menus'] if 'resize_test' in m['name']), None)
        g.check('native page retains checkbox value', page is not None and any(c['id']==102 and c.get('value')==1 for c in page['controls']), page)
        g.call('menu.click', id=101)
        time.sleep(.1)
        g.check('resized native page still accepts clicks', 'resize_test' in str(g.call('menu.state')))
        g.call('menu.page.close')
        page_open = False
        g.call('menu.command',command=111)
        time.sleep(.2)
        picture(g,1280,960,'page closed after resize')
        events = g.call('events.since', seq=seq, types='DEVICE_RESET_BEGIN,DEVICE_RESET_END')['events']
        ends = [e for e in events if e['type']=='DEVICE_RESET_END']
        g.check('reset events include successful completion', len(ends)>=5 and all(e['data']['success'] for e in ends), ends)
        now = g.call('ping')
        g.check('same process continues after all sizes', now['pid'] == ping['pid'] and now['frames'] > ping['frames'])
        # Optional test-only mod: force driver failures without allocating huge textures or changing game code.
        if any(m['id']=='kit' for m in mods):
            if 'kit' not in loaded:
                g.call('mod.load', id='kit')
            try:
                for twice in (False,True):
                    g.call('kit.device_reset_failure', twice=twice)
                    g.call('window.resize', width=1056,height=768)
                    if twice:
                        end=time.monotonic()+5
                        seen=False
                        while time.monotonic()<end:
                            failed=g.call('window.state')
                            if not failed['ready']:
                                seen=True
                                break
                            time.sleep(.025)
                        g.check('failed reset and rollback suspend scene drawing',seen,failed)
                    s=settled(g,(1280,960))
                    g.check('native reset recovers after %s forced failures' % (2 if twice else 1),s['ready'] and s['render']==s['native']==s['client'],s)
                    picture(g,1280,960,'after %s forced failures' % (2 if twice else 1))
                g.check('forced failures leave the process alive',g.call('ping')['pid']==ping['pid'])
            finally:
                g.call('mod.unload',id='kit')
    finally:
        if page_open:
            g.call('menu.page.close')
        g.call('window.resize', width=original[0], height=original[1])
        settled(g, original)
        picture(g,*original,'final restored size')
