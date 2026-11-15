"""An asleep corpse pushed by walking must not turn the walker's own motion into an incoming blow."""
def test_walk_over_resting_body(g):
    # Inherited water-lab freecam captures Forwards for camera motion, not the walking player.
    if any(m['id']=='devtools' and m['loaded'] for m in g.call('mods.list')):
        g.call('mod.unload',id='devtools')
        g.later('mod.load',id='devtools')
    g.call('mod.load',id='ragdoll')
    g.later('mod.unload',id='ragdoll')
    assert g.on_foot(),'player must leave the car'
    player=g.call('human.skeleton',actor='player')['actor']
    # The start parking area has an invisible native surface-33 box at z=34.387. Use the open grass at z=75.
    ground=g.call('world.ground',at=[-1981,0,75])['height']
    actor=g.spawn_human('civil15',[-1981,ground+.02,75])
    g.call('human.anim',actor=actor,play='smrt pad end A')
    g.sleep(250)
    g.call('ragdoll.start',actor=actor)
    g.call('human.hit',actor=actor,type=0,dir=[0,0,1],damage=1000)
    g.sleep(3500)
    state=g.call('ragdoll.state')
    corpse=next(b for b in state['ragdolls'] if b['actor']==actor)
    g.check('fixture body starts at rest',corpse['sleeping'] and corpse['fastest_part']<.05,corpse)
    knocked=False
    crossed=0
    displacement=0
    for start, direction in ((71,1),(79,-1),(71,1),(79,-1)):
        height=g.call('world.ground',at=[-1981,0,start])['height']
        g.tool('teleport',to=[-1981,height+.02,start],dir=[0,0,direction])
        for step in range(20):
            g.tool('input',steps=[{'action':'Forwards','ms':400}])
            bodies=g.call('ragdoll.state')['ragdolls']
            knocked |= any(b['actor']==player for b in bodies)
            moved=next(b for b in bodies if b['actor']==actor)
            for part in ('pelvis','chest','head'):
                displacement=max(displacement,sum((moved[part][i]-corpse[part][i])**2 for i in range(3))**.5)
            if (g.pos('player')[2]-75)*direction>1:break
        end=g.pos('player')
        g.log('traversal %s -> %s' % (start,end))
        crossed += (end[2]-75)*direction > .8
    g.check('player actually crosses the body in both directions',crossed==4,{'crossings':crossed})
    # A 400 ms snapshot can miss a brief limb velocity peak, especially at the native frame cap.
    # Measure lasting physical displacement from the asleep pose, rather than require a hard kick (>0.25 m/s).
    g.check('walking actually pushes the physical corpse',displacement>.01,{'max_displacement_m':displacement})
    g.check('walking across a resting corpse never knocks the player over',not knocked,g.call('ragdoll.knock'))
    push=g.call('ragdoll.state')['person_push']
    g.log('finite human effort: %s' % push)
    g.check('all impacts and overlap pushes share the person force budget',
            push['contacts']>0 and push['delivered_ns']>0 and
            push['peak_force_n']<=push['force_limit_n']+.01,push)
    g.check('contact proxies use actual segment masses',0<push['max_segment_kg']<=11.5,push)
