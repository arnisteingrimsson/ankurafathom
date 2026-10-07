"""Seeded imperfect operational fixtures and as-of calibration; no simulator trajectories."""
import argparse
from collections import Counter, defaultdict
from datetime import date, timedelta
import hashlib
import json
import math
from pathlib import Path
import random
import pyarrow as pa
import pyarrow.parquet as pq
from config import default_config

START=date(2023,1,1)
CUTOFF=date(2025,1,1)
END=date(2026,1,1)


def poisson(rng,mean):
    limit=math.exp(-mean);product=1.;n=0
    while product>limit:product*=rng.random();n+=1
    return n-1


def month_date(m):return date(2023+m//12,1+m%12,1)


def generate(directory,seed=20260930):
    directory=Path(directory);directory.mkdir(parents=True,exist_ok=False)
    rng=random.Random(seed);c=default_config();opps=[];history=[];accounts=[];roster=[];times=[]
    for i in range(100):accounts.append(dict(Id=f'A{i}',Practice='TR',Industry='synthetic'))
    for m in range(36):
        for _ in range(poisson(rng,c['prospects'])):
            oid=f'O{len(opps)}';born=month_date(m)+timedelta(days=rng.randrange(28))
            actual_close=born+timedelta(days=rng.randint(30,89));open_forever=rng.random()<.10
            won=rng.random()<c['win_rate'];stage='Closed Won' if won else 'Closed Lost'
            fee=rng.choices(['hourly','fixed','retainer','success'],[.65,.15,.15,.05])[0]
            amount=round(rng.lognormvariate(math.log(100000),.5),2)
            expected=born+timedelta(days=45);slip=rng.random()<.30
            # Snapshot fields may be stale and must not be used as historical truth.
            opps.append(dict(Id=oid,AccountId=f'A{rng.randrange(100)}',Practice='TR',CreatedDate=born,
                StageName=stage if not open_forever and actual_close<END else 'Proposal',
                Amount=amount,CloseDate=expected+timedelta(days=60 if slip else 0),
                Probability=rng.choice([.1,.5,.9]),FeeType=None if rng.random()<.08 else fee))
            history.append(dict(OpportunityId=oid,Timestamp=born,StageName='Qualified',ExpectedCloseDate=expected))
            history.append(dict(OpportunityId=oid,Timestamp=born+timedelta(days=15),StageName='Proposal',
                ExpectedCloseDate=expected+timedelta(days=60 if slip else 0)))
            if not open_forever and actual_close<END:
                history.append(dict(OpportunityId=oid,Timestamp=actual_close,StageName=stage,ExpectedCloseDate=actual_close))
    for l in c['levels']:
        for _ in range(l['count']):
            eid=f'E{len(roster)}'
            roster.append(dict(EmployeeId=eid,Level=l['id'],Practice='TR',HireDate=date(2020,1,1),
                TerminationDate=None,AnnualBasePay=l['annual_pay'],FTE=1.))
            for w in range(156):
                when=START+timedelta(days=7*w);paid=40.
                hours=paid*rng.uniform(.45,.75) if l['hours_per_job'] else 0.
                fee=rng.choices(['hourly','fixed','retainer','success'],[.65,.15,.15,.05])[0]
                realization=min(1.,max(.5,rng.gauss(c['realization'],.04)))
                times.append(dict(EmployeeId=eid,Week=when,ProjectId=f'P{w%40}',FeeType=fee,
                    PaidHours=paid,DeliveryHours=hours,StandardRate=l['rate'],
                    BilledAmount=hours*l['rate']*realization if fee=='hourly' else 0.))
    tables=dict(accounts=accounts,opportunities=opps,opportunity_history=history,roster=roster,timesheets=times)
    for name,records in tables.items():pq.write_table(pa.Table.from_pylist(records),directory/(name+'.parquet'))
    truth=dict(seed=seed,synthetic=True,prospects_per_month=100.,conditional_win_probability=.5,
        never_closes_probability=.10,mature_cohort_win_probability=.45,realization=.90,
        warning='Truth is validation-only; calibrate() never reads this file.')
    (directory/'planted-truth.json').write_text(json.dumps(truth,indent=2)+'\n')
    return truth


def calibrate(directory,asof=CUTOFF):
    directory=Path(directory)
    tables={name:pq.read_table(directory/(name+'.parquet')).to_pylist() for name in
        ('accounts','opportunities','opportunity_history','roster','timesheets')}
    for name,key in [('accounts','Id'),('opportunities','Id'),('roster','EmployeeId')]:
        keys=[r[key] for r in tables[name]]
        if len(keys)!=len(set(keys)):raise ValueError('duplicate '+name)
    accounts={r['Id'] for r in tables['accounts']};all_opps={r['Id'] for r in tables['opportunities']}
    opps={r['Id']:r for r in tables['opportunities'] if START<=r['CreatedDate']<asof}
    fee_types={'hourly','fixed','retainer','success'}
    if any(r['FeeType'] is not None and r['FeeType'] not in fee_types for r in opps.values()):
        raise ValueError('unsupported opportunity fee type')
    if any(r['AccountId'] not in accounts for r in opps.values()):raise ValueError('unknown account')
    if any(not math.isfinite(r['Amount']) or r['Amount']<0 for r in opps.values()):raise ValueError('invalid amount')
    stages={};slips=set();events=defaultdict(list)
    for r in tables['opportunity_history']:
        if r['OpportunityId'] not in all_opps:raise ValueError('unknown opportunity')
        if r['OpportunityId'] in opps and r['Timestamp']<asof:events[r['OpportunityId']].append(r)
    allowed={'Qualified','Proposal','Closed Won','Closed Lost'}
    for oid,rows in events.items():
        rows.sort(key=lambda r:r['Timestamp']);seen=set();prior=None;closed=False
        for r in rows:
            if r['Timestamp']<opps[oid]['CreatedDate'] or r['Timestamp'] in seen:raise ValueError('invalid stage chronology')
            if r['StageName'] not in allowed or closed:raise ValueError('invalid stage transition')
            seen.add(r['Timestamp']);closed=r['StageName'].startswith('Closed')
            if prior and r['ExpectedCloseDate']>prior:slips.add(oid)
            prior=r['ExpectedCloseDate']
        stages[oid]=rows[-1]['StageName']
    if stages.keys()!=opps.keys():raise ValueError('missing stage history')
    months=(asof.year-START.year)*12+asof.month-START.month
    if months<7 or asof.day!=1:raise ValueError('at least seven complete months required')
    mature=[o for o in opps.values() if (asof-o['CreatedDate']).days>=183]
    wins=sum(stages[o['Id']]=='Closed Won' for o in mature)
    p=wins/len(mature);n=len(mature);z=1.96;den=1+z*z/n
    center=(p+z*z/(2*n))/den;half=z*math.sqrt(p*(1-p)/n+z*z/(4*n*n))/den
    roster={r['EmployeeId']:r for r in tables['roster'] if r['HireDate']<asof and (r['TerminationDate'] is None or r['TerminationDate']>=asof)}
    accum=defaultdict(lambda:dict(paid=0.,delivery=0.,standard=0.,billed=0.,rates=[],pay=[],fte=0))
    for r in roster.values():
        if r['FTE']!=1.:raise ValueError('only full-time roster supported by this adapter')
        if not math.isfinite(r['AnnualBasePay']) or r['AnnualBasePay']<0:raise ValueError('invalid annual base pay')
        accum[r['Level']]['pay'].append(r['AnnualBasePay']);accum[r['Level']]['fte']+=1
    seen=set()
    for r in tables['timesheets']:
        if not START<=r['Week']<asof:continue
        key=(r['EmployeeId'],r['Week'],r['ProjectId'])
        if key in seen:raise ValueError('duplicate timesheet')
        seen.add(key)
        if r['EmployeeId'] not in roster:raise ValueError('timesheet employee absent from current roster; changing roster unsupported')
        if r['FeeType'] not in fee_types:raise ValueError('unsupported timesheet fee type')
        if r['Week']<roster[r['EmployeeId']]['HireDate']:raise ValueError('timesheet predates hire')
        if any(not math.isfinite(r[k]) or r[k]<0 for k in ('PaidHours','DeliveryHours','StandardRate','BilledAmount')) or r['DeliveryHours']>r['PaidHours']:
            raise ValueError('invalid timesheet quantity')
        row=accum[roster[r['EmployeeId']]['Level']];row['paid']+=r['PaidHours'];row['delivery']+=r['DeliveryHours'];row['rates'].append(r['StandardRate'])
        if r['FeeType']=='hourly':row['standard']+=r['DeliveryHours']*r['StandardRate'];row['billed']+=r['BilledAmount']
    totalstandard=sum(r['standard'] for r in accum.values())
    if not totalstandard:raise ValueError('no hourly standard value')
    estimates=dict(prospects=len(opps)/months,win_rate=p,realization=sum(r['billed'] for r in accum.values())/totalstandard)
    levels={k:dict(count=v['fte'],annual_pay=sum(v['pay'])/len(v['pay']),rate=sum(v['rates'])/len(v['rates']),
                  utilization=v['delivery']/v['paid']) for k,v in accum.items()}
    missing=sum(o['FeeType'] is None for o in opps.values())
    return dict(asof=asof.isoformat(),estimates=estimates,levels=levels,
        win_rate_definition='Wins / all opportunities in cohorts at least 183 days old, including long-open cases; not closed-only win rate.',
        win_rate_95_wilson=[center-half,center+half],mature_cohort=n,
        diagnostics=dict(opportunities=len(opps),missing_fee_type=missing,slipped_expected_close=len(slips),
            open_asof=sum(not s.startswith('Closed') for s in stages.values()),
            fee_mix_estimation='withheld: missing fees are reported, never silently imputed',
            stale_snapshot_probabilities='ignored; estimates reconstructed from dated stage history'),
        source_hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in directory.glob('*.parquet')})


def apply_calibration(c,fit):
    c.update(fit['estimates'])
    if {l['id'] for l in c['levels']}!=fit['levels'].keys():raise ValueError('level mismatch')
    for l in c['levels']:
        for k in ('count','annual_pay','rate'):l[k]=fit['levels'][l['id']][k]
    return c


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    generate(a.out);print(json.dumps(calibrate(a.out),indent=2))
