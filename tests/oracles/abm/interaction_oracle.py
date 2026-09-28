"""Independent pinned Mesa/NetworkX dynamic snapshots; offline CLI comparison."""
import argparse
import copy
import csv
import hashlib
import importlib.metadata
import io
import json
import math
from pathlib import Path
import subprocess
import tempfile
from interaction_plan import HERE, encoded, make_plan

def require(b,message):
    if not b: raise ValueError(message)

def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()

def metadata():
    return dict(version=1,plan_sha256=digest(HERE/'interaction-plan.json'),adapter_sha256=digest(Path(__file__)),
                requirements_sha256=digest(HERE/'requirements.txt'),engines={'Mesa':'3.5.1','networkx':'3.7'})

def generate():
    for line in (HERE/'requirements.txt').read_text().splitlines():
        package,version=line.split('==');require(importlib.metadata.version(package)==version,'unpinned '+package)
    from mesa.space import SingleGrid, ContinuousSpace, NetworkGrid
    import networkx as nx
    class Agent:
        def __init__(self,i): self.unique_id,self.pos=i,None
    result=metadata();result['cases']=[]
    for case in make_plan()['cases']:
        model=case['model'];c=model['components'][0];records=copy.deepcopy(c['agents']);spec=c.get('space')
        graph=nx.DiGraph() if c['network']['directed'] else nx.Graph()
        graph.add_nodes_from(range(len(records)));graph.add_edges_from(c['network']['edges'])
        network=NetworkGrid(graph);network_agents=[Agent(i) for i in range(len(records))]
        for a in network_agents: network.place_agent(a,a.unique_id)
        def snapshot():
            if spec is None: return None,[]
            space=SingleGrid(spec['width'],spec['height'],spec['wrap']) if spec['kind']=='grid' else ContinuousSpace(3,3,spec['wrap'],-1,-1)
            agents=[Agent(i) for i in range(len(records))]
            for a,r in zip(agents,records): space.place_agent(a,space.torus_adj((r['x'],r['y'])))
            return space,agents
        def query(q,i,space,agents):
            if q['source']=='space':
                pos=agents[i].pos
                found=space.get_neighbors(pos,q['moore'],q['include_self'],q['radius']) if spec['kind']=='grid' else space.get_neighbors(pos,q['radius'],include_center=True)
                ids=sorted(a.unique_id for a in found if q['include_self'] or a.unique_id!=i)
            elif q['source']=='network': ids=sorted(a.unique_id for a in network.get_neighbors(i,include_center=q.get('include_self',False)))
            else: ids=[j for j in range(len(records)) if q.get('include_self',False) or j!=i]
            if q['op']=='count': return len(ids)
            values=[records[j][q['field']] for j in ids]
            if q['op']=='count_same': return sum(v==records[i][q['field']] for v in values)
            total=sum(values)
            return total/len(values) if q['op']=='mean' and values else total
        rows=[]
        for tick in range(9):
            space,agents=snapshot()
            if tick:
                # Three Jacobi phases, expressed directly rather than interpreting native expressions.
                values=[(r['value']+query(c['queries'][0],i,space,agents))/2 for i,r in enumerate(records)]
                for r,v in zip(records,values): r['value']=v
                for r in records: r['x']+=r['vx']
                space,agents=snapshot()
                values=[r['value']+query(c['queries'][4],i,space,agents)/8 for i,r in enumerate(records)]
                for r,v in zip(records,values): r['value']=v
            for output in model['outputs']:
                i=output['agent']
                value=records[i][output['field']] if 'field' in output else query(next(q for q in c['queries'] if q['id']==output['query']),i,space,agents)
                rows.append([tick,output['id'],value])
        result['cases'].append(dict(id=case['id'],rows=rows))
    return result

def validate(ref):
    require((HERE/'interaction-plan.json').read_text()==encoded(make_plan()),'stale plan')
    require(set(ref)==set(metadata())|{'cases'},'reference fields')
    for k,v in metadata().items(): require(ref[k]==v,'metadata '+k)
    plan=make_plan()['cases'];require(len(ref['cases'])==len(plan),'case count')
    for actual,spec in zip(ref['cases'],plan):
        require(set(actual)=={'id','rows'} and actual['id']==spec['id'],'case identity')
        expected=[(t,o['id']) for t in range(9) for o in spec['model']['outputs']]
        require(len(actual['rows'])==len(expected),'row count')
        for row,key in zip(actual['rows'],expected):
            require(len(row)==3 and tuple(row[:2])==key and type(row[2]) in (int,float) and math.isfinite(row[2]),'invalid observation')

def compare(executable,ref):
    validate(ref);count=0;maximum=0.
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory)/'model.json'
        for spec,expected in zip(make_plan()['cases'],ref['cases']):
            path.write_text(json.dumps(spec['model']))
            run=subprocess.run([str(Path(executable).resolve()),'run',str(path)],capture_output=True,text=True)
            require(run.returncode==0,run.stderr)
            rows=list(csv.DictReader(io.StringIO(run.stdout)))
            require(len(rows)==len(expected['rows']),'native row count')
            for actual,(time,name,value) in zip(rows,expected['rows']):
                require(float(actual['time'])==time and actual['output_id']==name,'native observation key')
                got=float(actual['value']);delta=abs(got-value);maximum=max(maximum,delta)
                require(math.isclose(got,value,rel_tol=2e-12,abs_tol=2e-12),f'{spec["id"]} {time} {name}: {got} != {value}')
                count+=1
    print(f'{len(ref["cases"])} interaction trajectories; {count} Mesa/NetworkX observations; max error {maximum:g}')

def contract(ref):
    validate(ref)
    for mutate in [lambda r:r['cases'].pop(),lambda r:r['cases'][0]['rows'].pop(),lambda r:r.__setitem__('plan_sha256','stale'),lambda r:r['cases'][0]['rows'][0].__setitem__(2,float('nan')),lambda r:r['cases'][0]['rows'][0].__setitem__(1,'wrong')]:
        bad=copy.deepcopy(ref);mutate(bad)
        try: validate(bad)
        except ValueError: continue
        raise ValueError('corrupt reference accepted')
    print('interaction reference contract passed')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--generate',action='store_true');p.add_argument('--verify',action='store_true');p.add_argument('--contract',action='store_true');p.add_argument('--native');a=p.parse_args();path=HERE/'interaction-reference.json'
    if a.generate:
        reference=generate();validate(reference);path.write_text(encoded(reference));print('frozen independent interaction reference')
    else:
        reference=json.loads(path.read_text());validate(reference)
        if a.verify: require(generate()==reference,'regenerated trajectories differ');print('pinned interaction regeneration passed')
        if a.contract: contract(reference)
        if a.native: compare(a.native,reference)
