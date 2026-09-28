"""Isaac 2011 computational template workloads; independent record-table oracle."""
import argparse
import copy
import json
import math
from pathlib import Path
import random
import subprocess

SOURCE = 'https://www.jasss.org/14/2/5.html'


def reference(spec):
    version = spec['version']; width=spec['width']; height=spec['height']
    production=[]
    if version >= 15:
        rows=[line.split() for line in Path(spec['landscape']).read_text().splitlines()[3:]]
        coordinates={(int(x),int(y)):float(rate) for x,y,rate in rows}
        width=max(x for x,y in coordinates)+1; height=max(y for x,y in coordinates)+1
        production=[coordinates[x,y] for y in range(height) for x in range(width)]
    food=spec.get('food',[0.]*(width*height)).copy()
    agents={}
    for id,a in enumerate(spec['agents']):
        size=spec.get('initial_size',0.)
        if version >= 14:size=max(0.,spec.get('initial_mean',.1)+spec.get('initial_sd',.03)*a['normal'])
        agents[id]=[a['cell'],a.get('size',size)]
    hunters=dict(enumerate(spec.get('hunters',[]))) if version == 16 else {}
    next_id=len(agents); cursor=0; births=deaths=predation=failed=0; produced=consumed=0.; order=[]
    def uniform():
        nonlocal cursor
        value=spec['uniforms'][cursor];cursor+=1;return value
    def permute(values):
        result=list(values)
        for last in reversed(range(1,len(result))):
            j=int(uniform()*(last+1));result[last],result[j]=result[j],result[last]
        return result
    def neighbors(cell,radius,center=False):
        x,y=cell%width,cell//width;result=set()
        for dx in range(-radius,radius+1):
            for dy in range(-radius,radius+1):
                xx,yy=x+dx,y+dy
                if version < 15:xx%=width;yy%=height
                if 0<=xx<width and 0<=yy<height and (center or (xx,yy)!=(x,y)):result.add(yy*width+xx)
        return sorted(result)
    def snapshot(tick):
        sizes=[a[1] for a in agents.values()];hist=[0]*11
        for size in sizes:hist[min(10,math.floor(size))]+=1
        return dict(tick=tick,agents=[[id,*agents[id]] for id in sorted(agents)],hunters=[[id,hunters[id]] for id in sorted(hunters)],
                    food=food.copy(),births=births,deaths=deaths,predation=predation,failed_births=failed,produced=produced,consumed=consumed,
                    histogram=hist,minimum=min(sizes,default=0.),maximum=max(sizes,default=0.),mean=sum(sizes)/len(sizes) if sizes else 0.,move_order=order.copy())
    states=[snapshot(0)];stopped=False
    for tick in range(1,spec['steps']+1):
        if version>=3:
            increments=production if version>=15 else [uniform()*spec.get('max_produce',.01) for _ in food]
            for i,delta in enumerate(increments):food[i]+=delta;produced+=delta
        ids=sorted(agents)
        order=permute(ids) if version==9 else sorted(ids,key=lambda id:(-agents[id][1],id)) if version>=10 else ids.copy()
        occupied={a[0] for a in agents.values()}
        for id in order:
            old=agents[id][0];available=[c for c in neighbors(old,4) if c not in occupied];chosen=old
            if version<11:
                if available:chosen=available[int(uniform()*len(available))]
            else:
                maximum=max([food[old]]+[food[c] for c in available])
                if food[old]<maximum:
                    choices=[c for c in available if food[c]==maximum];chosen=choices[int(uniform()*len(choices))]
            occupied.remove(old);occupied.add(chosen);agents[id][0]=chosen
        if version>=2:
            for id in ids:
                cell=agents[id][0]
                amount=spec.get('extraction_rate',.1) if version==2 else min(spec.get('max_extract',1.),food[cell])
                agents[id][1]+=amount;consumed+=amount
                if version>=3:food[cell]-=amount
        if version>=12:
            for id in permute(ids):
                if agents[id][1]<=10:continue
                parent=agents[id][0]
                for _ in range(5):
                    candidates=permute(neighbors(parent,3))[:5]
                    free=next((c for c in candidates if c not in occupied),None)
                    if free is None:failed+=1
                    else:
                        agents[next_id]=[free,spec.get('initial_size',0.)];occupied.add(free);next_id+=1;births+=1
                occupied.remove(parent);del agents[id];deaths+=1
            exits=[id for id in sorted(agents) if uniform()<spec.get('exit_probability',.05)]
            for id in exits:del agents[id];deaths+=1
        if version==16:
            for id in permute(sorted(hunters)):
                search=permute(neighbors(hunters[id],1,True))
                for cell in search:
                    if any(h!=id and position==cell for h,position in hunters.items()):break
                    prey=next((a for a,state in agents.items() if state[0]==cell),None)
                    if prey is not None:
                        del agents[prey];predation+=1;hunters[id]=cell;break
                else:hunters[id]=search[int(uniform()*len(search))]
        states.append(snapshot(tick))
        stopped=(not agents or tick>=1000) if version>=12 else (any(a[1]>=100 for a in agents.values()) if version>=7 else False)
        if stopped:break
    return dict(id=spec['id'],version=version,states=states,draws_used=cursor,stopped=stopped,width=width,height=height)


