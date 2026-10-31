"""Public SDK atmosphere ownership and policy-only mod, shared by city/water and both upscaler paths."""
START = 'city'


def test_atmosphere(g):
    mods = g.call('mods.list')
    if isinstance(mods, dict):
        mods = mods['mods']
    wasLoaded = any(m['id'] == 'atmosphere' and m.get('loaded') for m in mods)
    if not wasLoaded:
        g.call('mod.load', id='atmosphere')
    old = g.call('config.get', mod='atmosphere')['changes']
    baseline = g.call('render.state')['fog']
    bridge = next((r['settings'] for r in g.call('atmosphere.state')['requests'] if r['owner']=='bridge'), None)
    keys = ['on','start','distance','density','height','base','sun','tint']
    dlss = any(m['id']=='dlss_nr' and m.get('loaded') for m in mods)
    oldDlss = g.call('config.get', mod='dlss_nr')['changes'].get('on') if dlss else None

    def put(key, value):
        g.call('config.set', mod='atmosphere', key=key, value=value)

    def frame():
        g.sleep(250)
        g.tool('screenshot', max_width=160)
        return g.call('gfx.state')

    try:
        g.call('atmosphere.set', on=False)
        g.call('render.set', fog_mode=0)
        g.call('camera.set', eye=[-1490,30,20], dir=[-100,-40,-45], ms=120000)
        for key,value in dict(start=0,distance=700,density=0.7,height=70,base=-10,sun=0.6,tint=0,on=True).items():
            put(key,value)
        gfx=frame()
        g.check('policy mod owns the ready SDK service', g.call('atmosphere.state')['owner']=='atmosphere' and g.call('atmosphere.status')['ready'])
        g.check('SDK atmosphere replaces native fog', gfx['world']['atmosphere']['on'] and not gfx['world']['fog']['on'], gfx['world'])
        a=gfx['world']['atmosphere']
        g.check('explicit extinction is shared with water/reflection shader', abs(a['density']-0.003)<0.00001, a)
        g.check('height parameters survive SDK request', a['base']==-10 and abs(a['falloff']-1/70)<0.00001, a)
        for height in [2,200]:
            g.call('camera.set',eye=[-1490,height,20],dir=[-100,-10,-45],ms=120000)
            moving=frame()
            g.check('SDK atmosphere follows camera height %s'%height, abs(moving['world']['eye'][1]-height)<0.01 and moving['world']['atmosphere']['on'])
        g.call('atmosphere.set',priority=100,extinction=0.001,color=[0.2,0.3,0.4])
        override=frame()
        g.check('higher priority bridge request overrides the mod', g.call('atmosphere.state')['owner']=='bridge' and abs(override['world']['atmosphere']['density']-0.001)<0.00001)
        put('density',0.5)
        frame()
        g.check('policy updates do not steal another owner', g.call('atmosphere.state')['owner']=='bridge')
        g.call('atmosphere.set',on=False)
        frame()
        g.check('release restores policy request automatically', g.call('atmosphere.state')['owner']=='atmosphere')
        g.call('mod.reload',id='atmosphere')
        frame()
        g.check('hot reload preserves one live policy request', g.call('atmosphere.state')['request_count']==1)
        if dlss:
            for on in [False,True]:
                g.call('config.set',mod='dlss_nr',key='on',value=on)
                current=frame()
                g.check('SDK atmosphere survives DLSS %s'%on,current['world']['atmosphere']['on'] and current['state']=='on')
                if on:
                    s=g.call('dlss_nr.state')
                    g.check('DLSS receives fogged frames',s['drawn']>0 and not s['last_error'],s)
        else:
            g.check('SDK atmosphere works without upscaler',not gfx['upscale']['mod'] and gfx['state']=='on')
        put('density',0)
        g.check('zero-density request clears atmosphere without native double fog',frame()['world']['atmosphere']['density']==0)
        g.call('mod.unload',id='atmosphere')
        native=frame()
        g.check('unloading policy restores native fog',not native['world']['atmosphere']['on'] and native['world']['fog']['on'],native['world'])
        g.check('unload removes SDK request',g.call('atmosphere.state')['request_count']==0)
        g.call('mod.load',id='atmosphere')
    finally:
        loaded=g.call('mods.list')
        if isinstance(loaded,dict):
            loaded=loaded['mods']
        if not any(m['id']=='atmosphere' and m.get('loaded') for m in loaded):
            g.call('mod.load',id='atmosphere')
        for key in keys:
            put(key,old.get(key))
        if dlss:
            g.call('config.set',mod='dlss_nr',key='on',value=oldDlss)
        g.call('render.set',fog_mode=baseline['mode'],fog_start=baseline['start'],fog_end=baseline['end'])
        g.call('atmosphere.set',on=False)
        if bridge:
            g.call('atmosphere.set',**bridge)
        if not wasLoaded:
            g.call('mod.unload',id='atmosphere')
        frame()
        g.call('camera.release')
