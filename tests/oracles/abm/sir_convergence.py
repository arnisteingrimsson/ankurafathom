"""Full joint-state SIR convergence, independent of native simulation engines."""
import argparse
import itertools
import json
import math
from pathlib import Path
from sir_async_plan import HERE, make_plan


def require(v,message):
    if not v:
        raise ValueError(message)


def code(states):
    value=0
    for s in states:
        value=3*value+s
    return value


def kernels(case,dt=None):
    states=list(itertools.product(range(3),repeat=len(case['states'])))
    rows=[]
    for state in states:
        hazards=[]
        for i,s in enumerate(state):
            contacts=sum(state[b if a==i else a]==1 for a,b in case['edges'] if a==i or b==i)
            hazards.append(case['infection_rate']*contacts if s==0 else case['recovery_rate'] if s==1 else 0)
        if dt is None:
            row={code(state):-sum(hazards)}
            for i,rate in enumerate(hazards):
                if rate:
                    target=list(state); target[i]+=1
                    row[code(target)]=rate
        else:
            branches=[(list(state),1.)]
            for i,rate in enumerate(hazards):
                probability=-math.expm1(-rate*dt)
                next_branches=[]
                for target,mass in branches:
                    if probability<1:
                        next_branches.append((target,mass*(1-probability)))
                    if probability>0:
                        changed=list(target); changed[i]+=1
                        next_branches.append((changed,mass*probability))
                branches=next_branches
            row={code(target):mass for target,mass in branches}
        rows.append(row)
    return rows


def multiply(vector,rows):
    result=[0.]*len(vector)
    for mass,row in zip(vector,rows):
        if mass:
            for j,p in row.items():
                result[j]+=mass*p
    return result


def distribution(case,time,dt=0):
    vector=[0.]*(3**len(case['states']))
    vector[code(case['states'])]=1.
    if dt:
        require(time/dt==int(time/dt),'unaligned exact sync horizon')
        rows=kernels(case,dt)
        for _ in range(int(time/dt)):
            vector=multiply(vector,rows)
        return vector
    generator=kernels(case)
    rate=max(-row.get(i,0) for i,row in enumerate(generator))
    if rate==0:
        return vector
    rows=[]
    for i,row in enumerate(generator):
        converted={j:value/rate for j,value in row.items()}
        converted[i]=converted.get(i,0)+1
        rows.append(converted)
    weight=math.exp(-rate*time)
    result=[v*weight for v in vector]
    total=weight
    for k in range(1,10000):
        vector=multiply(vector,rows)
        weight*=rate*time/k
        result=[a+weight*b for a,b in zip(result,vector)]
        total+=weight
        if total>=1-make_plan()['convergence']['uniformization_tail']:
            return result
    raise ValueError('uniformization failed to bound its tail')


def pinned():
    # Independent matrix exponential/powers, rather than Poisson uniformization.
    import numpy as np
    from scipy.linalg import expm
    cp=make_plan()['convergence']
    rows=[]
    for c in cp['cases']:
        size=3**len(c['states'])
        def matrix(sparse):
            result=np.zeros((size,size))
            for i,row in enumerate(sparse):
                for j,p in row.items():
                    result[i,j]=p
            return result
        q=matrix(kernels(c))
        for t in cp['times']:
            rows.append(dict(case=c['id'],time=t,dt=0,probabilities=expm(q*t)[code(c['states'])].tolist()))
            for dt in cp['steps']:
                rows.append(dict(case=c['id'],time=t,dt=dt,
                                 probabilities=np.linalg.matrix_power(matrix(kernels(c,dt)),int(t/dt))[code(c['states'])].tolist()))
    return rows


def tv(a,b):
    return .5*sum(abs(x-y) for x,y in zip(a,b))


def cdf_distance(a,b):
    aa=bb=distance=0.
    for x,y in zip(a,b):
        aa+=x; bb+=y
        distance=max(distance,abs(aa-bb))
    return distance


def exact_evidence(reference):
    cp=make_plan()['convergence']
    expected=[(c,t,dt) for c in cp['cases'] for t in cp['times'] for dt in [0,*cp['steps']]]
    require(len(reference)==len(expected),'joint reference coverage')
    lookup={}
    for row,(c,t,dt) in zip(reference,expected):
        require(set(row)=={'case','time','dt','probabilities'} and (row['case'],row['time'],row['dt'])==(c['id'],t,dt),'joint reference identity')
        values=row['probabilities']
        require(len(values)==3**len(c['states']) and all(type(v) in (int,float) and math.isfinite(v) and v>=-1e-14 for v in values)
                and abs(sum(values)-1)<1e-12,'joint probability simplex')
        independent=distribution(c,t,dt)
        require(max(abs(a-b) for a,b in zip(values,independent))<1e-12,'SciPy/independent probability mismatch')
        lookup[c['id'],t,dt]=values
    curves=[]
    for c in cp['cases']:
        for t in cp['times']:
            errors=[tv(lookup[c['id'],t,dt],lookup[c['id'],t,0]) for dt in cp['steps']]
            ratio=errors[-1]/errors[-2]
            passed=(all(b<a for a,b in zip(errors,errors[1:])) and errors[-1]<=cp['finest_tv_limit']
                    and errors[-1]/errors[0]<=cp['finest_coarse_ratio_limit']
                    and cp['last_ratio_min']<=ratio<=cp['last_ratio_max'])
            curves.append(dict(case=c['id'],time=t,steps=cp['steps'],total_variation=errors,last_ratio=ratio,passed=passed))
    return lookup,curves


