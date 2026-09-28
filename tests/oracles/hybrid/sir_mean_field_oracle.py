"""Independent SciPy SD and aggregate CTMC oracles for individual well-mixed SIR."""
import argparse
import bisect
import copy
import csv
import hashlib
import importlib.metadata
import json
import math
from pathlib import Path
import random
import statistics
import sys
from sir_mean_field_plan import HERE,make_plan,encoded as encoded_plan
REFERENCE=HERE/'sir-mean-field-reference.json'


def require(v,message):
    if not v:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata():
    return dict(plan_sha256=digest(HERE/'sir-mean-field-plan.json'),adapter_sha256=digest(Path(__file__)),
                requirements_sha256=digest(HERE/'requirements.txt'),engines=dict(numpy='2.5.3',scipy='1.18.1'))


def encode(value):
    return json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False)+'\n'


def generate():
    for line in (HERE/'requirements.txt').read_text().splitlines():
        package,version=line.split('==')
        require(importlib.metadata.version(package)==version,'unpinned '+package)
    from scipy.integrate import solve_ivp
    p=make_plan(); sd=[]; abm=[]
    i0=1/p['initial_infected_denominator']
    for index,c in enumerate(p['cases']):
        def derivative(t,x):
            infection=c['beta']*x[0]*x[1]; recovery=c['gamma']*x[1]
            return [-infection,infection-recovery,recovery]
        result=solve_ivp(derivative,[0,p['times'][-1]],[1-i0,i0,0],t_eval=p['times'],method='DOP853',rtol=1e-12,atol=1e-14)
        require(result.success,'SciPy solve failed')
        for t,x in zip(p['times'],result.y.T):
            sd.append(dict(case=c['id'],time=t,fractions=x.tolist()))
        for ni,n in enumerate(p['populations']):
            for rep in range(p['replications']):
                rng=random.Random(p['reference_seed']+1000003*index+10007*ni+rep)
                s,i,r=n-n//p['initial_infected_denominator'],n//p['initial_infected_denominator'],0
                clock=0.; events=0
                def schedule():
                    infection=c['beta']*s*i/n; recovery=c['gamma']*i
                    total=infection+recovery
                    if not total:
                        return None
                    return clock+rng.expovariate(total),rng.random()*total<infection
                pending=schedule()
                for t in p['times']:
                    while pending and pending[0]<=t:
                        clock,infect=pending
                        if infect:
                            s-=1; i+=1
                        else:
                            i-=1; r+=1
                        events+=1
                        pending=schedule()
                    abm.append(dict(case=c['id'],population=n,replication=rep,time=t,counts=[s,i,r],events=events))
    return dict(metadata=metadata(),sd=sd,abm=abm)


def validate_sd(rows,native=False):
    p=make_plan()
    expected=[(c,dt,t) for c in p['cases'] for dt in (p['sd_steps'] if native else [None]) for t in p['times']]
    require(len(rows)==len(expected),'SD observation coverage')
    prior=None
    for row,(c,dt,t) in zip(rows,expected):
        require(set(row)==({'kind','case','dt','time','fractions'} if native else {'case','time','fractions'}),'SD row fields')
        require(row['case']==c['id'] and type(row['time']) in (int,float) and row['time']==t,'SD row identity')
        if native:
            require(row['kind']=='sd' and type(row['dt']) in (int,float) and row['dt']==dt,'SD step identity')
        values=row['fractions']
        require(isinstance(values,list) and len(values)==3 and all(type(v) in (int,float) and math.isfinite(v) and v>=-1e-13 for v in values)
                and abs(sum(values)-1)<1e-12,'SD simplex/conservation')
        if t==0:
            require(values==[.9,.1,0],'SD initial fractions')
        else:
            require(values[0]<=prior[0]+1e-13 and values[2]>=prior[2]-1e-13,'SD reverse history')
        if c['gamma']==0:
            require(abs(values[2])<1e-14,'recovery at gamma=0')
        if c['beta']==0:
            require(abs(values[0]-.9)<1e-14 and abs(values[1]-.1*math.exp(-c['gamma']*t))<1e-8,'recovery-only analytic law')
        prior=values


