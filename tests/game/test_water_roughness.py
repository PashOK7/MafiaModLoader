"""Short waves and reflection softness are independent of retained large waves."""
import time
from test_window import picture

START = 'city'


def test_independent_roughness(g):
    old = g.call('config.get', mod='river_water')['changes']
    size = g.call('window.state')['render']
    touched = ('wind','waves','wave_height','chop','foam','small_ripples','reflection_softness')

    def put(key,value):
        g.call('config.set', mod='river_water',key=key,value=value)

    def ready():
        before = g.call('river_water.status')['drawn']
        until=time.monotonic()+25
        while time.monotonic()<until:
            s=g.call('river_water.status')
            if s['drawn']>before+3 and s['reflection']['plane']>=0:
                return s
            time.sleep(.05)
        raise RuntimeError('water/reflection not ready')

    try:
        for k,v in dict(wind=3,waves=2,wave_height=.5,chop=0,foam=0,small_ripples=1,reflection_softness=1).items():
            put(k,v)
        g.call('camera.set',eye=[-1336.83,-4.94,198.30],dir=[-.755,-.175,-.632],fov=40,ms=120000)
        original=ready()
        put('small_ripples',0)
        smooth=ready()
        g.check('short waves can be suppressed without disabling wind',smooth['waves']['wind']==3)
        g.check('capillary variance reaches zero',smooth['waves']['slope_var_rest']==0,smooth['waves'])
        g.check('long-wave height retained',smooth['waves']['height_dev_m']>=original['waves']['height_dev_m']*.9,
                {'old':original['waves'],'smooth':smooth['waves']})
        g.check('peak wavelength retained',smooth['waves']['peak_m']==original['waves']['peak_m'])
        g.check('short-wave slope energy reduced',0<smooth['waves']['slope_var_drawn']<original['waves']['slope_var_drawn'])
        put('reflection_softness',0)
        sharp=ready()
        g.check('reflection softness does not flatten waves',sharp['waves']==smooth['waves'],sharp['waves'])
        g.check('3D waves still drawn',sharp['geometry']['on'] and sharp['geometry']['triangles']>0,sharp['geometry'])
        picture(g,*size,'sharp wavy water')
        put('small_ripples',1)
        put('reflection_softness',1)
        back=ready()
        g.check('legacy slope energy restored',back['waves']==original['waves'],back['waves'])
        g.call('mod.reload',id='river_water')
        ready()
        g.check('material controls survive reload',g.call('config.get',mod='river_water')['value']['reflection_softness']==1)
    finally:
        for k in touched:
            put(k,old.get(k))
        g.call('camera.release')
