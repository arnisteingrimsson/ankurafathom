"""Independent birth/death queue, addressed Lindley recurrence, reflected-fluid law."""
import argparse
import bisect
import copy
import csv
import hashlib
import json
import math
from pathlib import Path
import random
import statistics
import sys
from des_fluid_plan import HERE,make_plan,encoded as encoded_plan
REFERENCE=HERE/'des-fluid-reference.json'


def require(v,message):
    if not v:raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata():
    return dict(plan_sha256=digest(HERE/'des-fluid-plan.json'),adapter_sha256=digest(Path(__file__)),
                engine='Python Random direct birth/death CTMC; closed-form reflected fluid')


def encode(value):
    return json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False)+'\n'


def fluid(c,t):
    return max(0.,c['initial_backlog']+(c['arrival_rate']-c['service_rate'])*t)


def generate():
    p=make_plan();rows=[]
    for ci,c in enumerate(p['cases']):
        for ni,n in enumerate(p['scales']):
            for rep in range(p['replications']):
                rng=random.Random(p['reference_seed']+1000003*ci+10007*ni+rep)
                arrivals=int(n*c['initial_backlog']);completed=0;clock=0.
                def schedule():
                    birth=n*c['arrival_rate'];death=n*c['service_rate'] if arrivals>completed else 0
                    rate=birth+death
                    return clock+rng.expovariate(rate),rng.random()*rate<birth
                pending=schedule()
                for t in p['times']:
                    while pending[0]<=t:
                        clock,birth=pending
                        if birth:arrivals+=1
                        else:completed+=1
                        pending=schedule()
                    backlog=arrivals-completed
                    rows.append(dict(case=c['id'],scale=n,replication=rep,time=t,arrivals=arrivals,completed=completed,
                                     waiting=max(0,backlog-1),busy=bool(backlog),stock=backlog))
    return dict(metadata=metadata(),observations=rows)


def validate(rows):
    p=make_plan();expected=[(c,n,r,t) for c in p['cases'] for n in p['scales'] for r in range(p['replications']) for t in p['times']]
    require(len(rows)==len(expected),'DES coverage')
    prior=None
    for row,(c,n,r,t) in zip(rows,expected):
        require(set(row)=={'case','scale','replication','time','arrivals','completed','waiting','busy','stock'},'DES fields')
        require(row['case']==c['id'] and type(row['scale']) is int and row['scale']==n
                and type(row['replication']) is int and row['replication']==r
                and type(row['time']) in (int,float) and row['time']==t,'DES row identity')
        require(all(type(row[k]) is int and row[k]>=0 for k in ['arrivals','completed','waiting']),'DES integer counts')
        require(type(row['busy']) is bool and type(row['stock']) in (int,float) and math.isfinite(row['stock']),'DES stock/busy type')
        a,d,q=row['arrivals'],row['completed'],row['stock']
        require(a>=d and q==a-d==row['waiting']+int(row['busy']) and row['busy']==(q>0),'exact DES/SD stock accounting')
        if t==0:
            require(a==int(n*c['initial_backlog']) and d==0,'DES initial backlog')
        else:
            require(a>=prior['arrivals'] and d>=prior['completed'],'DES counters reversed')
        prior=row


def validate_sd(rows):
    p=make_plan();expected=[(c,dt,t) for c in p['cases'] for dt in p['sd_steps'] for t in p['times']]
    require(len(rows)==len(expected),'fluid SD coverage')
    for row,(c,dt,t) in zip(rows,expected):
        require(set(row)=={'kind','case','dt','time','backlog'} and row['kind']=='sd' and row['case']==c['id']
                and type(row['dt']) in (int,float) and row['dt']==dt and type(row['time']) in (int,float) and row['time']==t,'fluid SD identity')
        require(type(row['backlog']) in (int,float) and math.isfinite(row['backlog']) and row['backlog']>=0,'invalid fluid SD backlog')
        if t==0:require(row['backlog']==c['initial_backlog'],'fluid initial backlog')


def read_reference():
    ref=json.loads(REFERENCE.read_text())
    require(set(ref)=={'metadata','observations'} and ref['metadata']==metadata(),'stale fluid reference')
    validate(ref['observations']);return ref