def compare(actual,expected,path=''):
    if isinstance(expected,dict):
        assert actual.keys()==expected.keys(),path
        return sum(compare(actual[k],v,path+'/'+str(k)) for k,v in expected.items())
    if isinstance(expected,list):
        assert len(actual)==len(expected),(path,len(actual),len(expected))
        return sum(compare(a,b,path+'/'+str(i)) for i,(a,b) in enumerate(zip(actual,expected)))
    if isinstance(expected,float):assert math.isfinite(actual) and math.isclose(actual,expected,rel_tol=2e-12,abs_tol=2e-12),(path,actual,expected)
    else:assert actual==expected,(path,actual,expected)
    return 1


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable',type=Path);parser.add_argument('destination',type=Path);args=parser.parse_args()
    args.destination.mkdir(parents=True,exist_ok=False);cases=[]
    def make(name,version,width,height,count,steps,seed):
        rng=random.Random(seed)
        agents=[dict(cell=c,normal=rng.gauss(0,1)) for c in rng.sample(range(width*height),count)]
        spec=dict(id=name,version=version,width=width,height=height,steps=steps,agents=agents)
        if version==16:spec['hunters']=rng.sample(range(width*height),200 if width>=20 else 5)
        if version>=15:
            path=args.destination/(name+'.Cell.Data')
            rows=[f'{x} {y} {((3*x+y)%7)/100}' for y in range(height) for x in range(width)];rng.shuffle(rows)
            path.write_text('Synthetic validation landscape\nx y production\nThree header rows\n'+'\n'.join(rows)+'\n')
            spec['landscape']=str(path.resolve())
        return spec
    for version in range(1,17):
        size=100 if version<15 else 20
        cases.append(make(f'v{version:02d}-default-scale',version,size,size,100,4,1000+version))
        cases.append(make(f'v{version:02d}-dense',version,7,6,30,8,2000+version))
    controls=[
        ('packed-movement',1,3,3,9,3,{}),
        ('constant-growth',2,9,9,2,4,{}),
        ('state-stop',7,9,9,1,4,dict(initial_size=99.5,food=[2.]*81,max_produce=0.)),
        ('tied-best-stays',11,9,9,2,4,dict(max_produce=0.)),
        ('threshold-is-strict',12,9,9,1,2,dict(initial_size=10.,max_produce=0.,exit_probability=0.)),
        ('five-offspring',12,9,9,1,1,dict(initial_size=0.,max_produce=0.,exit_probability=0.)),
        ('newborn-mortality',12,9,9,1,1,dict(initial_size=0.,max_produce=0.,exit_probability=1.)),
        ('blocked-births',12,3,3,9,1,dict(initial_size=0.,max_produce=0.,exit_probability=0.)),
        ('iteration-stop',12,1,1,1,1002,dict(max_produce=0.,exit_probability=0.)),
        ('clamped-normal',14,9,9,2,0,{}),
        ('predation',16,3,3,1,1,dict(exit_probability=0.,max_extract=0.)),
    ]
    for i,(name,v,w,h,count,steps,overrides) in enumerate(controls):
        spec=make(name,v,w,h,count,steps,3000+i);spec.update(overrides)
        if name in ('five-offspring','newborn-mortality','blocked-births'):spec['agents'][0]['size']=10.01
        if name=='clamped-normal':spec['agents'][0]['normal']=-10.;spec['agents'][1]['normal']=2.
        if name=='predation':
            spec['hunters']=[spec['agents'][0]['cell']]
            # Equal food keeps the prey on its current best cell before hunting.
            Path(spec['landscape']).write_text('Synthetic zero landscape\nx y production\nThree header rows\n'+
                ''.join(f'{x} {y} 0\n' for y in range(h) for x in range(w)))
        cases.append(spec)
    # Inputs and numeric gates fixed before invoking any native workload.
    plan=dict(source=SOURCE,versions=list(range(1,17)),cases=[s['id'] for s in cases],numeric_tolerance=2e-12,
              random_input='Fixed independent Python draws supplied to both engines; distributional docking is not claimed',
              out_of_scope=['Interactive GUI/probes and rendered histogram/time-series plots'],
              unassessed=['Original Cell.Data landscape','Published Python/NetLogo executable docking'])
    (args.destination/'plan.json').write_text(json.dumps(plan,indent=2)+'\n')
    for i,spec in enumerate(cases):
        rng=random.Random(917000+i)
        capacity=10000+spec['steps']*(spec['width']*spec['height']+len(spec['agents'])*100+len(spec.get('hunters',[]))*20)
        spec['uniforms']=[rng.random() for _ in range(capacity)]
        (args.destination/(spec['id']+'.input.json')).write_text(json.dumps(spec)+'\n')
    comparisons=0;results=[];observed={}
    for spec in cases:
        path=args.destination/(spec['id']+'.input.json')
        raw=subprocess.check_output([str(args.executable.resolve()),str(path)],text=True)
        actual=json.loads(raw);expected=reference(spec)
        comparisons+=compare(actual,expected,spec['id']);observed[spec['id']]=actual
        (args.destination/(spec['id']+'.native.json')).write_text(raw)
        for state in actual['states']:
            ids=[a[0] for a in state['agents']];positions=[a[1] for a in state['agents']]
            assert len(ids)==len(set(ids)) and len(positions)==len(set(positions))
            assert len(ids)==len(spec['agents'])+state['births']-state['deaths']-state['predation']
            assert sum(state['histogram'])==len(ids)
            if spec['version']>=3:
                assert math.isclose(sum(state['food'])+state['consumed'],sum(spec.get('food',[]))+state['produced'],rel_tol=2e-10,abs_tol=2e-10)
        results.append(dict(case=spec['id'],version=spec['version'],snapshots=len(actual['states']),verdict='pass'))
    end=lambda name:observed[name]['states'][-1]
    assert end('packed-movement')['agents']==observed['packed-movement']['states'][0]['agents']
    assert all(math.isclose(a[2],.4) for a in end('constant-growth')['agents'])
    assert observed['state-stop']['stopped'] and end('state-stop')['tick']==1 and end('state-stop')['maximum']==100.5
    assert end('tied-best-stays')['agents']==observed['tied-best-stays']['states'][0]['agents']
    assert end('threshold-is-strict')['births']==0
    assert end('five-offspring')['births']==5 and end('five-offspring')['deaths']==1
    assert end('newborn-mortality')['births']==5 and end('newborn-mortality')['deaths']==6 and observed['newborn-mortality']['stopped']
    assert end('blocked-births')['births']==0 and end('blocked-births')['failed_births']==5
    assert observed['iteration-stop']['stopped'] and end('iteration-stop')['tick']==1000
    assert [a[2] for a in end('clamped-normal')['agents']]==[0.,.16]
    assert end('predation')['predation']==1 and observed['predation']['stopped']
    # Missing/duplicate/malformed habitat data must not silently change the world.
    failures=[];base=copy.deepcopy(cases[-1])
    for name,body in [('missing-cell','0 0 .01\n1 1 .02\n'),('duplicate','0 0 .01\n0 0 .02\n'),('negative','0 0 -.01\n'),('extra-column','0 0 .01 ignored\n')]:
        data=args.destination/(name+'.data');data.write_text('a\nb\nc\n'+body);base['landscape']=str(data.resolve())
        path=args.destination/(name+'.input.json');path.write_text(json.dumps(base)+'\n')
        result=subprocess.run([str(args.executable.resolve()),str(path)],capture_output=True,text=True)
        assert result.returncode!=0 and not result.stdout,(name,result.stdout)
        failures.append(dict(case=name,diagnostic=result.stderr.strip()))
    report=dict(verdict='pass',source=SOURCE,scope='Computational behavior across template versions 1–16 through native GridSpace and SyncPopulation; engine-result acceptance',
                comparisons=comparisons,cases=results,invalid_landscapes=failures,unassessed=plan['unassessed'],out_of_scope=plan['out_of_scope'],
                source_archive=dict(url='https://www.jasss.org/14/2/5/isaac-2011-code4jasss.zip',sha256='bd49dbb4a4060e33ed8ae865fd4a07f2be6e17769fadcc0e58bb955116a864ee',executed=False))
    (args.destination/'validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(verdict='pass',versions=16,cases=len(cases),comparisons=comparisons,invalid_landscapes=len(failures))))


if __name__=='__main__':main()
