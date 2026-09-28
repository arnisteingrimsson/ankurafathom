"""Pinned Mesa direct SIR races, independent event replay and distribution gates."""
import argparse
import copy
import importlib.metadata
import json
import math
from pathlib import Path
import statistics
import sys
from sir_async_plan import HERE, make_plan, encoded as encoded_plan
from sir_oracle import require,digest,encode,metrics,validate_states,reachable,ks
REFERENCE=HERE/'sir-async-reference.json'


def metadata():
    return dict(plan_sha256=digest(HERE/'sir-async-plan.json'),adapter_sha256=digest(Path(__file__)),
                convergence_sha256=digest(HERE/'sir_convergence.py'),
                requirements_sha256=digest(HERE/'requirements.txt'),
                engines=dict(Mesa='3.5.1',networkx='3.7',scipy='1.18.1'))


def rates(case,states):
    result=[]
    for i,s in enumerate(states):
        contacts=sum(states[b if a==i else a]==1 for a,b in case['edges'] if a==i or b==i)
        result.append(case['infection_rate']*contacts if s==0 else case['recovery_rate'] if s==1 else 0)
    return result


def generate():
    for line in (HERE/'requirements.txt').read_text().splitlines():
        package,version=line.split('==')
        require(importlib.metadata.version(package)==version,'unpinned '+package)
    import mesa
    from mesa.space import NetworkGrid
    import networkx as nx
    from sir_convergence import pinned

    class Person(mesa.Agent):
        def __init__(self,model,index,state):
            super().__init__(model)
            self.index,self.state=index,state

        def hazard(self):
            if self.state==2:
                return 0.
            if self.state==1:
                return self.model.case['recovery_rate']
            return self.model.case['infection_rate']*sum(a.state==1 for a in self.model.grid.get_neighbors(self.pos,include_center=False))

    class Epidemic(mesa.Model):
        def __init__(self,case,seed):
            super().__init__(seed=seed)
            self.case=case
            self.clock=0.
            self.events=[]
            graph=nx.Graph()
            graph.add_nodes_from(range(len(case['states'])))
            graph.add_edges_from(case['edges'])
            self.grid=NetworkGrid(graph)
            self.people=[]
            for i,s in enumerate(case['states']):
                person=Person(self,i,s)
                self.people.append(person)
                self.grid.place_agent(person,i)
            self.schedule()

        def states(self):
            return [a.state for a in self.people]

        def schedule(self):
            weights=[a.hazard() for a in self.people]
            total=sum(weights)
            if total==0:
                self.pending=None
            else:
                deadline=self.clock+self.random.expovariate(total)
                person=self.random.choices(self.people,weights=weights,k=1)[0]
                self.pending=deadline,person

        def run(self,horizon):
            while self.pending and self.pending[0]<=horizon:
                self.clock,person=self.pending
                before=person.state
                require(before in (0,1) and person.hazard()>0,'Mesa impossible event')
                person.state+=1
                self.events.append([self.clock,person.index,before,person.state,len(self.events)])
                validate_states(self.case,self.states())
                self.schedule()
            self.clock=horizon

        def summaries(self):
            states=self.states()
            return dict(infected_fraction=sum(a.state==1 for a in self.agents)/len(states),
                        recovered_fraction=sum(a.state==2 for a in self.agents)/len(states),
                        attack_fraction=(self.case['states'].count(0)-sum(a.state==0 for a in self.agents))/self.case['states'].count(0),
                        si_edge_fraction=sum({self.grid.G.nodes[a]['agent'][0].state,self.grid.G.nodes[b]['agent'][0].state}=={0,1}
                                             for a,b in self.grid.G.edges)/self.grid.G.number_of_edges())

    plan,rows=make_plan(),[]
    for index,case in enumerate(plan['cases']):
        for r in range(plan['replications']):
            m=Epidemic(case,plan['reference_seed']+1000003*index+r)
            for t in plan['times']:
                m.run(t)
                rows.append(dict(case=case['id'],replication=r,time=t,states=m.states(),events=copy.deepcopy(m.events),metrics=m.summaries()))
    return dict(metadata=metadata(),rows=rows,convergence=pinned())