def key(row):
    return row['case'],row['scale'],row['replication'],row['time']


def ks(a,b):
    a,b=sorted(a),sorted(b)
    return max(abs(bisect.bisect_right(a,x)/len(a)-bisect.bisect_right(b,x)/len(b)) for x in set(a+b))


def exact_counts(rows):
    # Independent arrival/service Philox streams and FIFO Lindley recursion; no event heap or bridges.
    sys.path.insert(0,str(HERE.parents[1]))
    from abm_ir_contract import word
    p=make_plan();n=p['scales'][0];actual={key(r):r for r in rows};observations=0
    for c in p['cases']:
        for rep in [0,p['replications']-1]:
            arrival_times=[];departures=[];time=0.;last=0.;initial=int(n*c['initial_backlog'])
            for entity in range(p['max_entities']):
                def exponential(stream,rate):
                    u=(word(p['seed'],p['scenario'],rep,entity,0,stream)+.5)/2**32
                    return -math.log(u)/rate
                if entity>=initial:
                    time+=exponential(p['arrival_stream'],n*c['arrival_rate'])
                    if time>p['times'][-1]:break
                arrival_times.append(time)
                last=max(last,time)+exponential(p['service_stream'],n*c['service_rate']);departures.append(last)
            else:raise ValueError('independent arrival guard exhausted')
            for t in p['times']:
                a=bisect.bisect_right(arrival_times,t);d=bisect.bisect_right(departures,t)
                row=actual[c['id'],n,rep,t]
                require(row['arrivals']==a and row['completed']==d and row['stock']==a-d,'addressed queue recurrence differs')
                observations+=1
    return observations


def numeric_checks(rows):
    p=make_plan();cases={c['id']:c for c in p['cases']};gates=[]
    for c in p['cases']:
        error=max(abs(r['backlog']-fluid(cases[r['case']],r['time'])) for r in rows if r['case']==c['id'])
        gates.append(dict(case=c['id'],maximum_error=error,passed=error<=p['numeric_tolerance']))
    return gates


def compare(rows,reference,sd=None):
    p=make_plan();nr=p['replications'];policy=p['independent_gates'];limits=p['limit_gates'];times=p['times'][1:]
    actual={key(r):r for r in rows};independent={key(r):r for r in reference['observations']}
    cutoff=math.sqrt(math.log(2*policy['comparisons']/policy['family_alpha'])/nr)
    gates=[];summaries=[];curves=[]
    for c in p['cases']:
        cid=c['id']
        for n in p['scales']:
            bias2=variance=meanvariance=0.;trajectory=[]
            for t in p['times']:
                a=[actual[cid,n,r,t]['stock']/n for r in range(nr)];mean=statistics.mean(a);var=statistics.variance(a);se=math.sqrt(var/nr)
                trajectory.append(dict(time=t,mean=mean,standard_error=se,fluid=fluid(c,t)))
                if t==0:continue
                bias2+=(mean-fluid(c,t))**2/len(times);variance+=var*(nr-1)/nr/len(times);meanvariance+=var/nr/len(times)
                for metric in ['stock','arrivals','completed']:
                    a=[actual[cid,n,r,t][metric]/n for r in range(nr)];b=[independent[cid,n,r,t][metric]/n for r in range(nr)]
                    gap=statistics.mean(a)-statistics.mean(b);pairse=math.sqrt((statistics.variance(a)+statistics.variance(b))/nr)
                    bound=max(policy['mean_floor'],policy['mean_sigma']*pairse);distance=ks(a,b)
                    gates.append(dict(case=cid,scale=n,time=t,metric=metric,mean_gap=gap,mean_limit=bound,ks=distance,ks_limit=cutoff,
                                      difference_ci95=[gap-1.96*pairse,gap+1.96*pairse],passed=abs(gap)<=bound and distance<=cutoff))
            loss=[sum((actual[cid,n,r,t]['stock']/n-fluid(c,t))**2 for t in times)/len(times) for r in range(nr)]
            mse=statistics.mean(loss);msese=math.sqrt(statistics.variance(loss)/nr)
            require(abs(mse-bias2-variance)<1e-12,'fluid MSE decomposition')
            summaries.append(dict(case=cid,scale=n,path_rmse=math.sqrt(mse),ensemble_mean_l2=math.sqrt(bias2),population_spread_rms=math.sqrt(variance),
                                  mean_mc_standard_error_rms=math.sqrt(meanvariance),trajectory=trajectory,
                                  rmse_ci95=[math.sqrt(max(0,mse-1.96*msese)),math.sqrt(mse+1.96*msese)]))
        errors=[r['path_rmse'] for r in summaries if r['case']==cid]
        ratios=[b/a if a else 0 for a,b in zip(errors,errors[1:])];relative=errors[-1]/errors[0] if errors[0] else 0
        curves.append(dict(case=cid,scales=p['scales'],path_rmse=errors,ratios=ratios,finest_coarse_ratio=relative,
                           passed=all(limits['rmse_ratio_min']<=v<=limits['rmse_ratio_max'] for v in ratios)
                           and relative<=limits['finest_coarse_max'] and errors[-1]<=limits['finest_rmse_max']))
    require(len(gates)==policy['comparisons'],'fluid gate count')
    numeric=numeric_checks(sd) if sd is not None else []
    return dict(metadata=metadata(),runs_per_stochastic_engine=len(p['cases'])*len(p['scales'])*nr,
                independent_gates=gates,numeric_gates=numeric,curves=curves,summaries=summaries,
                passed=all(g['passed'] for g in gates+numeric+curves))


