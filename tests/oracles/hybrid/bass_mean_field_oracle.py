"""Independent binomial chain and closed-form Bass/SD convergence evidence."""
import argparse
import bisect
import copy
import csv
import hashlib
import importlib.metadata
import json
import math
from pathlib import Path
import statistics
import sys
from bass_mean_field_plan import HERE,make_plan,encoded as encoded_plan
REFERENCE=HERE/'bass-mean-field-reference.json'


def require(v,message):
    if not v:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata():
    return dict(plan_sha256=digest(HERE/'bass-mean-field-plan.json'),adapter_sha256=digest(Path(__file__)),
                requirements_sha256=digest(HERE/'requirements.txt'),engines=dict(numpy='2.5.3',scipy='1.18.1'))


def encode(value):
    return json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False)+'\n'


def closed(c,t):
    p,q,a=c['p'],c['q'],c['initial_fraction']
    if t==0 or a==1 or p+q==0 or (p==0 and a==0):
        return a
    e=math.exp(-(p+q)*t)
    return (p+q*a-p*(1-a)*e)/(p+q*a+q*(1-a)*e)


def euler(c,dt):
    a=c['initial_fraction']; tick=0; result={}
    for t in make_plan()['times']:
        while tick*dt<t:
            a+=dt*(c['p']+c['q']*a)*(1-a);tick+=1
        require(tick*dt==t,'unaligned Euler reference')
        result[t]=a
    return result


def generate():
    for line in (HERE/'requirements.txt').read_text().splitlines():
        package,version=line.split('==')
        require(importlib.metadata.version(package)==version,'unpinned '+package)
    import numpy as np
    from scipy.integrate import solve_ivp
    p=make_plan(); continuous=[]; abm=[]
    for ci,c in enumerate(p['cases']):
        sol=solve_ivp(lambda t,y:[(c['p']+c['q']*y[0])*(1-y[0])],
                      [0,p['times'][-1]],[c['initial_fraction']],t_eval=p['times'],method='DOP853',rtol=1e-12,atol=1e-14)
        require(sol.success,'SciPy Bass failed')
        for t,y in zip(p['times'],sol.y[0]):
            require(abs(y-closed(c,t))<=p['numeric_gates']['reference_tolerance'],'closed-form/SciPy disagreement')
            continuous.append(dict(case=c['id'],time=t,fraction=float(y)))
        for di,dt in enumerate(p['ensemble_steps']):
            for ni,n in enumerate(p['populations']):
                for rep in range(p['replications']):
                    rng=np.random.Generator(np.random.PCG64(p['reference_seed']+1000003*ci+100003*di+10007*ni+rep))
                    count=int(n*c['initial_fraction']);tick=0
                    for t in p['times']:
                        while tick*dt<t:
                            probability=dt*(c['p']+c['q']*(count/n))
                            count+=int(rng.binomial(n-count,probability));tick+=1
                        abm.append(dict(case=c['id'],dt=dt,population=n,replication=rep,time=t,adopted=count,ticks=tick))
    return dict(metadata=metadata(),continuous=continuous,abm=abm)


def validate_continuous(rows):
    p=make_plan();expected=[(c,t) for c in p['cases'] for t in p['times']]
    require(len(rows)==len(expected),'continuous coverage')
    for row,(c,t) in zip(rows,expected):
        require(set(row)=={'case','time','fraction'} and row['case']==c['id']
                and type(row['time']) in (int,float) and row['time']==t,'continuous identity')
        require(type(row['fraction']) in (int,float) and math.isfinite(row['fraction'])
                and abs(row['fraction']-closed(c,t))<=p['numeric_gates']['reference_tolerance'],'continuous closed-form discrepancy')


def validate_sd(rows):
    p=make_plan();expected=[(c,dt,m,t) for c in p['cases'] for dt in p['sd_steps'] for m in ['euler','rk4'] for t in p['times']]
    require(len(rows)==len(expected),'native SD coverage')
    prior=0
    for row,(c,dt,m,t) in zip(rows,expected):
        require(set(row)=={'kind','case','dt','method','time','fraction'} and row['kind']=='sd'
                and row['case']==c['id'] and row['method']==m and type(row['dt']) in (int,float) and row['dt']==dt
                and type(row['time']) in (int,float) and row['time']==t,'native SD identity')
        a=row['fraction']
        require(type(a) in (int,float) and math.isfinite(a) and 0<=a<=1,'SD fraction domain')
        require(a==c['initial_fraction'] if t==0 else a>=prior,'SD initial/irreversible adoption')
        prior=a


