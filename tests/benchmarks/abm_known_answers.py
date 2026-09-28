"""Exact Life transitions and frozen finite-time random-walk analytic gates."""
import argparse
import json
import math
from pathlib import Path
import random
import subprocess

p=argparse.ArgumentParser(description=__doc__);p.add_argument('executable',type=Path);p.add_argument('destination',type=Path);a=p.parse_args()
a.destination.mkdir(parents=True,exist_ok=False);cases=[]
for name,points in [('block',[(2,2),(2,3),(3,2),(3,3)]),('blinker',[(2,3),(3,3),(4,3)]),('glider',[(2,1),(3,2),(1,3),(2,3),(3,3)])]:
    cases.append(dict(id=name,width=8,height=8,wrap=False,steps=8,initial=[int((x,y) in points) for y in range(8) for x in range(8)]))
rng=random.Random(20260928)
for wrap in (False,True):
    for width,height in [(3,3),(5,7),(8,8),(12,9)]:
        for rep in range(8):
            cases.append(dict(id=f'random-{wrap}-{width}-{height}-{rep}',width=width,height=height,wrap=wrap,steps=12,
                              initial=[rng.randrange(2) for _ in range(width*height)]))
plan=dict(life=cases,walkers=4096,walk_steps=128,replications=4)
# Freeze gates before invoking native code. DKW controls all 16 CDF comparisons.
alpha=.001;cdf_gate=math.sqrt(math.log(2*16/alpha)/(2*4096))
plan['acceptance']=dict(familywise_cdf_alpha=alpha,cdf_gate=cdf_gate,mean_se_multiplier=6,msd_se_multiplier=6,
    note='CDF gate is DKW+union bound for independent walkers; six-SE moment checks are additional normal-approximation diagnostics/gates')
path=a.destination/'plan.json';path.write_text(json.dumps(plan,indent=2)+'\n')
r=subprocess.run([str(a.executable.resolve()),str(path)],check=True,capture_output=True,text=True)
(a.destination/'native.json').write_text(r.stdout);native=json.loads(r.stdout);compared=0;life=[]
assert len(native['life'])==len(cases), 'missing Life cases'
assert len(native['walk'])==plan['replications']*4, 'missing walk snapshots'
assert {(r['replication'],r['time']) for r in native['walk']}=={(r,t) for r in range(plan['replications']) for t in (1,8,32,128)}
for spec,run in zip(cases,native['life']):
    assert run['id']==spec['id'];w,h=spec['width'],spec['height'];state=spec['initial'][:]
    assert len(run['states'])==spec['steps']+1, 'truncated Life trajectory'
    for t,actual in enumerate(run['states']):
        assert actual==state,(spec['id'],t);compared+=len(state)
        # Direct coordinate stencil; no Fathom grid or scheduling implementation.
        next_state=[]
        for y in range(h):
            for x in range(w):
                positions=set()
                for dx in (-1,0,1):
                    for dy in (-1,0,1):
                        xx,yy=x+dx,y+dy
                        if spec['wrap']:xx%=w;yy%=h
                        if 0<=xx<w and 0<=yy<h and (xx,yy)!=(x,y):positions.add((xx,yy))
                count=sum(state[yy*w+xx] for xx,yy in positions)
                next_state.append(int(count==3 or (state[y*w+x] and count==2)))
        state=next_state
    life.append(dict(case=spec['id'],verdict='pass'))
by_id={r['id']:r['states'] for r in native['life']}
assert by_id['block'][0]==by_id['block'][-1]
assert by_id['blinker'][0]==by_id['blinker'][2] and by_id['blinker'][0]!=by_id['blinker'][1]
expected_glider=[0]*64
for x,y in [(3,2),(4,3),(2,4),(3,4),(4,4)]:expected_glider[y*8+x]=1
assert by_id['glider'][4]==expected_glider
walk=[]
for r in native['walk']:
    t=r['time'];n=plan['walkers'];hist=dict(r['histogram']);assert sum(hist.values())==n
    assert all(abs(x)<=t and (x-t)%2==0 for x in hist)
    empirical=0.;target=0.;distance=0.
    for k in range(t+1):
        x=2*k-t;empirical+=hist.get(x,0)/n;target+=math.comb(t,k)/2**t
        distance=max(distance,abs(empirical-target))
    mean_gate=6*math.sqrt(t/n);msd_gate=6*math.sqrt(2*t*(t-1)/n)
    assert distance<=cdf_gate and abs(r['mean'])<=mean_gate and abs(r['msd']-t)<=msd_gate,(r,distance,msd_gate)
    walk.append(dict(replication=r['replication'],time=t,msd=r['msd'],expected_msd=t,cdf_distance=distance,
                     cdf_gate=cdf_gate,mean=r['mean'],mean_gate=mean_gate,msd_gate=msd_gate,verdict='pass'))
report=dict(verdict='pass',scope='native SyncPopulation/GridSpace and addressed Philox workloads; Life rules, not NetLogo binary execution',
            life_cases=len(life),life_cell_comparisons=compared,walk_gates=walk,life=life,
            sources=['https://ccl.northwestern.edu/netlogo/models/Life'],
            limitations=['No Ising/percolation claim','Random-walk result assumes independent streams; finite ensemble, not exact stochastic trajectory matching'])
(a.destination/'validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(dict(verdict='pass',life_cases=len(life),life_cell_comparisons=compared,walk_snapshots=len(walk))))
