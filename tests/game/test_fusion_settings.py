"""Fusion Fix follows live window sizes; the existing camera settings work through native menu commands."""
import time

from test_window import settled, picture

START = 'city'


def test_resize_and_camera_settings(g):
    mods = g.call('mods.list')
    loaded = {m['id'] for m in mods if m.get('loaded')}
    if not {'fusion_fix', 'devtools'} <= loaded:
        from mml_game import Skip
        raise Skip('load fusion_fix and devtools first (fusion_fix borderless_windowed:false)')
    window = g.call('window.state')
    original = window['render']
    config = g.call('config.get', mod='devtools')
    old = config['changes']
    panel = g.call('panel.state')
    menus = g.call('menu.state')['menus']
    had_page = bool(menus and menus[0]['name']=='mml_mod')
    ff = g.call('fusion_fix.status')
    count = (ff['patches'], ff['hooks'])

    def command(id, value):
        g.call('menu.value', id=id, value=value)
        g.call('menu.command', command=id)
        time.sleep(.15)  # 1.0 dispatches the command at its next native screen tick

    def hud_hidden(wanted):
        # menu.value uses the game's float setter (sliders); a checkbox uses its native click instead.
        if g.call('config.get',mod='devtools')['value']['hide_hud'] != wanted:
            g.call('menu.click',id=305)
            until=time.monotonic()+5
            while time.monotonic()<until and g.call('config.get',mod='devtools')['value']['hide_hud'] != wanted:
                time.sleep(.05)

    try:
        if had_page:
            g.call('menu.command',command=901)
            time.sleep(.15)
        g.call('panel.open', mod='devtools', part='The camera')
        time.sleep(.2)
        page = g.call('menu.state')['menus'][0]
        g.check('camera page contains FOV and HUD controls', {304,305} <= {c['id'] for c in page['controls']}, page['name'])
        command(304, 90/130)
        state = g.call('devtools.state')
        camera = g.call('camera.game')
        g.check('FOV slider reaches the native camera', state['fov']==90 and abs(camera['fov_degrees']-90)<.01, camera)
        hud_hidden(True)
        g.check('HUD checkbox hides native HUD', not g.call('devtools.state')['hud_shown'])
        for w,h in [(1440,810),(960,720),(1280,960)]:
            g.call('window.resize', width=w,height=h)
            settled(g,(w,h))
            until = time.monotonic()+10
            while time.monotonic()<until:
                ff = g.call('fusion_fix.status')
                if ff['screen']==[w,h]:
                    break
                time.sleep(.05)
            g.check('Fusion Fix adapts to %sx%s' % (w,h), ff['applied'] and ff['screen']==[w,h] and
                    abs(ff['base_width']-600*w/h)<.02, ff)
            g.check('owned patches and hooks stay stable', (ff['patches'],ff['hooks'])==count, ff)
            g.check('hidden HUD survives resize', not g.call('devtools.state')['hud_shown'])
            picture(g,w,h,'Fusion Fix %sx%s' % (w,h))
        command(304, 100/130)
        g.check('FOV control still works after resize', abs(g.call('camera.game')['fov_degrees']-100)<.01)
        hud_hidden(False)
        g.check('HUD control shows it again after resize', g.call('devtools.state')['hud_shown'])
    finally:
        for key in ('fov','hide_hud'):
            g.call('config.set', mod='devtools',key=key,value=old.get(key))
        g.call('window.resize',width=original[0],height=original[1])
        settled(g,original)
        # Leave the owner's camera page available; tests in a fresh lab close their temporary page.
        g.call('menu.command',command=901)
        time.sleep(.15)
        if had_page:
            g.call('panel.open',mod=panel['page'],part=panel['part'])
        elif not menus:
            g.call('menu.command',command=111)