def validate_abm(rows):
    p=make_plan();expected=[(c,dt,n,r,t) for c in p['cases'] for dt in p['ensemble_steps']
                           for n in p['populations'] for r in range(p['replications']) for t in p['times']]
    require(len(rows)==len(expected),'ABM coverage')
    prior=0
    for row,(c,dt,n,r,t) in zip(rows,expected):
        require(set(row)=={'case','dt','population','replication','time','adopted','ticks'},'ABM fields')
        require(row['case']==c['id'] and type(row['dt']) in (int,float) and row['dt']==dt
                and type(row['population']) is int and row['population']==n
                and type(row['replication']) is int and row['replication']==r
                and type(row['time']) in (int,float) and row['time']==t,'ABM identity')
        require(type(row['ticks']) is int and row['ticks']*dt==t,'ABM tick clock')
        a=row['adopted']
        require(type(a) is int and 0<=a<=n,'ABM exact population accounting')
        require(a==int(n*c['initial_fraction']) if t==0 else a>=prior,'ABM initial/irreversible adoption')
        prior=a


def read_reference():
    ref=json.loads(REFERENCE.read_text())
    require(set(ref)=={'metadata','continuous','abm'} and ref['metadata']==metadata(),'stale Bass reference')
    validate_continuous(ref['continuous']);validate_abm(ref['abm'])
    return ref


def ks(a,b):
    a,b=sorted(a),sorted(b)
    return max(abs(bisect.bisect_right(a,x)/len(a)-bisect.bisect_right(b,x)/len(b)) for x in set(a+b))


def key(row):
    return row['case'],row['dt'],row['population'],row['replication'],row['time']


def exact_counts(rows):
    sys.path.insert(0,str(HERE.parents[1]))
    from abm_ir_contract import word
    p=make_plan();actual={key(r):r['adopted'] for r in rows};n=p['populations'][0];observations=0
    for c in p['cases']:
        for dt in p['ensemble_steps']:
            for rep in [0,p['replications']-1]:
                states=[1]*int(n*c['initial_fraction'])+[0]*(n-int(n*c['initial_fraction']));tick=0
                for t in p['times']:
                    while tick*dt<t:
                        probability=dt*(c['p']+c['q']*(sum(states)/n))
                        states=[int(old or (word(p['seed'],p['scenario'],rep,i,tick,p['stream'])+.5)/2**32<probability)
                                for i,old in enumerate(states)]
                        tick+=1
                    require(sum(states)==actual[c['id'],dt,n,rep,t],'addressed Bass count differs')
                    observations+=1
    return observations


def numeric_checks(sd):
    p=make_plan();policy=p['numeric_gates'];checks=[];curves=[]
    actual={(r['case'],r['dt'],r['method'],r['time']):r['fraction'] for r in sd}
    for c in p['cases']:
        errors={m:[] for m in ['euler','rk4']};agreement=0
        for dt in p['sd_steps']:
            discrete=euler(c,dt)
            for m in errors:
                errors[m].append(max(abs(actual[c['id'],dt,m,t]-closed(c,t)) for t in p['times']))
            agreement=max(agreement,*[abs(actual[c['id'],dt,'euler',t]-discrete[t]) for t in p['times']])
        ratios=[b/a if a else 0 for a,b in zip(errors['euler'],errors['euler'][1:])]
        rk=errors['rk4'][-2]/errors['rk4'][-1] if errors['rk4'][-1] else 0
        passed=(agreement<=policy['euler_agreement'] and errors['euler'][-1]<=policy['finest_euler_error']
                and all(policy['euler_halving_ratio'][0]<=x<=policy['euler_halving_ratio'][1] for x in ratios)
                and errors['rk4'][-1]<=policy['rk4_finest_error'] and policy['rk4_halving_ratio'][0]<=rk<=policy['rk4_halving_ratio'][1])
        checks.append(dict(case=c['id'],euler_agreement=agreement,euler_halving_ratios=ratios,rk4_halving_ratio=rk,passed=passed))
        curves.append(dict(case=c['id'],steps=p['sd_steps'],euler_errors=errors['euler'],rk4_errors=errors['rk4']))
    return checks,curves


