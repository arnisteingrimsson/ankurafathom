"""C21 event-contact ball: native SD/DEVS against exact flight-segment formulas."""
import argparse
import bisect
import json
import math
from pathlib import Path
import subprocess

SOURCE='https://www.sne-journal.org/fileadmin/user_upload_sne/SNE_Issues_OA/SNE_26_2/articles/sne.26.2.10339.bn21.OA.pdf'


def oracle(p):
    g,b,mu=p['g'],p['beta'],p['mu']; height=p['height'];time=0.;impacts=[];apices=[];segments=[]
    for i in range(1000):
        if b:
            scale=math.sqrt(g*b);terminal=math.sqrt(g/b)
            down=math.asinh(math.sqrt(math.expm1(2*b*height)))/scale
            incoming=-terminal*math.tanh(scale*down)
        else:down=math.sqrt(2*height/g);incoming=-g*down
        segments.append((time,'down',height));time+=down
        outgoing=-mu*incoming;impacts.append(dict(time=time,before=incoming,after=outgoing))
        segments.append((time,'up',outgoing))
        if b:
            up=math.atan(outgoing/terminal)/scale
            height=math.log1p(b*outgoing*outgoing/g)/(2*b)
        else:up=outgoing/g;height=outgoing*outgoing/(2*g)
        time+=up;apices.append(dict(time=time,height=height))
        if height<p['height_cutoff']:break
    else:raise AssertionError('oracle failed to reach cutoff')
    return dict(impacts=impacts,apices=apices,segments=segments,stop_time=time)


def check(actual,p):
    expected=oracle(p);tol=1e-8 if not p['beta'] else 1e-8+.02*p['dt']**4
    assert actual['rest'] and actual['final_height']==0 and actual['final_velocity']==0
    assert len(actual['impacts'])==len(expected['impacts'])>=100
    assert len(actual['apices'])==len(expected['apices'])
    comparisons=0;gaps={'time':0.,'velocity':0.,'height':0.}
    for key in ('impacts','apices'):
        for a,e in zip(actual[key],expected[key]):
            for field,target in e.items():
                error=abs(a[field]-target);category='time' if field=='time' else 'height' if field=='height' else 'velocity'
                gaps[category]=max(gaps[category],error)
                assert error<=tol,(p,key,field,a[field],target,tol);comparisons+=1
    if not p['beta']:
        first=math.sqrt(2*p['height']/p['g']);mu=p['mu']
        for m,impact in enumerate(actual['impacts'][:100],1):
            target=first*(1+2*mu*(1-mu**(m-1))/(1-mu))
            assert abs(impact['time']-target)<=tol;comparisons+=1
        limit=first*(1+mu)/(1-mu)
        # Rest at the cutoff apex approximates the Zeno limit, with an explicit
        # remaining-flight bound; it is not claimed as the exact last impact.
        remaining=math.sqrt(2*p['height_cutoff']/p['g'])*(1+mu)/(1-mu)
        assert abs(actual['stop_time']-limit)<=remaining+tol;comparisons+=1
    previous=0.;starts=[s[0] for s in expected['segments']]
    for sample in actual['samples']:
        t=sample['time'];assert t>previous and t<=p['horizon'];previous=t
        assert sample['height']>=-tol
        if sample['event']:continue
        start,phase,value=expected['segments'][bisect.bisect_right(starts,t)-1];elapsed=t-start;g=p['g'];b=p['beta']
        if b:
            scale=math.sqrt(g*b);terminal=math.sqrt(g/b)
            if phase=='down':h=value-math.log(math.cosh(scale*elapsed))/b;v=-terminal*math.tanh(scale*elapsed)
            else:
                theta=math.atan(value/terminal);angle=theta-scale*elapsed
                h=(math.log(math.cos(angle))-math.log(math.cos(theta)))/b;v=terminal*math.tan(angle)
        elif phase=='down':h=value-g*elapsed**2/2;v=-g*elapsed
        else:h=value*elapsed-g*elapsed**2/2;v=value-g*elapsed
        assert abs(sample['height']-h)<=tol and abs(sample['velocity']-v)<=tol,(p,t,sample,h,v,tol)
        comparisons+=2
    return dict(comparisons=comparisons,event_gaps=gaps,tolerance=tol,bounces=len(actual['impacts']))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('executable',type=Path);p.add_argument('destination',type=Path);a=p.parse_args()
    a.destination.mkdir(parents=True,exist_ok=False)
    cases=[dict(id=f'beta-{beta}-dt-{dt}',beta=beta,dt=dt,g=9.81,mu=.9,height=10.,horizon=30.,height_cutoff=1e-10)
           for beta in (0.,.002) for dt in (.2,.1,.05,.025)]
    (a.destination/'plan.json').write_text(json.dumps(dict(source=SOURCE,cases=cases,
        tolerance='beta=0: 1e-8; beta=.002: 1e-8 + .02*dt^4; all frozen before execution',
        scope='C21 section 2 event-contact ball only; custom guard localization around native SD RK4 and DEVS clock'),indent=2)+'\n')
    reports=[]
    def execute(spec,name):
        path=a.destination/(name+'.input.json');path.write_text(json.dumps(spec)+'\n')
        raw=subprocess.check_output([str(a.executable.resolve()),str(path)],text=True)
        (a.destination/(name+'.native.json')).write_text(raw);return json.loads(raw)
    for spec in cases:
        result=check(execute(spec,spec['id']),spec);reports.append(dict(case=spec['id'],**result))
    # Coarse event quantization is a deliberate wrong implementation. Its
    # height reset and velocity reflection still look plausible but must fail.
    broken=dict(cases[0],localize=False);actual=execute(broken,'negative-quantized-events')
    rejected=False
    try:check(actual,broken)
    except AssertionError:rejected=True
    assert rejected,'oracle accepted quantized bounce events'
    drag=[r['event_gaps']['time'] for r in reports[4:]]
    assert all(b<a for a,b in zip(drag,drag[1:])),drag
    report=dict(verdict='pass',cases=reports,comparisons=sum(r['comparisons'] for r in reports),negative_control_rejected=rejected,
                source=SOURCE,scope='Event-contact bouncing ball, with/without quadratic drag; first 100 bounce times and cutoff-rest behavior',
                unassessed=['Dynamic contact and spring/damper studies','Initial-velocity compensation fit','RLC diode/DAE cases',
                            'Structural pendulum case','General production state-event API'],
                out_of_scope=['Plots for a formal SNE submission'])
    (a.destination/'validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(verdict='pass',cases=len(reports),comparisons=report['comparisons'],negative_control_rejected=rejected)))


if __name__=='__main__':main()
