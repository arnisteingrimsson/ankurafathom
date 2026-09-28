"""ARGESIM C22 pathwise comparison: native DEVS adapter vs Python event calendar.

The published workload definitions and selected deterministic solution tables are
used. Independent RNG distributional comparisons remain unassessed.
"""
import argparse
import heapq
import json
import math
from pathlib import Path
import random
import subprocess

SOURCE='https://www.sne-journal.org/fileadmin/user_upload_sne/SNE_Issues_OA/SNE_29_3/articles/sne.29.3.10481.bn22.OA.pdf'


def reference(spec):
    # Record-table reference: waiting order is an explicit rank, not native deque state.
    jobs=[dict(j,queue=None,rank=0,start=-1.,finish=-1.,reneged=False,eligible=False) for j in spec['jobs']]
    n=spec.get('queues',4);pending=[set() for _ in range(n)];active=[None]*n;mode=spec['mode'];events=[];serial=0;current=0;area=0.;previous=0.;maximum=0
    def enqueue(time,kind,index):
        nonlocal serial
        heapq.heappush(events,(time,serial,kind,index));serial+=1
    for i,j in enumerate(jobs):enqueue(j['arrival'],'arrival',i)
    if mode=='classing':enqueue(10.,'operator',None)
    def queued(q):return sorted(pending[q],key=lambda i:jobs[i]['rank'])
    def serving(q):return [] if active[q] is None else [active[q]]
    def length(q):return len(pending[q])+int(active[q] is not None)
    def put(i,q):
        line=queued(q)
        old=jobs[i]['queue']
        if old is not None:pending[old].remove(i)
        jobs[i]['queue']=q;jobs[i]['rank']=max((jobs[k]['rank'] for k in line),default=-1)+1
        pending[q].add(i)
    def dispatch(t):
        for q in range(n):
            line=queued(q)
            if not serving(q) and line:
                i=line[0];j=jobs[i]
                if mode!='classing' or j['eligible']:
                    pending[q].remove(i);active[q]=i;j['start']=t;enqueue(t+j['service'],'finish',i)
    def arrivals(batch,t):
        for _,_,kind,i in batch:
            if kind=='arrival':
                put(i,min(range(n),key=lambda q:(length(q),q)))
                if mode=='reneging':enqueue(t+9,'renege',i)
                dispatch(t)
    terminated=0;end=0
    while events and terminated<len(jobs):
        t=events[0][0];batch=[]
        while events and events[0][0]==t:batch.append(heapq.heappop(events))
        area+=(t-previous)*sum(len(queued(q)) for q in range(n));previous=t
        if spec.get('arrival_first'):arrivals(batch,t)
        for _,_,kind,i in batch:
            if kind=='finish':jobs[i]['finish']=t;active[jobs[i]['queue']]=None;terminated+=1
        dispatch(t)
        if mode=='reneging':
            for _,_,kind,i in batch:
                if kind=='renege' and jobs[i]['start']<0:
                    pending[jobs[i]['queue']].remove(i);jobs[i]['reneged']=True;jobs[i]['finish']=t;terminated+=1
        if mode=='jockeying':
            while True:
                eligible=[(d,s) for d in range(n) for s in range(n) if length(s)-length(d)>=2]
                if not eligible:break
                d,s=min(eligible,key=lambda pair:(pair[0],abs(pair[1]-pair[0]),pair[1]))
                put(queued(s)[-1],d);dispatch(t)
        if not spec.get('arrival_first'):arrivals(batch,t)
        if mode=='classing' and t>=10:
            present=[j for j in jobs if j['queue'] is not None and j['finish']<0]
            if present and not any(j['eligible'] for j in present):
                for _ in range(5):
                    current=current-1 if current>1 else 5
                    for q in range(n):
                        line=queued(q);ordered=[i for i in line if jobs[i]['class']==current]+[i for i in line if jobs[i]['class']!=current]
                        for rank,i in enumerate(ordered):jobs[i]['rank']=rank;jobs[i]['eligible']=jobs[i]['class']==current
                    dispatch(t)
                    if any(j['eligible'] for j in present):break
        maximum=max(maximum,sum(len(queued(q)) for q in range(n)));end=t
    assert terminated==len(jobs), (terminated,len(jobs),current,end)
    records=[dict(id=j['id'],start=j['start'],finish=j['finish'],reneged=j['reneged'],
                  wait=(j['finish'] if j['reneged'] else j['start'])-j['arrival']) for j in jobs]
    waits=[j['wait'] for j in records]
    assert math.isclose(area,sum(waits),rel_tol=1e-10,abs_tol=1e-9), 'finite drained queue area must equal all waiting intervals'
    return dict(jobs=records,horizon=end,queue_area=area,queue_mean=area/end,queue_max=maximum,wait_mean=sum(waits)/len(waits),wait_max=max(waits))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('executable',type=Path);p.add_argument('destination',type=Path)
    a=p.parse_args();a.destination.mkdir(parents=True,exist_ok=False);exe=a.executable.resolve();results=[];comparisons=0
    specs=[]
    for mode in ('base','jockeying','reneging','classing'):
        jobs=[dict(id=i+1,arrival=float(i+1),service=4.5,**{'class':i%5+1}) for i in range(100)]
        specs.append((mode+'-deterministic',dict(mode=mode,jobs=jobs)))
        for rep in range(16):
            rng=random.Random(20260928+rep);arrive=1.;jobs=[]
            for i in range(500):
                if i:arrive+=rng.expovariate(1.)
                jobs.append(dict(id=i+1,arrival=arrive,service=rng.triangular(2.5,6.5,4.5),**{'class':rng.randrange(1,6)}))
            specs.append((f'{mode}-stochastic-{rep}',dict(mode=mode,jobs=jobs)))
    specs.append(('base-arrival-first',dict(specs[0][1],arrival_first=True)))
    rng=random.Random(20260928);arrive=1.;jobs=[]
    for i in range(5000):
        if i:arrive+=rng.expovariate(10.)
        jobs.append(dict(id=i+1,arrival=arrive,service=rng.triangular(2.5,6.5,4.5),**{'class':rng.randrange(1,6)}))
    specs.append(('base-large-stochastic',dict(mode='base',queues=40,jobs=jobs)))
    for name,spec in specs:
        path=a.destination/(name+'.input.json');path.write_text(json.dumps(spec)+'\n')
        native=subprocess.run([str(exe),str(path)],check=True,capture_output=True,text=True)
        result=json.loads(native.stdout);(a.destination/(name+'.native.json')).write_text(native.stdout)
        expected=reference(spec);worst=0
        for key in ('horizon','queue_area','queue_mean','queue_max','wait_mean','wait_max'):
            error=abs(result[key]-expected[key]);worst=max(worst,error)
            assert math.isclose(result[key],expected[key],rel_tol=2e-11,abs_tol=1e-8),(name,key,result[key],expected[key]);comparisons+=1
        assert len(result['jobs'])==len(expected['jobs'])
        if name=='reneging-deterministic':
            assert [(e['time'],e['id']) for e in result['events'] if e['event']=='renege']==[(77,68),(86,77),(95,86),(104,95)]
        if name=='jockeying-deterministic':
            moves=[(e['time'],e['id'],e['from'],e['queue']) for e in result['events'] if e['event']=='jockey']
            assert moves[:5]==[(6.5,6,1,2),(7.5,7,1,3),(8.5,8,1,4),(11,10,1,2),(12,11,1,3)]
            assert moves[-5:]==[(89.5,89,2,4),(93,92,2,3),(94,93,2,4),(98.5,98,3,4),(103,100,1,4)]
        if name=='classing-deterministic':
            for cls,mean,maximum in zip(result['classes'],[66.80,60.85,41.85,50.98,60.77],[121.50,104.,77.,120.,125.50]):
                assert abs(cls['mean_wait']-mean)<=.00500001 and cls['max_wait']==maximum,(cls,mean,maximum)
        for actual,wanted in zip(result['jobs'],expected['jobs']):
            for key,value in wanted.items():
                assert math.isclose(actual[key],value,rel_tol=2e-11,abs_tol=1e-8),(name,key,actual,wanted);comparisons+=1
        assert math.isclose(result['queue_area'],sum(j['wait'] for j in result['jobs']),rel_tol=2e-11,abs_tol=1e-8)
        results.append(dict(case=name,verdict='pass',jobs=len(spec['jobs']),horizon=result['horizon'],queue_max=result['queue_max'],
            queue_mean=result['queue_mean'],reneged=sum(j['reneged'] for j in result['jobs']),
            jockey_events=sum(e['event']=='jockey' for e in result['events']),max_summary_gap=worst))
    report=dict(source=SOURCE,scope='C22 numeric workload execution through one custom native atomic; no claim of full published-solution docking or declarative policy support',
        verdict='pass',cases=results,comparisons=comparisons,stochastic='16 fixed-seed paths per policy, shared input draws across implementations; distributional equivalence not claimed',
        published_checks='MatlabGPSS Tables 3, 5 and 7: jockey moves, reneging times/IDs, class wait summaries; rounded means use half-last-decimal tolerance',
        classing_semantics='MatlabGPSS incoming-chain/called-batch convention; new arrivals are not included in the already called batch',
        published_source='https://www.sne-journal.org/fileadmin/user_upload_sne/SNE_Issues_OA/SNE_29_4/articles/sne.29.4.10496.bn22.OA.pdf',
        unassessed=['Full published solution trajectories/statistics','Independent RNG distribution comparison'],
        out_of_scope=['Graphical benchmark submission artifacts'])
    (a.destination/'validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(verdict='pass',cases=len(results),comparisons=comparisons)))
if __name__=='__main__':main()