def validate_abm(rows):
    p=make_plan()
    expected=[(c,n,r,t) for c in p['cases'] for n in p['populations'] for r in range(p['replications']) for t in p['times']]
    require(len(rows)==len(expected),'ABM observation coverage')
    prior=None
    for row,(c,n,r,t) in zip(rows,expected):
        require(set(row)=={'case','population','replication','time','counts','events'},'ABM row fields')
        require(row['case']==c['id'] and type(row['population']) is int and row['population']==n
                and type(row['replication']) is int and row['replication']==r
                and type(row['time']) in (int,float) and row['time']==t,'ABM row identity')
        counts=row['counts']; i0=n//p['initial_infected_denominator']; s0=n-i0
        require(isinstance(counts,list) and len(counts)==3 and all(type(v) is int and v>=0 for v in counts)
                and sum(counts)==n,'ABM exact population conservation')
        s,i,removed=counts
        require(s<=s0 and type(row['events']) is int and row['events']==s0-s+removed,'ABM event accounting')
        if t==0:
            require(counts==[s0,i0,0] and row['events']==0,'ABM initial counts')
        else:
            require(s<=prior['counts'][0] and removed>=prior['counts'][2] and row['events']>=prior['events'],'ABM reverse history')
            if prior['counts'][1]==0:
                require(counts==prior['counts'],'absorbed epidemic restarted')
        if c['beta']==0:
            require(s==s0,'infection at beta=0')
        if c['gamma']==0:
            require(removed==0,'recovery at gamma=0')
        prior=row


def read_reference():
    value=json.loads(REFERENCE.read_text())
    require(set(value)=={'metadata','sd','abm'} and value['metadata']==metadata(),'stale mean-field reference')
    validate_sd(value['sd']);validate_abm(value['abm'])
    return value


def ks(a,b):
    a,b=sorted(a),sorted(b)
    return max(abs(bisect.bisect_right(a,x)/len(a)-bisect.bisect_right(b,x)/len(b)) for x in set(a+b))


