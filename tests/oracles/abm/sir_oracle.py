"""Independent pinned Mesa network SIR and offline native distribution gates."""
import argparse
import bisect
import copy
import hashlib
import importlib.metadata
import json
import math
from pathlib import Path
import statistics
import sys
from sir_plan import HERE, encoded as encoded_plan, make_plan
REFERENCE = HERE/'sir-reference.json'


def require(value,message):
    if not value:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata():
    return dict(plan_sha256=digest(HERE/'sir-plan.json'),adapter_sha256=digest(Path(__file__)),
                requirements_sha256=digest(HERE/'requirements.txt'),engines=dict(Mesa='3.5.1',networkx='3.7'))


def encode(value):
    return json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False)+'\n'


def metrics(case,row):
    states=row['states']
    n=len(states)
    return dict(infected_fraction=states.count(1)/n,recovered_fraction=states.count(2)/n,
                attack_fraction=(case['states'].count(0)-states.count(0))/case['states'].count(0),
                si_edge_fraction=sum({states[a],states[b]}=={0,1} for a,b in case['edges'])/len(case['edges']))


def reachable(case):
    # Only initially susceptible/infected vertices can carry this epidemic.
    seen={i for i,s in enumerate(case['states']) if s==1}
    while True:
        added=set()
        for a,b in case['edges']:
            if a in seen and case['states'][b]!=2:
                added.add(b)
            if b in seen and case['states'][a]!=2:
                added.add(a)
        if added<=seen:
            return seen
        seen|=added


def validate_states(case,states):
    require(isinstance(states,list) and len(states)==len(case['states']), 'SIR membership conservation')
    require(all(type(s) is int and 0<=s<=2 for s in states),'SIR state type/range')
    for initial,current in zip(case['states'],states):
        require(current>=initial,'SIR reverse transition/initial immunity')


def generate():
    for line in (HERE/'requirements.txt').read_text().splitlines():
        package,version=line.split('==')
        require(importlib.metadata.version(package)==version,'unpinned '+package)
    import mesa
    from mesa.space import NetworkGrid
    import networkx as nx

    class Person(mesa.Agent):
        def __init__(self,model,index,state):
            super().__init__(model)
            self.index,self.state,self.pending=index,state,state

        def compute(self):
            self.pending=self.state
            if self.state==0:
                infectious=sum(a.state==1 for a in self.model.grid.get_neighbors(self.pos,include_center=False))
                hazard=(self.model.case['infection_rate']*self.model.case['dt'])*infectious
                if self.random.random() < -math.expm1(-hazard):
                    self.pending=1
            elif self.state==1:
                if self.random.random() < -math.expm1(-self.model.case['recovery_rate']*self.model.case['dt']):
                    self.pending=2

        def commit(self):
            require(self.state<=self.pending<=self.state+1,'Mesa invalid one-tick transition')
            self.state=self.pending

    class Epidemic(mesa.Model):
        def __init__(self,case,seed):
            super().__init__(seed=seed)
            self.case=case
            graph=nx.Graph()
            graph.add_nodes_from(range(len(case['states'])))
            graph.add_edges_from(case['edges'])
            self.grid=NetworkGrid(graph)
            self.people=[]
            for i,state in enumerate(case['states']):
                person=Person(self,i,state)
                self.people.append(person)
                self.grid.place_agent(person,i)

        def states(self):
            return [a.state for a in self.people]

        def step(self):
            self.agents.do('compute')
            self.agents.do('commit')
            validate_states(self.case,self.states())
            require(len(self.agents)==len(self.case['states']), 'Mesa population changed')
            require(all(len(data['agent'])==1 for _,data in self.grid.G.nodes(data=True)), 'Mesa node occupancy')

        def summaries(self):
            n=len(self.people)
            s=sum(a.state==0 for a in self.agents)
            si=sum({data_a['agent'][0].state,data_b['agent'][0].state}=={0,1}
                   for a,b in self.grid.G.edges for data_a,data_b in [(self.grid.G.nodes[a],self.grid.G.nodes[b])])
            return dict(infected_fraction=sum(a.state==1 for a in self.agents)/n,
                        recovered_fraction=sum(a.state==2 for a in self.agents)/n,
                        attack_fraction=(self.case['states'].count(0)-s)/self.case['states'].count(0),
                        si_edge_fraction=si/self.grid.G.number_of_edges())

    plan,rows=make_plan(),[]
    for index,case in enumerate(plan['cases']):
        for replication in range(plan['replications']):
            model=Epidemic(case,plan['reference_seed']+1000003*index+replication)
            tick=0
            for observation in plan['ticks']:
                while tick<observation:
                    model.step()
                    tick+=1
                rows.append(dict(case=case['id'],replication=replication,tick=tick,time=tick*case['dt'],
                                 states=model.states(),metrics=model.summaries()))
    return dict(metadata=metadata(),rows=rows)