def compare(abm,reference,sd=None):
    p=make_plan();nr=p['replications'];times=p['times'][1:];actual={key(r):r['adopted'] for r in abm}
    independent={key(r):r['adopted'] for r in reference['abm']}
    policy=p['independent_gates'];limits=p['limit_gates'];analytic=p['analytic_gates']
    cutoff=math.sqrt(math.log(2*policy['comparisons']/policy['family_alpha'])/nr)
    gates=[];mean_gates=[];analytic_gates=[];summaries=[];curves=[]
    for c in p['cases']:
        cid=c['id']
        for dt in p['ensemble_steps']:
            target=euler(c,dt)
            for n in p['populations']:
                bias2=variance=meanvariance=continuous_bias2=0.;trajectory=[]
                for t in p['times']:
                    a=[actual[cid,dt,n,r,t]/n for r in range(nr)]
                    mean=statistics.mean(a);var=statistics.variance(a);se=math.sqrt(var/nr)
                    trajectory.append(dict(time=t,mean=mean,standard_error=se,discrete_sd=target[t],continuous_sd=closed(c,t)))
                    if t==0:continue
                    bias2+=(mean-target[t])**2/len(times)
                    variance+=var*(nr-1)/nr/len(times);meanvariance+=var/nr/len(times)
                    continuous_bias2+=(mean-closed(c,t))**2/len(times)
                    b=[independent[cid,dt,n,r,t]/n for r in range(nr)]
                    gap=mean-statistics.mean(b);pairse=math.sqrt((var+statistics.variance(b))/nr)
                    bound=max(policy['mean_floor'],policy['mean_sigma']*pairse);distance=ks(a,b)
                    gates.append(dict(case=cid,dt=dt,population=n,time=t,mean_gap=gap,mean_limit=bound,ks=distance,ks_limit=cutoff,
                                      difference_ci95=[gap-1.96*pairse,gap+1.96*pairse],passed=abs(gap)<=bound and distance<=cutoff))
                    if n==p['populations'][-1]:
                        bound=limits['mean_floor']+limits['mean_sigma']*se
                        numerical=abs(target[t]-closed(c,t));gap=mean-target[t];continuous_gap=mean-closed(c,t)
                        mean_gates.append(dict(case=cid,dt=dt,time=t,discrete_gap=gap,continuous_gap=continuous_gap,
                                               numeric_error=numerical,standard_error=se,discrete_limit=bound,continuous_limit=bound+numerical,
                                               passed=abs(gap)<=bound and abs(continuous_gap)<=bound+numerical))
                    if c['q']==0 and c['initial_fraction']==0:
                        probability=1-(1-c['p']*dt)**int(t/dt)
                        exactvar=probability*(1-probability)/n
                        fourth=3*exactvar**2+probability*(1-probability)*(1-6*probability*(1-probability))/n**3
                        varse=math.sqrt(max(0,(fourth-(nr-3)/(nr-1)*exactvar**2)/nr))
                        meanlimit=analytic['mean_sigma']*math.sqrt(exactvar/nr)
                        varlimit=max(analytic['variance_floor'],analytic['variance_sigma']*varse)
                        analytic_gates.append(dict(case=cid,dt=dt,population=n,time=t,mean_gap=mean-probability,
                                                   variance_gap=var-exactvar,mean_limit=meanlimit,variance_limit=varlimit,
                                                   passed=abs(mean-probability)<=meanlimit and abs(var-exactvar)<=varlimit))
                losses=[sum((actual[cid,dt,n,r,t]/n-target[t])**2 for t in times)/len(times) for r in range(nr)]
                mse=statistics.mean(losses);msese=math.sqrt(statistics.variance(losses)/nr)
                require(abs(mse-bias2-variance)<1e-13,'Bass MSE decomposition')
                summaries.append(dict(case=cid,dt=dt,population=n,path_rmse=math.sqrt(mse),ensemble_mean_l2=math.sqrt(bias2),
                                      continuous_mean_l2=math.sqrt(continuous_bias2),population_spread_rms=math.sqrt(variance),
                                      mean_mc_standard_error_rms=math.sqrt(meanvariance),
                                      numeric_bias_l2=math.sqrt(sum((target[t]-closed(c,t))**2 for t in times)/len(times)),
                                      rmse_ci95=[math.sqrt(max(0,mse-1.96*msese)),math.sqrt(mse+1.96*msese)],trajectory=trajectory))
            errors=[r['path_rmse'] for r in summaries if r['case']==cid and r['dt']==dt]
            ratios=[b/a if a else 0 for a,b in zip(errors,errors[1:])]
            relative=errors[-1]/errors[0] if errors[0] else 0
            curves.append(dict(case=cid,dt=dt,populations=p['populations'],path_rmse=errors,ratios=ratios,finest_coarse_ratio=relative,
                               passed=all(limits['rmse_ratio_min']<=x<=limits['rmse_ratio_max'] for x in ratios) and relative<=limits['finest_coarse_max']))
    require(len(gates)==policy['comparisons'] and len(mean_gates)==limits['mean_comparisons']
            and len(analytic_gates)==analytic['comparisons'],'Bass gate coverage')
    numeric,refinement=numeric_checks(sd) if sd is not None else ([],[])
    return dict(metadata=metadata(),runs_per_stochastic_engine=len(p['cases'])*len(p['ensemble_steps'])*len(p['populations'])*nr,
                independent_gates=gates,mean_field_gates=mean_gates,analytic_gates=analytic_gates,numeric_gates=numeric,
                curves=curves,refinement=refinement,summaries=summaries,
                passed=all(g['passed'] for g in gates+mean_gates+analytic_gates+numeric+curves))