def validate_rows(rows,reference=False):
    p=make_plan()
    expected=[(c,r,t) for c in p['cases'] for r in range(p['replications']) for t in p['times']]
    require(len(rows)==len(expected),'async observation coverage')
    previous=None
    for row,(c,r,t) in zip(rows,expected):
        require(set(row)=={'case','replication','time','states','events'}|({'metrics'} if reference else set()),'async row fields')
        require(row['case']==c['id'] and type(row['replication']) is int and row['replication']==r
                and type(row['time']) in (int,float) and row['time']==t,'async row identity')
        validate_states(c,row['states'])
        require(isinstance(row['events'],list) and len(row['events'])<=2*len(c['states']),'async event count')
        state=list(c['states']); now=0.
        for generation,event in enumerate(row['events']):
            require(isinstance(event,list) and len(event)==5,'async event shape')
            time,i,before,after,g=event
            require(type(time) in (int,float) and math.isfinite(time) and now<time<=t,'async event clock')
            require(all(type(v) is int for v in event[1:]) and 0<=i<len(state) and g==generation,'async event identity')
            active=(c['recovery_rate']>0 if before==1 else c['infection_rate']>0 and
                    any(state[b if a==i else a]==1 for a,b in c['edges'] if a==i or b==i))
            require(state[i]==before and after==before+1 and before in (0,1) and active,'async impossible transition')
            state[i]=after; now=time
        require(state==row['states'],'async event/state reconstruction')
        if t==0:
            require(not row['events'] and row['states']==c['states'],'async initial state')
        else:
            require(row['events'][:len(previous['events'])]==previous['events'],'async event history changed')
        if reference:
            derived=metrics(c,row)
            require(set(row['metrics'])==set(p['metrics']),'async metric names')
            for name,value in row['metrics'].items():
                require(type(value) in (int,float) and math.isfinite(value) and abs(value-derived[name])<1e-14,'Mesa/scorer summary differs')
        previous=row


def read_reference():
    value=json.loads(REFERENCE.read_text())
    require(set(value)=={'metadata','rows','convergence'} and value['metadata']==metadata(),'stale async SIR reference')
    validate_rows(value['rows'],True)
    return value


def compare(rows,reference):
    p=make_plan(); cases={c['id']:c for c in p['cases']}
    native,ref={},{}
    for row in rows:
        native.setdefault((row['case'],row['time']),[]).append(metrics(cases[row['case']],row))
    for row in reference:
        ref.setdefault((row['case'],row['time']),[]).append(row['metrics'])
    n,policy=p['replications'],p['gates']
    limit=math.sqrt(math.log(2*policy['comparisons']/policy['family_alpha'])/n)
    gates=[]
    for c in p['cases']:
        for t in p['times'][1:]:
            for metric in p['metrics']:
                a=[s[metric] for s in native[c['id'],t]]; b=[s[metric] for s in ref[c['id'],t]]
                ma,mb=statistics.mean(a),statistics.mean(b)
                se=math.sqrt((statistics.variance(a)+statistics.variance(b))/n)
                mean_limit=max(policy['mean_floor'][metric],policy['mean_sigma']*se)
                distance=ks(a,b)
                gates.append(dict(case=c['id'],time=t,metric=metric,native_mean=ma,reference_mean=mb,
                                  mean_gap=abs(ma-mb),mean_limit=mean_limit,difference_ci95=[ma-mb-1.96*se,ma-mb+1.96*se],
                                  ks=distance,ks_limit=limit,passed=abs(ma-mb)<=mean_limit and distance<=limit))
    require(len(gates)==policy['comparisons'],'async gate count')
    return gates


def addressed(case,draws,times,first=0):
    sys.path.insert(0,str(HERE.parents[1]))
    from abm_ir_contract import word
    states=list(case['states']); now=0.; events=[]; result={}; generation=0
    pending=None
    def schedule():
        weights=rates(case,states); total=sum(weights)
        if not total:
            return None
        def u(stream):
            return (word(draws['seed'],draws['scenario'],draws['replication'],first,generation,stream)+.5)/2**32
        time=now-math.log(u(draws['waiting_stream']))/total
        target=min(u(draws['selection_stream'])*total,math.nextafter(total,0.))
        cumulative=0.
        for i,rate in enumerate(weights):
            cumulative+=rate
            if target<cumulative:
                return time,i
        raise ValueError('Python event selection failed')
    pending=schedule()
    for t in times:
        while pending and pending[0]<=t:
            now,i=pending
            before=states[i]; states[i]+=1
            events.append([now,first+i,before,states[i],generation]); generation+=1
            pending=schedule()
        result[t]=dict(states=list(states),events=copy.deepcopy(events))
    return result