def validate_rows(rows,reference=False):
    plan=make_plan()
    expected=[(c,r,t) for c in plan['cases'] for r in range(plan['replications']) for t in plan['ticks']]
    accessible={c['id']:reachable(c) for c in plan['cases']}
    require(isinstance(rows,list) and len(rows)==len(expected),'SIR observation count')
    previous=None
    for row,(case,replication,tick) in zip(rows,expected):
        require(set(row)=={'case','replication','tick','time','states'}|({'metrics'} if reference else set()),'SIR row fields')
        require(row['case']==case['id'] and type(row['replication']) is int and row['replication']==replication
                and type(row['tick']) is int and row['tick']==tick,'SIR observation identity/order')
        require(type(row['time']) in (int,float) and math.isfinite(row['time']) and row['time']==tick*case['dt'],'SIR physical clock')
        validate_states(case,row['states'])
        for i,(initial,current) in enumerate(zip(case['states'],row['states'])):
            if initial==0 and i not in accessible[case['id']]:
                require(current==0,'SIR crossed disconnected/immune barrier')
        if tick==0:
            require(row['states']==case['states'],'SIR initial states differ')
        else:
            require(all(a<=b for a,b in zip(previous['states'],row['states'])),'SIR reverse history')
        if reference:
            require(set(row['metrics'])==set(plan['metrics']),'SIR metric names')
            derived=metrics(case,row)
            for name,value in row['metrics'].items():
                require(type(value) in (int,float) and math.isfinite(value) and abs(value-derived[name])<1e-14,'Mesa/scorer metric mismatch')
        previous=row


def read_reference():
    value=json.loads(REFERENCE.read_text())
    require(set(value)=={'metadata','rows'} and value['metadata']==metadata(),'stale SIR reference')
    validate_rows(value['rows'],reference=True)
    return value


def ks(a,b):
    a,b=sorted(a),sorted(b)
    return max(abs(bisect.bisect_right(a,x)/len(a)-bisect.bisect_right(b,x)/len(b)) for x in set(a+b))


def compare(rows,reference):
    plan=make_plan()
    cases={c['id']:c for c in plan['cases']}
    samples,ref={},{}
    for row in rows:
        samples.setdefault((row['case'],row['tick']),[]).append(metrics(cases[row['case']],row))
    for row in reference:
        ref.setdefault((row['case'],row['tick']),[]).append(row['metrics'])
    n,policy=plan['replications'],plan['gates']
    limit=math.sqrt(math.log(2*policy['comparisons']/policy['family_alpha'])/n)
    gates=[]
    for case in plan['cases']:
        for tick in plan['ticks'][1:]:
            for metric in plan['metrics']:
                a=[v[metric] for v in samples[case['id'],tick]]
                b=[v[metric] for v in ref[case['id'],tick]]
                ma,mb=statistics.mean(a),statistics.mean(b)
                se=math.sqrt((statistics.variance(a)+statistics.variance(b))/n)
                mean_limit=max(policy['mean_floor'][metric],policy['mean_sigma']*se)
                distance=ks(a,b)
                gates.append(dict(case=case['id'],tick=tick,metric=metric,native_mean=ma,reference_mean=mb,
                                  mean_gap=abs(ma-mb),difference_ci95=[ma-mb-1.96*se,ma-mb+1.96*se],
                                  mean_limit=mean_limit,ks=distance,ks_limit=limit,
                                  passed=abs(ma-mb)<=mean_limit and distance<=limit))
    require(len(gates)==policy['comparisons'],'SIR gate count drift')
    return gates


def addressed(case,draws,ticks,first=0):
    sys.path.insert(0,str(HERE.parents[1]))
    from abm_ir_contract import word
    states=list(case['states'])
    neighbors=[[] for _ in states]
    for a,b in case['edges']:
        neighbors[a].append(b)
        neighbors[b].append(a)
    result={}
    for tick in range(max(ticks)+1):
        if tick:
            updated=list(states)
            for i,state in enumerate(states):
                if state==2:
                    continue
                stream=draws['infection_stream'] if state==0 else draws['recovery_stream']
                u=(word(draws['seed'],draws['scenario'],draws['replication'],first+i,tick-1,stream)+.5)/2**32
                hazard=((case['infection_rate']*case['dt'])*sum(states[j]==1 for j in neighbors[i])
                        if state==0 else case['recovery_rate']*case['dt'])
                if u < -math.expm1(-hazard):
                    updated[i]=state+1
            states=updated
        if tick in ticks:
            result[tick]=list(states)
    return result