def native(path):
    with Path(path).open() as f:
        require(json.loads(next(f))==dict(plan=make_plan()),'native Bass plan differs')
        rows=[json.loads(line) for line in f]
    require(all(r.get('kind') in ['sd','abm'] for r in rows),'unknown native row')
    sd=[r for r in rows if r['kind']=='sd'];abm=[{k:v for k,v in r.items() if k!='kind'} for r in rows if r['kind']=='abm']
    validate_sd(sd);validate_abm(abm)
    return sd,abm


def verify():
    a=read_reference();b=generate()
    require(b['metadata']==metadata(),'regenerated metadata');validate_continuous(b['continuous']);validate_abm(b['abm'])
    require(a['abm']==b['abm'],'binomial reference changed')
    maximum=max(abs(x['fraction']-y['fraction']) for x,y in zip(a['continuous'],b['continuous']))
    require(maximum<=make_plan()['numeric_gates']['reference_tolerance'],'SciPy Bass reference changed')
    print('Pinned binomial/SciPy Bass regeneration passed; maximum continuous difference',maximum)


def contract():
    ref=read_reference()
    for mutate in [lambda r:r['abm'].pop(),lambda r:r['metadata'].__setitem__('plan_sha256','stale'),
                   lambda r:r['continuous'].pop(),lambda r:r['continuous'][1].__setitem__('fraction',-.1),
                   lambda r:r['abm'][1].__setitem__('adopted',True),lambda r:r['abm'][1].__setitem__('adopted',100000),
                   lambda r:r['abm'][1].__setitem__('ticks',999),lambda r:r['abm'][1].__setitem__('dt',.1)]:
        bad=copy.deepcopy(ref);mutate(bad)
        try:
            require(bad['metadata']==metadata(),'stale metadata');validate_continuous(bad['continuous']);validate_abm(bad['abm'])
        except ValueError:continue
        raise ValueError('corrupt Bass reference accepted')
    # Structural accounting alone admits frozen dynamics: the scored gates must reject it.
    frozen=copy.deepcopy(ref['abm']);cases={c['id']:c for c in make_plan()['cases']}
    for row in frozen:row['adopted']=int(row['population']*cases[row['case']]['initial_fraction'])
    validate_abm(frozen);bad=compare(frozen,ref)
    require(not bad['passed'] and all(not c['passed'] for c in bad['curves'])
            and any(not g['passed'] for g in bad['analytic_gates']),'frozen Bass dynamics accepted')
    require(ks([0,0],[1,1])==1 and ks([0,1],[0,1])==0,'KS contract')
    for c in [dict(p=0.,q=.7,initial_fraction=0.),dict(p=.2,q=.7,initial_fraction=1.),dict(p=0.,q=0.,initial_fraction=.3)]:
        require(closed(c,8)==c['initial_fraction'],'absorbing closed form')
    for dt in make_plan()['sd_steps']:
        c=cases['innovation-only']
        require(all(abs(v-(1-(1-c['p']*dt)**int(t/dt)))<2e-14 for t,v in euler(c,dt).items()),'discrete analytic law')
    frozen_sd=[dict(kind='sd',case=c['id'],dt=dt,method=m,time=t,fraction=c['initial_fraction'])
               for c in make_plan()['cases'] for dt in make_plan()['sd_steps'] for m in ['euler','rk4'] for t in make_plan()['times']]
    validate_sd(frozen_sd)
    require(all(not g['passed'] for g in numeric_checks(frozen_sd)[0]),'frozen SD dynamics accepted')
    print('Bass reference coverage, conservation, absorption, analytic and frozen-dynamics contracts passed')


