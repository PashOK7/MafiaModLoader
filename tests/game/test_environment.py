"""World sky capture: native directional invariance, foreground exclusion and lifecycle, both builds."""
import time
from mml_game import Skip
from test_window import settled

START = 'city'

def test_native_sky_environment(g):
    loaded = {m['id'] for m in g.call('mods.list') if m.get('loaded')}
    if not {'kit', 'kit_rs', 'devtools'} <= loaded:
        raise Skip('load the C/C++ kit environment probe, Rust kit and devtools')
    old = g.call('config.get', mod='devtools')['changes']
    old_time = g.call('state.time')['scale']
    old_size = g.call('window.state')['render']
    records = []

    def wait_capture(after=0):
        deadline = time.monotonic()+30
        while time.monotonic()<deadline:
            s = g.call('kit.environment')
            if s.get('sample') and s['sample']['captures']>after:
                return s
            time.sleep(.05)
        raise RuntimeError('no coherent native cube: '+str(s))

    try:
        g.call('state.time', scale=0)
        g.call('config.set', mod='devtools', key='freecam', value=False)
        g.call('kit.environment', off=True)
        g.call('kit.environment', on=True)
        baseline = wait_capture()
        records.append(baseline)
        g.check('C/C++ and safe Rust report the same supported view flags',
                baseline['features_c']==baseline['features_cpp']==g.call('kit_rs.view_features')==7)
        g.check('native sky contains six nonblack color faces', len(baseline['sample']['faces'])==6 and
                all(max(p)>50 for p in baseline['sample']['faces']), baseline['sample'])
        g.check('sky excludes foreground geometry in native depth',baseline['sample']['min_depth']==100000)
        for label, eye, direction, fov in [
            ('pitch down',[-1420,2,198],[0,-.8, .6],40),
            ('yaw away',[-1420,2,198],[-1,-.1,0],100),
            ('world origin',[0,0,0],[0,0,1],50),
            ('high camera',[0,100,0],[1,-.1,0],70),
        ]:
            g.call('camera.set',eye=eye,dir=direction,fov=fov,ms=600000)
            # Advance to a rendered camera before refreshing all six explicitly world-oriented views.
            start=g.call('kit.projection')['main']['frames']
            deadline=time.monotonic()+15
            while g.call('kit.projection')['main']['frames']<=start+2:
                if time.monotonic()>deadline: raise RuntimeError('camera not rendered')
                time.sleep(.05)
            before=records[-1]['sample']['captures']
            g.call('kit.environment',refresh=True)
            sample=wait_capture(before)
            records.append(sample)
            g.check(label+': complete sky RGB is independent of observer pose/frustum',
                    sample['sample']['hash']==baseline['sample']['hash'],sample['sample'])
            g.check(label+': no foreground or auxiliary effects/callbacks',
                    sample['sample']['min_depth']==100000 and sample['callbacks']==sample['effect_calls']==0,sample)
        g.call('window.resize',width=old_size[0]+64,height=old_size[1]+32)
        settled(g,[old_size[0]+64,old_size[1]+32])
        sample=wait_capture(records[-1]['sample']['captures'])
        g.check('native reset recreates a coherent identical environment',sample['sample']['hash']==baseline['sample']['hash'],sample)
        g.call('mod.reload',id='kit')
        g.check('probe reload releases its owned environment views',
                not any(v['mod']=='kit' for v in g.call('view.state')['views']))
        g.call('kit.environment',on=True)
        sample=wait_capture()
        g.check('probe reload can reacquire six valid native faces',sample['sample']['hash']==baseline['sample']['hash'],sample)
        g.check('all auxiliary views restored native state without faults',
                all(v['faults']==0 for v in g.call('view.state')['views']))
    finally:
        g.call('kit.environment',off=True)
        g.call('window.resize',width=old_size[0],height=old_size[1])
        settled(g,old_size)
        g.call('camera.release')
        g.call('config.set',mod='devtools',key='freecam',value=old.get('freecam'))
        g.call('state.time',scale=old_time)
