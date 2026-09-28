"""NetworkX reference regeneration and dependency-free native distribution gates."""
import argparse,bisect,hashlib,json,math,statistics
from pathlib import Path
from generator_plan import make_plan
HERE=Path(__file__).resolve().parent
REFERENCE=HERE/'generator-reference.json'

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def metadata():return dict(plan_sha256=digest(HERE/'generator-plan.json'),adapter_sha256=digest(Path(__file__)),requirements_sha256=digest(HERE/'requirements.txt'),networkx='3.7')
def encode(value):return json.dumps(value,indent=2,sort_keys=True,allow_nan=False)+'\n'
def invariants(case,edges,metrics):
    n=case['n'];assert all(math.isfinite(v) and 0<=v<=1 for v in metrics.values())
    if case['kind']=='watts_strogatz':assert edges==n*case['degree']//2
    if case['kind']=='barabasi_albert':assert edges==case['m']*(n-case['m']) and metrics['giant_fraction']==1

def reference():
    import networkx as nx
    assert nx.__version__=='3.7','reference NetworkX version differs'
    plan=make_plan();rows={}
    for index,c in enumerate(plan['cases']):
        values=[];n=c['n']
        for r in range(plan['replications']):
            seed=plan['reference_seed']+1000003*index+r
            if c['kind']=='erdos_renyi':g=nx.gnp_random_graph(n,c['probability'],seed=seed)
            elif c['kind']=='watts_strogatz':g=nx.watts_strogatz_graph(n,c['degree'],c['probability'],seed=seed)
            else:g=nx.barabasi_albert_graph(n,c['m'],seed=seed)
            degrees=[d for _,d in g.degree()]
            metrics=dict(density=nx.density(g),max_degree=max(degrees)/(n-1),degree_second_moment=sum(d*d for d in degrees)/(n*(n-1)**2),transitivity=nx.transitivity(g),giant_fraction=max(map(len,nx.connected_components(g)))/n)
            invariants(c,g.number_of_edges(),metrics);values.append(dict(edges=g.number_of_edges(),metrics=metrics))
        rows[c['id']]=values
    return dict(metadata=metadata(),rows=rows)

def read_reference():
    value=json.loads(REFERENCE.read_text());assert value['metadata']==metadata(),'stale generator reference metadata'
    plan=make_plan();assert set(value['rows'])=={c['id'] for c in plan['cases']}
    for c in plan['cases']:
        rows=value['rows'][c['id']];assert len(rows)==plan['replications']
        for row in rows:
            assert set(row)=={'edges','metrics'} and set(row['metrics'])==set(plan['metrics'])
            assert isinstance(row['edges'],int) and 0<=row['edges']<=c['n']*(c['n']-1)//2
            assert row['metrics']['density']==row['edges']/(c['n']*(c['n']-1)/2)
            invariants(c,row['edges'],row['metrics'])
    return value

def native_metrics(n,edges):
    adjacent=[set() for _ in range(n)];seen=set()
    for a,b in edges:
        assert isinstance(a,int) and isinstance(b,int) and 0<=a<b<n and (a,b) not in seen,'invalid native graph'
        seen.add((a,b));adjacent[a].add(b);adjacent[b].add(a)
    degree=[len(a) for a in adjacent];triples=sum(d*(d-1)//2 for d in degree)
    # Count each triangle once at its lowest ordered edge.
    triangles=sum(sum(w>v for w in adjacent[u]&adjacent[v]) for u,v in seen)
    remaining=set(range(n));largest=0
    while remaining:
        stack=[remaining.pop()];size=0
        while stack:
            u=stack.pop();size+=1;new=adjacent[u]&remaining;remaining-=new;stack.extend(new)
        largest=max(largest,size)
    return dict(density=len(edges)/(n*(n-1)/2),max_degree=max(degree)/(n-1),degree_second_moment=sum(d*d for d in degree)/(n*(n-1)**2),transitivity=3*triangles/triples if triples else 0,giant_fraction=largest/n)

def ks(a,b):
    a=sorted(a);b=sorted(b)
    return max(abs(bisect.bisect_right(a,x)/len(a)-bisect.bisect_right(b,x)/len(b)) for x in set(a+b))

def score(native_path,report_path):
    plan=make_plan();ref=read_reference();native={c['id']:[] for c in plan['cases']}
    with Path(native_path).open() as f:
        assert json.loads(next(f))==dict(plan=plan),'native report plan differs'
        for c in plan['cases']:
            for r in range(plan['replications']):
                row=json.loads(next(f));assert set(row)=={'case','replication','edges'} and row['case']==c['id'] and row['replication']==r,'native graph identity/order differs'
                metrics=native_metrics(c['n'],row['edges']);invariants(c,len(row['edges']),metrics);native[c['id']].append(dict(edges=len(row['edges']),metrics=metrics))
        assert not f.read().strip(),'extra native graphs'
    gates=[];r=plan['replications'];policy=plan['gates'];critical=math.sqrt(math.log(2*policy['ks_tests']/policy['family_alpha'])/r)
    for c in plan['cases']:
        for metric in plan['metrics']:
            a=[v['metrics'][metric] for v in native[c['id']]];b=[v['metrics'][metric] for v in ref['rows'][c['id']]]
            gap=abs(statistics.mean(a)-statistics.mean(b));limit=max(policy['mean_floor'][metric],policy['mean_sigma']*math.sqrt((statistics.variance(a)+statistics.variance(b))/r));distance=ks(a,b)
            gates.append(dict(case=c['id'],metric=metric,mean_gap=gap,mean_limit=limit,ks=distance,ks_limit=critical,passed=gap<=limit and distance<=critical))
        if c['kind']=='erdos_renyi':
            pairs=c['n']*(c['n']-1)/2;mean=pairs*c['probability'];variance=mean*(1-c['probability'])
            for label,rows in [('native',native[c['id']]),('reference',ref['rows'][c['id']])]:
                samples=[v['edges'] for v in rows];gap=abs(statistics.mean(samples)-mean);limit=policy['er_mean_sigma']*math.sqrt(variance/r);relative=abs(statistics.variance(samples)/variance-1)
                gates.append(dict(case=c['id'],metric='edge_binomial_'+label,mean_gap=gap,mean_limit=limit,variance_relative_error=relative,variance_limit=policy['er_variance_relative_tolerance'],passed=gap<=limit and relative<=policy['er_variance_relative_tolerance']))
    result=dict(metadata=metadata(),graphs_per_engine=r*len(plan['cases']),gates=gates,passed=all(g['passed'] for g in gates))
    if report_path:Path(report_path).write_text(encode(result))
    failures=[g for g in gates if not g['passed']];assert not failures,failures
    print(f'graph distributions: {len(gates)} comparison/analytic gates passed over {result["graphs_per_engine"]} graphs per engine')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--write',action='store_true');p.add_argument('--verify',action='store_true');p.add_argument('--contract',action='store_true');p.add_argument('--native');p.add_argument('--report');args=p.parse_args()
    assert json.loads((HERE/'generator-plan.json').read_text())==make_plan(),'plan differs'
    if args.write:REFERENCE.write_text(encode(reference()))
    if args.verify:assert encode(reference())==REFERENCE.read_text(),'pinned generator reference does not reproduce'
    if args.contract:read_reference();assert ks([0,0],[1,1])==1 and ks([0,1],[0,1])==0;assert native_metrics(3,[[0,1],[0,2],[1,2]])['transitivity']==1;assert native_metrics(3,[])['giant_fraction']==1/3;print('generator reference contract passed')
    if args.native:score(args.native,args.report)