def exact(rows):
    p=make_plan(); lookup={(r['case'],r['replication'],r['time']):r for r in rows}
    count=events=0; maximum=0.
    for c in p['cases']:
        for r in [0,1,p['replications']-1]:
            for t,expected in addressed(c,dict(p,replication=r),p['times']).items():
                actual=lookup[c['id'],r,t]
                require(actual['states']==expected['states'] and len(actual['events'])==len(expected['events']),'addressed async states/count')
                count+=len(actual['states'])
                for a,b in zip(actual['events'],expected['events']):
                    require(a[1:]==b[1:],'addressed async event differs')
                    error=abs(a[0]-b[0]); maximum=max(maximum,error)
                    require(error<=p['event_tolerance']*max(1,abs(b[0])),'addressed async event clock differs')
                    events+=1
    return dict(agent_states=count,event_observations=events,max_time_error=maximum)


def verify():
    saved=read_reference(); regenerated=generate(); maximum=0.
    validate_rows(regenerated['rows'],True)
    require(regenerated['metadata']==saved['metadata'],'regenerated metadata differs')
    require(len(regenerated['convergence'])==len(saved['convergence']),'regenerated joint-law coverage differs')
    for a,b in zip(saved['rows'],regenerated['rows']):
        require({k:v for k,v in a.items() if k!='events'}=={k:v for k,v in b.items() if k!='events'},'Mesa async row changed')
        require(len(a['events'])==len(b['events']),'Mesa async event count changed')
        for x,y in zip(a['events'],b['events']):
            require(x[1:]==y[1:],'Mesa async transition changed')
            error=abs(x[0]-y[0]); maximum=max(maximum,error)
            require(error<=make_plan()['event_tolerance']*max(1,abs(x[0])),'Mesa async clock changed')
    for a,b in zip(saved['convergence'],regenerated['convergence']):
        require((a['case'],a['time'],a['dt'])==(b['case'],b['time'],b['dt']) and
                len(a['probabilities'])==len(b['probabilities']) and
                max(abs(x-y) for x,y in zip(a['probabilities'],b['probabilities']))<1e-12,'SciPy joint law changed')
    print('Pinned Mesa/SciPy regeneration passed; maximum event time error',maximum)


def contract():
    ref=read_reference()
    for mutate in [lambda r:r['rows'].pop(),lambda r:r['metadata'].__setitem__('adapter_sha256','stale'),
                   lambda r:r['rows'][1].__setitem__('time',True),lambda r:r['rows'][1]['states'].__setitem__(0,True),
                   lambda r:r['rows'][1]['events'][0].__setitem__(0,-1),lambda r:r['rows'][1]['events'][0].__setitem__(4,99),
                   lambda r:r['rows'][1]['events'][0].__setitem__(3,9),lambda r:r['rows'][1]['events'].pop(),
                   lambda r:r['rows'][1]['metrics'].__setitem__('attack_fraction',99)]:
        bad=copy.deepcopy(ref); mutate(bad)
        try:
            require(bad['metadata']==metadata(),'stale metadata'); validate_rows(bad['rows'],True)
        except ValueError:
            continue
        raise ValueError('corrupt async reference accepted')
    p=make_plan(); cases={c['id']:c for c in p['cases']}
    frozen=[dict(case=r['case'],replication=r['replication'],time=r['time'],states=cases[r['case']]['states'],events=[]) for r in ref['rows']]
    validate_rows(frozen)
    require(any(not g['passed'] for g in compare(frozen,ref['rows'])),'frozen async process passed gates')
    print('Async SIR reference, event-history, corruption and wrong-dynamics contracts passed')


if __name__=='__main__':
    p=argparse.ArgumentParser()
    for flag in ['write','verify','contract']:
        p.add_argument('--'+flag,action='store_true')
    p.add_argument('--native'); p.add_argument('--report'); args=p.parse_args()
    require((HERE/'sir-async-plan.json').read_text()==encoded_plan(),'stale async SIR plan')
    if args.write:
        REFERENCE.write_text(encode(generate()))
    if args.verify:
        verify()
    if args.contract:
        contract()
    if args.native:
        with Path(args.native).open() as file:
            require(json.loads(next(file))==dict(plan=make_plan()),'native async plan differs')
            rows=[json.loads(line) for line in file]
        validate_rows(rows); observations=exact(rows); gates=compare(rows,read_reference()['rows'])
        result=dict(metadata=metadata(),runs_per_engine=len(make_plan()['cases'])*make_plan()['replications'],
                    exact=observations,gates=gates,passed=all(g['passed'] for g in gates))
        if args.report:
            Path(args.report).write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
        require(result['passed'],'async SIR gates failed: '+str([g for g in gates if not g['passed']]))
        print('Async SIR: 32 Mesa distribution gates passed;',observations)
