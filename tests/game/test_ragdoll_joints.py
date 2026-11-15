"""A curved, already lying start pose must not grant a spine joint almost 180 degrees of twist."""
import json
import re

def test_lying_spine_limits(g):
    g.call('mod.load',id='ragdoll')
    g.later('mod.unload',id='ragdoll')
    for index,variant in enumerate(('A','B')):
        actor=g.spawn_human('Paulie',[-1981,-4.7,25+index*6])
        # A render-only human.pose is not sufficient: the mod snapshots the native pose at mission tick.
        g.call('human.anim',actor=actor,play='smrt pad end '+variant)
        g.sleep(250)
        g.call('ragdoll.start',actor=actor)
        g.sleep(1200)
        report=g.call('ragdoll.inspect',actor=actor)
        pts=json.loads(report.splitlines()[0])['init']
        g.check('ragdoll really captured an already lying native pose',
                abs(pts[4][1]-pts[0][1])<.3 and
                sum((pts[4][i]-pts[0][i])**2 for i in (0,2))>.16,pts[:5])
        joint=next(line for line in report.splitlines() if 'chestlow' in line and 'twist' in line)
        g.log('native lying variant %s: %s' % (variant, joint.strip()))
        match=re.search(r'twist\s+[-\d.]+\s+\(([-\d.]+)\.\.([-\d.]+)\)',joint)
        g.check('lying curved spine retains a narrow twist range',match and
                float(match[1])>=-30 and float(match[2])<=30,joint)
        g.check('lying spine settles inside its joint limits','OVER' not in joint,joint)


def test_model_rest_anatomy(g):
    """Check skin anatomy as well as physics ranges; a narrow range in a wrong frame is insufficient."""
    import math
    g.call('mod.load',id='ragdoll')
    g.later('mod.unload',id='ragdoll')
    def dot(a,b):
        return sum(x*y for x,y in zip(a,b))/math.sqrt(sum(x*x for x in a)*sum(x*x for x in b))
    for index,model in enumerate(('Paulie','civil15')):
        for variant in ('A','B'):
            actor=g.spawn_human(model,[-1981-index*3,-4.7,25+(variant=='B')*6])
            g.call('human.anim',actor=actor,play='smrt pad end '+variant)
            g.sleep(250)
            g.call('human.hit',actor=actor,type=0,dir=[0,0,0],damage=1000)
            bodies=g.call('ragdoll.state')['ragdolls']
            g.check(model+variant+': native death is adopted without a puppet command',
                    any(b['actor']==actor and not b['puppet'] for b in bodies),bodies)
            g.sleep(2200)
            report=g.call('ragdoll.inspect',actor=actor)
            joints=report.split('  joints:')[1]
            head=next(line for line in joints.splitlines() if 'head ' in line)
            g.check(model+variant+': neck limits remain anatomical', '(-70..70)' in head and 'OVER' not in head,head)
            for side in ('r','l'):
                elbow=next(line for line in joints.splitlines() if side+'_forearm' in line)
                g.check(model+variant+': '+side+' elbow has fixed bend limits','(0..145)' in elbow and 'OVER' not in elbow,elbow)
            bones={b['name']:b for b in g.call('human.skeleton',actor=actor)['bones']}
            neck_front=dot(bones['back3']['z_axis'],bones['neck']['z_axis'])
            g.check(model+variant+': head never faces through the back of the torso',neck_front>0,neck_front)
            for side in ('r','l'):
                alignment=dot(bones[side+'_arm']['y_axis'],bones[side+'_elbow']['y_axis'])
                g.check(model+variant+': '+side+' elbow bending axis is aligned, not flipped',alignment>.85,alignment)