def score(rows,reference):
    cp=make_plan()['convergence']
    lookup,curves=exact_evidence(reference)
    expected=[(c,r,dt,t) for c in cp['cases'] for r in range(cp['replications']) for dt in [0,*cp['steps']] for t in cp['times']]
    require(len(rows)==len(expected),'native convergence row count')
    samples={}
    previous=None
    from sir_oracle import validate_states,reachable
    reach={c['id']:reachable(c) for c in cp['cases']}
    for row,(c,r,dt,t) in zip(rows,expected):
        require(set(row)=={'case','replication','time','dt','states'} and row['case']==c['id']
                and type(row['replication']) is int and row['replication']==r
                and type(row['time']) in (int,float) and row['time']==t
                and type(row['dt']) in (int,float) and row['dt']==dt,'native convergence identity')
        validate_states(c,row['states'])
        require(all(s==0 for i,s in enumerate(row['states']) if c['states'][i]==0 and i not in reach[c['id']]),'unreachable convergence infection')
        if t!=cp['times'][0]:
            require(all(a<=b for a,b in zip(previous,row['states'])),'reverse convergence history')
        previous=row['states']
        samples.setdefault((c['id'],t,dt),[]).append(code(row['states']))
    n,alpha,count=cp['replications'],cp['family_alpha'],cp['joint_comparisons']
    ks_limit=math.sqrt(math.log(2*count/alpha)/(2*n))
    gates=[]; empirical={}
    for key,values in samples.items():
        exact=lookup[key]
        freq=[0.]*len(exact)
        for i in values:
            freq[i]+=1/n
        empirical[key]=freq
        # Union bound over subsets gives P(TV>eps) <= 2^K exp(-2*n*eps^2).
        tv_limit=math.sqrt((len(exact)*math.log(2)+math.log(count/alpha))/(2*n))
        distance=cdf_distance(freq,exact)
        variation=tv(freq,exact)
        gates.append(dict(case=key[0],time=key[1],dt=key[2],ks=distance,ks_limit=ks_limit,
                          total_variation=variation,tv_limit=tv_limit,passed=distance<=ks_limit and variation<=tv_limit))
    require(len(gates)==count,'joint gate count')
    pairs=[]
    noise=math.sqrt(2*math.log(4*cp['pair_comparisons']/alpha)/n)
    fine=cp['steps'][-1]
    for c in cp['cases']:
        for t in cp['times']:
            distance=cdf_distance(empirical[c['id'],t,fine],empirical[c['id'],t,0])
            # Two one-sample DKW bounds plus known discretization bias; pairing is allowed.
            limit=noise+tv(lookup[c['id'],t,fine],lookup[c['id'],t,0])
            pairs.append(dict(case=c['id'],time=t,ks=distance,limit=limit,passed=distance<=limit))
    return dict(exact_curves=curves,joint_gates=gates,pair_gates=pairs,
                native_runs=len(cp['cases'])*n*(len(cp['steps'])+1),
                passed=all(g['passed'] for g in curves+gates+pairs))


def contract():
    from sir_async_oracle import read_reference
    reference=read_reference()['convergence']
    lookup,curves=exact_evidence(reference)
    require(all(c['passed'] for c in curves),'predeclared exact convergence gates failed')
    # Two-agent irreversible infection only: exact exponential waiting distribution.
    c=dict(states=[1,0],edges=[[0,1]],infection_rate=.7,recovery_rate=0)
    p=distribution(c,2)
    require(abs(p[code([1,0])]-math.exp(-1.4))<1e-13 and abs(p[code([1,1])]-(1-math.exp(-1.4)))<1e-13,'hand CTMC law')
    import copy
    bad=copy.deepcopy(reference)
    bad[0]['probabilities'][0]+=.1
    try:
        exact_evidence(bad)
    except ValueError:
        pass
    else:
        raise ValueError('corrupt joint law accepted')
    cp=make_plan()['convergence']
    frozen=[dict(case=c['id'],replication=r,time=t,dt=dt,states=c['states'])
            for c in cp['cases'] for r in range(cp['replications']) for dt in [0,*cp['steps']] for t in cp['times']]
    require(not score(frozen,reference)['passed'],'frozen dynamics passed joint law gates')
    # A non-refining implementation has identical coarse/fine error and must fail.
    require(not (1<=cp['finest_coarse_ratio_limit'] and cp['last_ratio_min']<=1<=cp['last_ratio_max']), 'non-refining curve accepted')
    print('SIR exact joint laws, refinement gates, corruption and wrong-dynamics contracts passed')


if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--contract',action='store_true'); p.add_argument('--native'); p.add_argument('--report')
    args=p.parse_args()
    if args.contract:
        contract()
    if args.native:
        from sir_async_oracle import read_reference
        with Path(args.native).open() as file:
            require(json.loads(next(file))==dict(plan=make_plan()),'native convergence plan differs')
            rows=[json.loads(line) for line in file]
        result=score(rows,read_reference()['convergence'])
        if args.report:
            Path(args.report).write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
        require(result['passed'],'SIR convergence gates failed: '+str([g for group in ['exact_curves','joint_gates','pair_gates'] for g in result[group] if not g['passed']]))
        print('SIR convergence: 6 exact curves, 36 native joint-law gates and 6 fine-sync/async gates passed')