def native(path):
    with Path(path).open() as f:
        require(json.loads(next(f))==dict(plan=make_plan()),'native fluid plan differs');rows=[json.loads(line) for line in f]
    require(all(r.get('kind') in ['sd','des'] for r in rows),'unknown fluid output kind')
    sd=[r for r in rows if r['kind']=='sd'];des=[{k:v for k,v in r.items() if k!='kind'} for r in rows if r['kind']=='des']
    validate_sd(sd);validate(des);return sd,des


def verify():
    ref=read_reference();regenerated=generate()
    require(regenerated['metadata']==metadata(),'regenerated fluid metadata');validate(regenerated['observations'])
    require(ref==regenerated,'birth/death reference changed')
    print('Independent birth/death regeneration matches all observations exactly')


def contract():
    ref=read_reference()
    for mutate in [lambda r:r['observations'].pop(),lambda r:r['metadata'].__setitem__('plan_sha256','stale'),
                   lambda r:r['observations'][1].__setitem__('arrivals',True),lambda r:r['observations'][1].__setitem__('busy',1),
                   lambda r:r['observations'][1].__setitem__('stock',-1),lambda r:r['observations'][1].__setitem__('waiting',99999),
                   lambda r:r['observations'][1].__setitem__('time',99)]:
        bad=copy.deepcopy(ref);mutate(bad)
        try:
            require(bad['metadata']==metadata(),'stale metadata');validate(bad['observations'])
        except ValueError:continue
        raise ValueError('corrupt fluid reference accepted')
    # A broken server that never completes still has valid stock/queue accounting.
    broken=copy.deepcopy(ref['observations'])
    for r in broken:
        r['completed']=0;r['stock']=r['arrivals'];r['waiting']=max(0,r['arrivals']-1);r['busy']=bool(r['arrivals'])
    validate(broken);result=compare(broken,ref)
    require(not result['passed'] and all(not r['passed'] for r in result['curves'])
            and any(not r['passed'] for r in result['independent_gates']),'disabled service accepted')
    p=make_plan()
    frozen=[dict(kind='sd',case=c['id'],dt=dt,time=t,backlog=c['initial_backlog']) for c in p['cases'] for dt in p['sd_steps'] for t in p['times']]
    validate_sd(frozen)
    require(any(not g['passed'] for g in numeric_checks(frozen)),'frozen fluid dynamics accepted')
    require(fluid(dict(initial_backlog=1.,arrival_rate=.5,service_rate=1.),1)==.5
            and fluid(dict(initial_backlog=1.,arrival_rate=.5,service_rate=1.),3)==0,'reflection law')
    require(ks([0,0],[1,1])==1 and ks([0,1],[0,1])==0,'KS contract')
    print('Fluid coverage, exact accounting, reflection, corruption and disabled-service controls passed')


