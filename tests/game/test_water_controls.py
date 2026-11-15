"""Wave length/phase and material controls reach actual native menus and rendered pixels, in both builds."""
import struct
import time
from pathlib import Path

START = 'city'


def test_wave_and_material_controls(g):
    old = g.call('config.get', mod='river_water')['changes']
    dev = g.call('config.get', mod='devtools')['changes']
    old_scale = g.call('state.time')['scale']
    has_dlss = any(m['id']=='dlss_nr' and m.get('loaded') for m in g.call('mods.list'))
    dlss = g.call('config.get', mod='dlss_nr')['changes'] if has_dlss else {}
    touched = ('wind','waves','wave_height','wave_width','wave_speed','chop','foam','glitter','debug',
               'reflection_strength','colour','tint','scattering','small_ripples')

    def put(key, value):
        g.call('config.set', mod='river_water', key=key, value=value)

    def ready():
        before = g.call('river_water.status')['drawn']
        until = time.monotonic()+25
        while time.monotonic()<until:
            s = g.call('river_water.status')
            if s['drawn']>before+4 and s['reflection']['plane']>=0:
                return s
            time.sleep(.05)
        raise RuntimeError('water not drawn')

    def pixels():
        shot = g.call('screenshot')
        until = time.monotonic()+15
        while time.monotonic()<until:
            result = g.call('screenshot.state', id=shot['id'])
            if result.get('done'):
                break
            time.sleep(.05)
        assert result.get('ok'), result
        raw = Path(result['path']).read_bytes()
        magic,w,h,_ = struct.unpack_from('<4I',raw)
        assert magic==0x534c4d4d
        # The held embankment view's lower central river, away from the menu and opposite bank.
        return [tuple(raw[16+4*(y*w+x):19+4*(y*w+x)])
                for y in range(h*6//10,h*9//10,4) for x in range(w*4//10,w*8//10,4)]

    def command(id, value):
        g.call('menu.value',id=id,value=value)
        g.call('menu.command',command=id)
        time.sleep(.2)  # 1.0 dispatches on its native screen tick

    try:
        g.call('state.time',scale=0)
        # DLAA jitters/reconstructs even a static surface; measure native normals before temporal processing.
        if has_dlss:
            g.call('config.set',mod='dlss_nr',key='on',value=False)
        g.call('config.set',mod='devtools',key='freecam',value=False)
        for k,v in dict(wind=3,waves=2,wave_height=1,wave_width=1,wave_speed=0,chop=0,foam=0,
                        glitter=0,debug=2,small_ripples=.35).items():
            put(k,v)
        g.call('camera.set',eye=[-1336.83,-4.94,198.30],dir=[-.755,-.175,-.632],fov=40,ms=600000)
        base = ready()
        a = pixels()
        frozen = ready()
        b = pixels()
        g.check('zero speed freezes integrated phase',base['wave_controls']['time']==frozen['wave_controls']['time'])
        g.check('zero speed freezes actual rendered river normals',a==b, sum(x!=y for x,y in zip(a,b)))
        put('wave_width',4)
        wide = ready()
        c = pixels()
        g.check('width stretches wavelength fourfold',abs(wide['waves']['peak_m']-4*base['waves']['peak_m'])<.03,wide['waves'])
        g.check('width retains gravity-wave height within band transition',.8*base['waves']['height_dev_m']<wide['waves']['height_dev_m']<1.3*base['waves']['height_dev_m'])
        g.check('width retains fine ripple energy instead of flattening every scale',
                base['waves']['slope_var_drawn']/16<wide['waves']['slope_var_drawn']<base['waves']['slope_var_drawn'] and
                wide['waves']['slope_var_rest']==base['waves']['slope_var_rest'])
        g.check('width reaches actual surface sampling',sum(x!=y for x,y in zip(b,c))>len(c)*.1)
        put('wave_speed',2)
        moving = ready()
        d = pixels()
        g.check('speed advances phase and rendered normals',moving['wave_controls']['time']>wide['wave_controls']['time'] and
                sum(x!=y for x,y in zip(c,d))>len(c)*.1)
        put('wave_speed',0)
        put('debug',0)
        put('reflection_strength',0)
        put('scattering',1.5)
        put('colour','#bb100f')
        ready()
        red = pixels()
        put('colour','#0f10bb')
        ready()
        blue = pixels()
        rb = lambda ps: sum(p[2]-p[0] for p in ps)/len(ps)
        g.check('body color changes rendered water red to blue',rb(red)>rb(blue)+15,{'red':rb(red),'blue':rb(blue)})
        put('reflection_strength',1)
        ready()
        reflected = pixels()
        g.check('reflection strength changes actual composite',sum(x!=y for x,y in zip(blue,reflected))>len(blue)*.2)
        put('colour','#123456')
        put('tint','#abcdef')
        state = g.call('panel.state')
        water = next(m for m in state['mods'] if m['mod']=='river_water')
        g.check('two compact wave/material tabs exposed', [p['label'] for p in water['parts']]==['Waves','Material'] and len(water['controls'])==12)
        rgb = [c for c in water['controls'] if c['key']=='colour']
        g.check('one hex setting exposed as three numeric channels', [c['value'] for c in rgb]==[18,52,86] and
                [c['color_channel'] for c in rgb]==[0,1,2],rgb)
        g.call('panel.open',mod='river_water',part='Material')
        time.sleep(.3)
        material = next(s for s in water['sections'] if s['title']=='Material')
        first = material['keys'].index('colour')
        command(300+first, 200/255)
        g.check('native red slider preserves green and blue',g.call('config.get',mod='river_water')['value']['colour']=='#c83456')
        command(301+first, 100/255)
        command(302+first, 25/255)
        g.check('native green/blue sliders preserve other channels',g.call('config.get',mod='river_water')['value']['colour']=='#c86419')
        g.check('native water color edits preserve advanced absorption settings',g.call('config.get',mod='river_water')['value']['tint']=='#abcdef')
        g.call('menu.command',command=901)
        time.sleep(.2)
        g.call('menu.command',command=111)
        g.call('mod.reload',id='river_water')
        s = ready()
        cfg = g.call('config.get',mod='river_water')['value']
        g.check('wave and color controls survive hot reload',cfg['colour']=='#c86419' and cfg['wave_width']==4 and s['wave_controls']['speed']==0)
    finally:
        g.call('state.time',scale=old_scale)
        for k in touched:
            put(k,old.get(k))
        g.call('camera.release')
        g.call('config.set',mod='devtools',key='freecam',value=dev.get('freecam'))
        if has_dlss:
            g.call('config.set',mod='dlss_nr',key='on',value=dlss.get('on'))
