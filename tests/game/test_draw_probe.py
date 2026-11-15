"""Read the CPU vertices actually submitted through the native D3D8 interface, both builds."""
import math
import time


def test_native_skin_draw_capture(g):
    if not any(m['id'] == 'draw_probe' and m['loaded'] for m in g.call('mods.list')):
        g.call('mod.load', id='draw_probe')
    g.later('mod.unload', id='draw_probe')
    here = g.pos('player')
    at = [here[0]+5, here[1], here[2]+4]
    actor = g.spawn_human('Paulie', at)
    g.call('human.stop', actor=actor)
    # game::kActorFrame, both builds; traffic elements use a different layout.
    frame = g.tool('mem_read', addr=hex(int(actor,16)+0x68), size=4, **{'as':'u32'})['value'][0]
    g.call('camera.set', eye=[at[0]-5,at[1]+2,at[2]-7], at=[at[0],at[1]+1,at[2]], ms=15000)
    g.later('camera.release')
    g.call('draw_probe.target', model=hex(frame))
    def sample():
        deadline=time.monotonic()+4
        while time.monotonic()<deadline:
            state=g.call('draw_probe.status')
            valid=[s for s in state['recent'] if s['matched']]
            if valid:
                return state,valid[0]
            g.sleep(80)
        raise RuntimeError('no actual native skin draw matched: '+str(state))
    try:
        g.sleep(250)
        first,before=sample()
        g.check('native draw observer active and fault-free', first['hooked'] and first['faults']==0, first)
        g.check('draw range matched actual CPU-copied vertices', before['vertices']>20 and before['copy_frame']<=before['frame'], before)
        g.check('native draw vertices are in the visible model bounds', math.dist(before['center'],at)<3, before)
        g.call('draw_probe.mark')
        pelvis=next(b for b in g.call('human.skeleton',actor=actor)['bones'] if b['name']=='pelvis')['pos']
        g.call('human.pose',actor=actor,bones={'pelvis':{'pos':[pelvis[0]+2.5,pelvis[1],pelvis[2]]}},ms=10000)
        g.later('human.pose',actor=actor,clear=True)
        g.sleep(250)
        second,after=sample()
        # Main skin and face have distinct visuals. Compare the same visual/range across the transition.
        incident=g.call('draw_probe.incident')
        pairs=[(a,b) for a in incident['before'] for b in incident['after']
               if a['matched'] and b['matched'] and a['visual']==b['visual'] and a['vertices']>50 and
               a['draw_range_vertices']==b['draw_range_vertices'] and b['ms']>100]
        shift=max((math.dist(a['center'],b['center']) for a,b in pairs),default=0)
        g.check('actual copied skin detects a 2.5 m bone displacement', 2.2<shift<2.8, {'shift':shift,'pairs':len(pairs)})
        g.check('incident retains native draws before and after the transition',bool(incident['before']) and bool(incident['after']),
                [len(incident['before']),len(incident['after'])])
        g.sleep(1400)
        saved=g.call('draw_probe.status')
        g.check('completed transition saved as a unique file', saved['written']>0 and saved['last_file'].endswith('.json') and not saved['last_error'],saved)
        g.call('mod.unload',id='draw_probe')
        g.call('mod.load',id='draw_probe')
        g.check('observer detaches and hot reloads cleanly',g.call('draw_probe.status')['faults']==0)
    finally:
        g.call('draw_probe.target')