def exact_counts(rows):
    # Individual recurrence, independent of native records, calendar and contact queries.
    sys.path.insert(0,str(HERE.parents[1]))
    from abm_ir_contract import word
    p=make_plan(); n=p['populations'][0]
    actual={(r['case'],r['population'],r['replication'],r['time']):r for r in rows}
    observations=0
    for c in p['cases']:
        for rep in [0,p['replications']-1]:
            states=[1]*(n//p['initial_infected_denominator'])+[0]*(n-n//p['initial_infected_denominator'])
            now=0.; generation=0
            def schedule():
                common=(c['beta']/n)*states.count(1)
                weights=[common if s==0 else c['gamma'] if s==1 else 0. for s in states]
                total=sum(weights)
                if not total:
                    return None
                def u(stream):
                    return (word(p['seed'],p['scenario'],rep,0,generation,stream)+.5)/2**32
                time=now-math.log(u(p['waiting_stream']))/total
                target=min(u(p['selection_stream'])*total,math.nextafter(total,0.))
                cumulative=0.
                for i,w in enumerate(weights):
                    cumulative+=w
                    if target<cumulative:
                        return time,i
                raise ValueError('independent selection failed')
            pending=schedule()
            for t in p['times']:
                while pending and pending[0]<=t:
                    now,i=pending; states[i]+=1; generation+=1; pending=schedule()
                row=actual[c['id'],n,rep,t]
                require(row['counts']==[states.count(s) for s in range(3)] and row['events']==generation,'addressed well-mixed count history differs')
                observations+=1
    return observations


def compare(sd,abm,reference,*,check_sd=True):
    p=make_plan(); nrep=p['replications']; times=p['times'][1:]
    sd_lookup={(r['case'],r['time']):r['fractions'] for r in reference['sd']}
    sd_errors={}
    for row in sd:
        errors=[abs(a-b) for a,b in zip(row['fractions'],sd_lookup[row['case'],row['time']])]
        key=row['case'],row['dt'];sd_errors[key]=max(sd_errors.get(key,0),*errors)
    sd_gates=[]
    for c in (p['cases'] if check_sd else []):
        coarse,fine=[sd_errors[c['id'],dt] for dt in p['sd_steps']]
        ratio=coarse/fine if fine else 0.
        sd_gates.append(dict(case=c['id'],coarse_error=coarse,fine_error=fine,halving_ratio=ratio,
                             passed=fine<=p['sd_error_limit'] and p['sd_halving_ratio'][0]<=ratio<=p['sd_halving_ratio'][1]))
    actual={(r['case'],r['population'],r['replication'],r['time']):r for r in abm}
    ref={(r['case'],r['population'],r['replication'],r['time']):r for r in reference['abm']}
    gates=[]; summaries=[]; finest_gates=[]
    policy=p['independent_gates'];limit=math.sqrt(math.log(2*policy['comparisons']/policy['family_alpha'])/nrep)
    for c in p['cases']:
        cid=c['id']
        for n in p['populations']:
            loss=[]; bias2=variance=mean_variance=0.; trajectory=[]
            for t in p['times']:
                vectors=[[v/n for v in actual[cid,n,r,t]['counts']] for r in range(nrep)]
                means=[statistics.mean(v[k] for v in vectors) for k in range(3)]
                variances=[statistics.variance(v[k] for v in vectors) for k in range(3)]
                trajectory.append(dict(time=t,mean=means,standard_error=[math.sqrt(v/nrep) for v in variances]))
                if not t:
                    continue
                target=sd_lookup[cid,t]
                bias2+=sum((a-b)**2 for a,b in zip(means,target))/(3*len(times))
                variance+=sum(v*(nrep-1)/nrep for v in variances)/(3*len(times))
                mean_variance+=sum(variances)/(nrep*3*len(times))
                if n==p['populations'][-1]:
                    for k,metric in enumerate(['susceptible','infected','recovered']):
                        se=math.sqrt(variances[k]/nrep); bound=p['limit_gates']['finest_mean_floor']+p['limit_gates']['mean_sigma']*se
                        gap=means[k]-target[k]
                        finest_gates.append(dict(case=cid,time=t,metric=metric,mean=means[k],sd=target[k],gap=gap,
                                                 standard_error=se,limit=bound,difference_ci95=[gap-1.96*se,gap+1.96*se],passed=abs(gap)<=bound))
                for k,metric in [(1,'infected'),(2,'recovered')]:
                    a=[v[k] for v in vectors]; b=[ref[cid,n,r,t]['counts'][k]/n for r in range(nrep)]
                    ma,mb=statistics.mean(a),statistics.mean(b)
                    se=math.sqrt((statistics.variance(a)+statistics.variance(b))/nrep)
                    bound=max(policy['mean_floor'],policy['mean_sigma']*se);distance=ks(a,b)
                    gates.append(dict(case=cid,population=n,time=t,metric=metric,mean_gap=abs(ma-mb),mean_limit=bound,
                                      difference_ci95=[ma-mb-1.96*se,ma-mb+1.96*se],ks=distance,ks_limit=limit,
                                      passed=abs(ma-mb)<=bound and distance<=limit))
            for r in range(nrep):
                loss.append(sum((actual[cid,n,r,t]['counts'][k]/n-sd_lookup[cid,t][k])**2 for t in times for k in range(3))/(3*len(times)))
            mse=statistics.mean(loss); mse_se=math.sqrt(statistics.variance(loss)/nrep)
            require(abs(mse-bias2-variance)<1e-13,'path MSE decomposition failed')
            summaries.append(dict(case=cid,population=n,path_rmse=math.sqrt(mse),
                                  rmse_ci95=[math.sqrt(max(0,mse-1.96*mse_se)),math.sqrt(mse+1.96*mse_se)],
                                  ensemble_mean_l2=math.sqrt(bias2),population_spread_rms=math.sqrt(variance),
                                  mean_mc_standard_error_rms=math.sqrt(mean_variance),trajectory=trajectory))
    require(len(gates)==policy['comparisons'] and len(finest_gates)==p['limit_gates']['mean_comparisons'],'mean-field gate count')
    curves=[]
    for c in p['cases']:
        errors=[r['path_rmse'] for r in summaries if r['case']==c['id']]
        ratios=[b/a for a,b in zip(errors,errors[1:])];g=p['limit_gates']
        curves.append(dict(case=c['id'],populations=p['populations'],path_rmse=errors,ratios=ratios,finest_coarse_ratio=errors[-1]/errors[0],
                           passed=all(g['rmse_ratio_min']<=r<=g['rmse_ratio_max'] for r in ratios) and errors[-1]/errors[0]<=g['finest_coarse_max']))
    return dict(metadata=metadata(),runs_per_stochastic_engine=len(p['cases'])*len(p['populations'])*nrep,
                sd_gates=sd_gates,independent_gates=gates,mean_field_gates=finest_gates,curves=curves,summaries=summaries,
                passed=all(g['passed'] for g in sd_gates+gates+finest_gates+curves))


def native(path):
    with Path(path).open() as file:
        require(json.loads(next(file))==dict(plan=make_plan()),'native mean-field plan differs')
        rows=[json.loads(line) for line in file]
    require(all(row.get('kind') in ['sd','abm'] for row in rows),'unknown native kind')
    sd=[row for row in rows if row['kind']=='sd']
    abm=[{k:v for k,v in row.items() if k!='kind'} for row in rows if row['kind']=='abm']
    validate_sd(sd,True);validate_abm(abm)
    return sd,abm


def verify():
    saved=read_reference();regenerated=generate()
    validate_sd(regenerated['sd']);validate_abm(regenerated['abm'])
    require(saved['abm']==regenerated['abm'],'aggregate CTMC reference changed')
    maximum=0.
    for a,b in zip(saved['sd'],regenerated['sd']):
        require(a['case']==b['case'] and a['time']==b['time'],'SciPy identity changed')
        maximum=max(maximum,*[abs(x-y) for x,y in zip(a['fractions'],b['fractions'])])
    require(maximum<=make_plan()['reference_tolerance'],'SciPy SIR reference changed')
    print('Pinned SD/aggregate CTMC regeneration passed; maximum SD error',maximum)


def contract():
    reference=read_reference()
    for mutate in [lambda r:r['abm'].pop(),lambda r:r['metadata'].__setitem__('plan_sha256','stale'),
                   lambda r:r['abm'][1]['counts'].__setitem__(0,True),lambda r:r['abm'][1].__setitem__('events',999999),
                   lambda r:r['abm'][1].__setitem__('time',999),lambda r:r['sd'][1]['fractions'].__setitem__(1,-1),
                   lambda r:r['sd'][1]['fractions'].__setitem__(0,.1)]:
        bad=copy.deepcopy(reference);mutate(bad)
        try:
            require(bad['metadata']==metadata(),'stale metadata');validate_sd(bad['sd']);validate_abm(bad['abm'])
        except ValueError:
            continue
        raise ValueError('corrupt mean-field reference accepted')
    require(ks([0,0],[1,1])==1 and ks([0,1],[0,1])==0,'KS contract')
    frozen=copy.deepcopy(reference['abm'])
    for row in frozen:
        n=row['population']; infected=n//make_plan()['initial_infected_denominator']
        row['counts']=[n-infected,infected,0]; row['events']=0
    validate_abm(frozen)  # Valid accounting alone cannot detect missing dynamics.
    rejected=compare([],frozen,reference,check_sd=False)
    require(not rejected['passed'] and all(not c['passed'] for c in rejected['curves'])
            and any(not g['passed'] for g in rejected['independent_gates']), 'frozen dynamics accepted')
    print('Mean-field reference, conservation/accounting, analytic recovery, corruption and frozen-dynamics contracts passed')


def plot(report,directory):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    result=json.loads(Path(report).read_text()); require(result['passed'],'cannot plot failed mean-field evidence')
    out=Path(directory);out.mkdir(parents=True,exist_ok=True)
    colors=['#2463a7','#bd5c28','#27865b','#875aa7']
    fig,axes=plt.subplots(1,2,figsize=(12,4.6),layout='constrained')
    for color,curve in zip(colors,result['curves']):
        points=[r for r in result['summaries'] if r['case']==curve['case']]
        n=[r['population'] for r in points];error=[r['path_rmse'] for r in points]
        axes[0].loglog(n,error,'o-',color=color,label=curve['case'])
        axes[0].fill_between(n,[r['rmse_ci95'][0] for r in points],[r['rmse_ci95'][1] for r in points],color=color,alpha=.12)
        axes[1].loglog(n,[r['ensemble_mean_l2'] for r in points],'o-',color=color,label=curve['case'])
    n=make_plan()['populations'];anchor=result['curves'][0]['path_rmse'][0]
    axes[0].loglog(n,[anchor*math.sqrt(n[0]/x) for x in n],'k--',linewidth=1,label='N⁻¹ᐟ² guide')
    axes[0].set_title('Individual path error shrinks with population')
    axes[0].set_ylabel('Path RMSE of S, I, R fractions')
    axes[1].set_title('Ensemble-mean error (sampling noise included)')
    axes[1].set_ylabel('L2 gap of ensemble mean to SD')
    for ax in axes:
        ax.set_xlabel('Population N');ax.set_xticks(n,labels=[str(v) for v in n]);ax.grid(True,which='both',alpha=.2)
    axes[0].legend(fontsize=8)
    fig.suptitle('Well-mixed SIR → SD limit | 256 replications per point',fontsize=13)
    fig.savefig(out/'sir-mean-field-convergence.svg',metadata={'Date':None})
    fig.savefig(out/'sir-mean-field-convergence.png',dpi=170)
    plt.close(fig)
    fig,ax=plt.subplots(figsize=(9,4.6),layout='constrained')
    reference=read_reference()
    epidemic=[r for r in reference['sd'] if r['case']=='epidemic']
    means=next(r for r in result['summaries'] if r['case']=='epidemic' and r['population']==n[-1])['trajectory']
    for k,(color,label) in enumerate(zip(colors,['Susceptible','Infected','Recovered'])):
        ax.plot([r['time'] for r in epidemic],[r['fractions'][k] for r in epidemic],'-',color=color,label=label+' SD')
        ax.errorbar([r['time'] for r in means],[r['mean'][k] for r in means],
                    yerr=[1.96*r['standard_error'][k] for r in means],fmt='o',ms=3,capsize=2,color=color,label=label+' ABM mean ± 1.96 SE')
    ax.set(xlabel='Time',ylabel='Population fraction',title='Epidemic case: N=640 ABM ensemble and independent SD')
    ax.set_ylim(0,1);ax.grid(alpha=.2);ax.legend(fontsize=8,ncol=2)
    fig.savefig(out/'sir-mean-field-trajectory.svg',metadata={'Date':None});fig.savefig(out/'sir-mean-field-trajectory.png',dpi=170);plt.close(fig)
    with (out/'sir-mean-field-gaps.csv').open('w',newline='') as file:
        fields=['case','population','path_rmse','ensemble_mean_l2','population_spread_rms','mean_mc_standard_error_rms']
        writer=csv.DictWriter(file,fieldnames=fields);writer.writeheader()
        for row in result['summaries']:
            writer.writerow({k:row[k] for k in fields})
    (out/'sir-mean-field-provenance.json').write_text(json.dumps(dict(metadata=result['metadata'],report_sha256=digest(Path(report)),
        matplotlib=importlib.metadata.version('matplotlib')),indent=2)+'\n')
    print('Mean-field SVG/PNG figures, numeric gaps and provenance written to',out)


if __name__=='__main__':
    p=argparse.ArgumentParser()
    for flag in ['write','verify','contract']:
        p.add_argument('--'+flag,action='store_true')
    p.add_argument('--native');p.add_argument('--report');p.add_argument('--plot');args=p.parse_args()
    require((HERE/'sir-mean-field-plan.json').read_text()==encoded_plan(),'stale mean-field plan')
    if args.write:
        REFERENCE.write_text(encode(generate()))
    if args.verify:
        verify()
    if args.contract:
        contract()
    if args.native:
        sd,abm=native(args.native);result=compare(sd,abm,read_reference());result['exact_count_snapshots']=exact_counts(abm)
        if args.report:
            Path(args.report).write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
        require(result['passed'],'SIR mean-field gates failed: '+str([g for key in ['sd_gates','independent_gates','mean_field_gates','curves'] for g in result[key] if not g['passed']]))
        print('SIR mean field: 168 independent gates, 84 finest-N mean gates, 4 population-scaling curves and 4 RK4 checks passed;',result['exact_count_snapshots'],'exact snapshots')
    if args.plot:
        require(args.report,'plot requires report');plot(args.report,args.plot)