def exact(rows):
    plan=make_plan()
    actual={(r['case'],r['replication'],r['tick']):r for r in rows}
    count=0
    for case in plan['cases']:
        for replication in [0,1,plan['replications']-1]:
            for tick,states in addressed(case,dict(plan,replication=replication),plan['ticks']).items():
                require(actual[case['id'],replication,tick]['states']==states,'addressed SIR trajectory differs')
                count+=len(states)
    return count


def contract():
    reference=read_reference()
    for mutate in [lambda r:r['rows'].pop(),lambda r:r['metadata'].__setitem__('plan_sha256','stale'),
                   lambda r:r['rows'][1].__setitem__('replication',True),
                   lambda r:r['rows'][1].__setitem__('time',99),
                   lambda r:r['rows'][1]['states'].pop(),
                   lambda r:r['rows'][1]['states'].__setitem__(0,True),
                   lambda r:r['rows'][1]['states'].__setitem__(0,3),
                   lambda r:r['rows'][1]['states'].__setitem__(23,0),
                   lambda r:r['rows'][1]['metrics'].__setitem__('infected_fraction',99),
                   lambda r:r['rows'][-1]['states'].__setitem__(18,1)]:
        bad=copy.deepcopy(reference)
        mutate(bad)
        try:
            require(bad['metadata']==metadata(),'stale metadata')
            validate_rows(bad['rows'],reference=True)
        except ValueError:
            continue
        raise ValueError('corrupt SIR evidence accepted')
    line=dict(states=[1,0,0,2],edges=[[0,1],[1,2],[2,3]],infection_rate=800,recovery_rate=800,dt=1)
    draws=dict(seed=1,scenario=0,replication=0,infection_stream=1,recovery_stream=2)
    require(addressed(line,draws,[1,2,3])=={1:[2,1,0,2],2:[2,2,1,2],3:[2,2,2,2]},'hand synchronous recurrence')
    require(metrics(line,dict(states=[2,1,0,2]))==dict(infected_fraction=.25,recovered_fraction=.5,attack_fraction=.5,si_edge_fraction=1/3),'hand SIR metrics')
    require(ks([0,0],[1,1])==1 and ks([0,1],[0,1])==0,'KS contract')
    frozen=[]
    cases={c['id']:c for c in make_plan()['cases']}
    for row in reference['rows']:
        frozen.append(dict(case=row['case'],replication=row['replication'],tick=row['tick'],time=row['time'],states=list(cases[row['case']]['states'])))
    validate_rows(frozen)
    require(any(not g['passed'] for g in compare(frozen,reference['rows'])),'frozen SIR dynamics passed gates')
    print('SIR reference, transitions/metrics, corruption and wrong-dynamics contracts passed')


def score(path,report):
    with Path(path).open() as file:
        require(json.loads(next(file))==dict(plan=make_plan()),'native SIR plan differs')
        rows=[json.loads(line) for line in file]
    validate_rows(rows)
    observations=exact(rows)
    gates=compare(rows,read_reference()['rows'])
    result=dict(metadata=metadata(),runs_per_engine=len(make_plan()['cases'])*make_plan()['replications'],
                exact_agent_states=observations,gates=gates,passed=all(g['passed'] for g in gates))
    if report:
        Path(report).write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    require(result['passed'],'SIR gates failed: '+str([g for g in gates if not g['passed']]))
    print(f'SIR: {len(gates)} distribution gates, {result["runs_per_engine"]} runs per engine, {observations} exact states passed')


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    for flag in ['write','verify','contract']:
        parser.add_argument('--'+flag,action='store_true')
    parser.add_argument('--native')
    parser.add_argument('--report')
    args=parser.parse_args()
    require((HERE/'sir-plan.json').read_text()==encoded_plan(),'stale SIR plan')
    if args.write:
        REFERENCE.write_text(encode(generate()))
    if args.verify:
        require(encode(generate())==REFERENCE.read_text(),'Mesa SIR reference does not reproduce')
    if args.contract:
        contract()
    if args.native:
        score(args.native,args.report)