def plot(report,directory):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    result=json.loads(Path(report).read_text());require(result['passed'] and result['metadata']==metadata(),'failed/stale fluid figure source')
    out=Path(directory);out.mkdir(parents=True,exist_ok=True);colors=['#2463a7','#bd5c28','#27865b','#875aa7']
    fig,axes=plt.subplots(1,2,figsize=(12,4.8),layout='constrained')
    for c,color in zip(result['curves'],colors):
        points=[r for r in result['summaries'] if r['case']==c['case']];n=[r['scale'] for r in points]
        axes[0].loglog(n,[r['path_rmse'] for r in points],'o-',color=color,label=c['case'])
        axes[0].fill_between(n,[r['rmse_ci95'][0] for r in points],[r['rmse_ci95'][1] for r in points],color=color,alpha=.12)
        axes[1].loglog(n,[r['ensemble_mean_l2'] for r in points],'o-',color=color,label=c['case'])
    axes[0].set(title='Queue path convergence',ylabel='RMSE of backlog / N to reflected fluid')
    axes[1].set(title='Ensemble-mean gap',ylabel='L2 gap to fluid (sampling included)')
    for ax in axes:
        ax.set_xlabel('Rate and population scale N');ax.set_xticks(make_plan()['scales'],labels=[str(n) for n in make_plan()['scales']]);ax.grid(True,which='both',alpha=.2)
    axes[0].legend(fontsize=9);fig.suptitle('DES → SD fluid limit | 256 replications per point',fontsize=13)
    for ext in ['svg','png']:fig.savefig(out/f'des-fluid-convergence.{ext}',dpi=170,metadata={'Date':None} if ext=='svg' else None)
    plt.close(fig)
    fig,axes=plt.subplots(2,2,figsize=(10,7),layout='constrained')
    for ax,c,color in zip(axes.flat,make_plan()['cases'],colors):
        rows=next(r for r in result['summaries'] if r['case']==c['id'] and r['scale']==make_plan()['scales'][-1])['trajectory']
        ax.plot([r['time'] for r in rows],[r['fluid'] for r in rows],color=color,label='Reflected fluid SD')
        ax.errorbar([r['time'] for r in rows],[r['mean'] for r in rows],yerr=[1.96*r['standard_error'] for r in rows],fmt='o',color=color,ms=3,capsize=2,label='DES mean ± 1.96 SE')
        ax.set(title=c['id'],xlabel='Time',ylabel='Normalized backlog');ax.grid(alpha=.2);ax.legend(fontsize=8)
    fig.suptitle('N=128: integer pulse stock / N and independent fluid trajectory',fontsize=12)
    for ext in ['svg','png']:fig.savefig(out/f'des-fluid-trajectories.{ext}',dpi=170,metadata={'Date':None} if ext=='svg' else None)
    plt.close(fig)
    fields=['case','scale','path_rmse','ensemble_mean_l2','population_spread_rms','mean_mc_standard_error_rms']
    with (out/'des-fluid-gaps.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader()
        for r in result['summaries']:writer.writerow({k:r[k] for k in fields})
    (out/'des-fluid-provenance.json').write_text(json.dumps(dict(metadata=result['metadata'],report_sha256=digest(Path(report)),matplotlib=matplotlib.__version__),indent=2)+'\n')
    print('Fluid figures, numeric gaps and provenance written to',out)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    for flag in ['write','verify','contract']:parser.add_argument('--'+flag,action='store_true')
    parser.add_argument('--native');parser.add_argument('--report');parser.add_argument('--plot');args=parser.parse_args()
    require((HERE/'des-fluid-plan.json').read_text()==encoded_plan(),'stale fluid plan')
    if args.write:REFERENCE.write_text(encode(generate()))
    if args.verify:verify()
    if args.contract:contract()
    if args.native:
        sd,rows=native(args.native);result=compare(rows,read_reference(),sd);result['exact_count_snapshots']=exact_counts(rows)
        if args.report:Path(args.report).write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
        require(result['passed'],'DES fluid gates failed: '+str([g for k in ['independent_gates','numeric_gates','curves'] for g in result[k] if not g['passed']]))
        print('DES fluid: 180 independent comparisons, four scaling curves, four native SD checks passed;',result['exact_count_snapshots'],'exact addressed snapshots')
    if args.plot:
        require(args.report,'plot needs report');plot(args.report,args.plot)