def plot(report,directory):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    result=json.loads(Path(report).read_text());require(result['passed'],'cannot plot failed Bass evidence')
    require(result['metadata']==metadata(),'stale Bass figure source')
    out=Path(directory);out.mkdir(parents=True,exist_ok=True)
    p=make_plan();colors=['#2463a7','#bd5c28','#27865b']
    fig,axes=plt.subplots(1,2,figsize=(12,4.8),layout='constrained')
    for c,color in zip(p['cases'],colors):
        for dt,style in zip(p['ensemble_steps'],['--','-']):
            points=[r for r in result['summaries'] if r['case']==c['id'] and r['dt']==dt]
            n=[r['population'] for r in points];label=f"{c['id']}, dt={dt:g}"
            axes[0].loglog(n,[r['path_rmse'] for r in points],'o'+style,color=color,label=label)
            axes[1].loglog(n,[r['continuous_mean_l2'] for r in points],'o'+style,color=color,label=label)
    axes[0].set(title='Population error relative to discrete SD',ylabel='Path RMSE to Euler trajectory')
    axes[1].set(title='Mean gap to continuous SD',ylabel='L2 gap (numerical bias + sampling included)')
    for ax in axes:
        ax.set_xlabel('Population N');ax.set_xticks(p['populations'],labels=[str(n) for n in p['populations']]);ax.grid(True,which='both',alpha=.2)
    axes[0].legend(fontsize=7)
    fig.suptitle('Bass adoption → SD | 256 replications per point',fontsize=13)
    for extension in ['svg','png']:fig.savefig(out/f'bass-mean-field-convergence.{extension}',dpi=170,metadata={'Date':None} if extension=='svg' else None)
    plt.close(fig)
    fig,axes=plt.subplots(1,2,figsize=(12,4.6),layout='constrained')
    for c,color in zip(result['refinement'],colors):
        axes[0].loglog(c['steps'],c['euler_errors'],'o-',color=color,label=c['case'])
        axes[1].loglog(c['steps'],c['rk4_errors'],'o-',color=color,label=c['case'])
    for ax,method in zip(axes,['Euler','RK4']):
        ax.set(xlabel='Step dt',ylabel='Maximum error to closed-form Bass',title=method+' numerical refinement')
        ax.grid(True,which='both',alpha=.2);ax.legend(fontsize=8)
    fig.suptitle('Time-step error measured separately from population and sampling error',fontsize=12)
    for extension in ['svg','png']:fig.savefig(out/f'bass-mean-field-refinement.{extension}',dpi=170,metadata={'Date':None} if extension=='svg' else None)
    plt.close(fig)
    fields=['case','dt','population','path_rmse','ensemble_mean_l2','continuous_mean_l2','numeric_bias_l2','population_spread_rms','mean_mc_standard_error_rms']
    with (out/'bass-mean-field-gaps.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader()
        for row in result['summaries']:writer.writerow({k:row[k] for k in fields})
    with (out/'bass-mean-field-refinement.csv').open('w',newline='') as f:
        writer=csv.writer(f);writer.writerow(['case','dt','euler_max_error','rk4_max_error'])
        for c in result['refinement']:
            for dt,e,r in zip(c['steps'],c['euler_errors'],c['rk4_errors']):writer.writerow([c['case'],dt,e,r])
    (out/'bass-mean-field-provenance.json').write_text(json.dumps(dict(metadata=result['metadata'],report_sha256=digest(Path(report)),matplotlib=importlib.metadata.version('matplotlib')),indent=2)+'\n')
    print('Bass figures, numeric gaps, refinement and provenance written to',out)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    for flag in ['write','verify','contract']:parser.add_argument('--'+flag,action='store_true')
    parser.add_argument('--native');parser.add_argument('--report');parser.add_argument('--plot');args=parser.parse_args()
    require((HERE/'bass-mean-field-plan.json').read_text()==encoded_plan(),'stale Bass plan')
    if args.write:REFERENCE.write_text(encode(generate()))
    if args.verify:verify()
    if args.contract:contract()
    if args.native:
        sd,abm=native(args.native);result=compare(abm,read_reference(),sd);result['exact_count_snapshots']=exact_counts(abm)
        if args.report:Path(args.report).write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
        require(result['passed'],'Bass mean-field gates failed: '+str([g for key in ['independent_gates','mean_field_gates','analytic_gates','numeric_gates','curves'] for g in result[key] if not g['passed']]))
        print('Bass: 90 independent comparisons, 30 finest-N mean gates, 30 analytic gates, six population curves and three numerical refinement gates passed;',result['exact_count_snapshots'],'exact snapshots')
    if args.plot:
        require(args.report,'plot needs report');plot(args.report,args.plot)
