"""Build, execute, independently verify and summarize the bounded monthly pilot."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
import pyarrow.parquet as pq
from config import default_config, scenarios, scenario
from model import build, experiment
from oracle import compare
from data import generate, calibrate, apply_calibration

ROOT=Path(__file__).resolve().parents[2]
EXE=Path(os.environ.get('FATHOM_EXECUTABLE',ROOT/'build-arrow/fathom'))


def save(path,value):Path(path).write_text(json.dumps(value,indent=2,allow_nan=False)+'\n')


def invoke(exe,*args):
    p=subprocess.run([str(exe),*map(str,args)],capture_output=True,text=True)
    if p.returncode:raise RuntimeError(f'{args}: {p.stderr or p.stdout}')
    return p.stdout


def calendar(months):
    return dict(time=list(range(months+1)),adoption=[min(1.,(m+1)/6) for m in range(months+1)],
        initial=[1.]+[0.]*months,market=[1.]*(months+1),
        season=[1.+.1*math.sin(2*math.pi*m/12) for m in range(months+1)])


def native(exe,model,exp,out,threads=1,resume=False):
    check=out.with_suffix('.validation.json')
    if not (resume and out.exists() and check.exists() and Path(str(out)+'.manifest.json').exists()):
        invoke(exe,'check',model,'--experiment',exp,'--threads',threads,'--out',check)
        invoke(exe,'run',model,'--experiment',exp,'--threads',threads,'--require-check','--out',out)
    receipt=json.loads(check.read_text())
    if receipt['verdict']!='pass':raise AssertionError(receipt)
    manifest=json.loads(Path(str(out)+'.manifest.json').read_text())
    if receipt['inputs']!=manifest['inputs'] or receipt['result']!=manifest['result']:raise AssertionError('check/run identity mismatch')
    invoke(exe,'verify-results',out,'--embedded')
    actual={}
    for row in pq.read_table(out,columns=['scenario','replication','time','output_id','value']).to_pylist():
        if row['replication']!=0 or row['time']!=int(row['time']):raise AssertionError('unexpected sample')
        obs=actual.setdefault((row['scenario'],int(row['time'])),{})
        if row['output_id'] in obs:raise AssertionError('duplicate output')
        obs[row['output_id']]=row['value']
    return actual,dict(manifest_id=manifest['id'],rules=sum(x.get('samples',0) for x in receipt['checks']))


def execute(directory,c,rows,cal,exe=EXE,threads=1,resume=False):
    directory=Path(directory)
    if resume and (directory/'inputs').exists():
        with tempfile.TemporaryDirectory(prefix='tr-resume-') as tmp:
            fresh=Path(tmp)/'inputs';build(c,fresh,cal)
            for name in ('model.json','calendar.parquet','parameters.parquet'):
                if (fresh/name).read_bytes()!=(directory/'inputs'/name).read_bytes():
                    raise ValueError('resume inputs differ: '+name)
        if json.loads((directory/'scenarios.json').read_text())!=rows:raise ValueError('resume scenarios differ')
    else:build(c,directory/'inputs',cal)
    save(directory/'config.json',c);save(directory/'scenarios.json',rows)
    save(directory/'inputs/experiment.json',experiment(rows))
    obs,receipt=native(exe,directory/'inputs/model.json',directory/'inputs/experiment.json',directory/'results.parquet',threads,resume)
    proof=compare(obs,c,rows,cal)
    return obs,dict(native=receipt,independent=proof)


def ratio(a,b):return a/b if b else None


def summarize(obs,c,rows):
    result=[]
    for sid,s in enumerate(rows):
        for end in range(12,c['months']+1,12):
            start=end-12;last=obs[(sid,end)];first=obs[(sid,start)]
            delta=lambda key:last['cum_'+key]-first['cum_'+key]
            avg=lambda key:sum(obs[(sid,t)]['m_'+key] for t in range(start+1,end+1))/12
            fte=avg('fte');revenue=delta('revenue');ebitda=delta('ebitda')
            hours=delta('delivery_hours');billpaid=sum(sum(obs[(sid,t)]['m_paid_l'+str(i)] for i,l in enumerate(c['levels']) if l['hours_per_job']) for t in range(start+1,end+1))
            md=len(c['levels'])-2 if c['levels'][-1]['hours_per_job']==0 else len(c['levels'])-1
            mdavg=avg('fte_l'+str(md))
            result.append(dict(scenario=s['name'],year=end//12,revenue=revenue,ebitda=ebitda,
                ebitda_margin=ratio(ebitda,revenue),operating_cash=delta('cash_flow'),collections=delta('collections'),
                ending_receivables=last['receivables'],ending_deposit_liability=last['deposit_liability'],
                loaded_comp=delta('loaded_comp'),comp_ratio=ratio(delta('loaded_comp'),revenue),
                average_expected_fte=fte,ending_expected_fte=last['m_fte'],
                billable_role_utilization=ratio(hours,billpaid),all_staff_utilization=ratio(hours,delta('paid')),
                blended_hourly_rate=ratio(delta('earned_tm'),delta('hourly_hours')),
                hourly_realization_before_disallowance=ratio(delta('earned_tm'),delta('hourly_standard')),
                revenue_per_fte=ratio(revenue,fte),revenue_per_md=ratio(revenue,mdavg),
                leverage=ratio(sum(avg('fte_l'+str(i)) for i,l in enumerate(c['levels']) if l['hours_per_job'])-mdavg,mdavg),
                ai_cost_per_fte=ratio(delta('ai_cost'),fte),won_work=delta('won'),delivered_work=delta('delivery'),
                win_rate=ratio(delta('won'),delta('prospects')),
                backlog_months=ratio(sum(last['backlog_'+f] for f in ('tm','fixed','retainer','success')),delta('delivery')/12),
                bd_hours=delta('bd_hours'),bd_generated_prospects=delta('bd_prospects'),
                levels=[dict(level=l['id'],ending_expected_fte=last[f'l{i}_fte'],
                    utilization=ratio(delta('delivery_l'+str(i)),sum(obs[(sid,t)]['m_paid_l'+str(i)] for t in range(start+1,end+1))),
                    exits=delta('exit_l'+str(i)),starts=delta('starts_l'+str(i)),requests=delta('request_l'+str(i))) for i,l in enumerate(c['levels'])]))
    return result


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--executable',type=Path,default=EXE);parser.add_argument('--threads',type=int,default=8)
    parser.add_argument('--resume',action='store_true');parser.add_argument('--workers',type=int,default=3)
    args=parser.parse_args();c=default_config();rows=scenarios();cal=calendar(c['months'])
    # Explicit synthetic countercyclical stress, not a forecast fitted to bankruptcy data.
    cal['market']=[1. if m<12 else 1.4 if m<24 else .8 if m<36 else 1. for m in range(c['months']+1)]
    args.out.mkdir(parents=True,exist_ok=args.resume)
    if not (args.resume and (args.out/'operational-data').exists()):generate(args.out/'operational-data')
    fit=calibrate(args.out/'operational-data');apply_calibration(c,fit)
    save(args.out/'calibration.json',fit);save(args.out/'config.json',c);save(args.out/'scenarios.json',rows)
    obs={};proof=dict(batches=[])
    # Bound Arrow's repeated per-row lineage memory; retain complete native artifacts per batch.
    def batch(offset):
        group=rows[offset:offset+3];folder=args.out/f'batch-{offset//3:02d}'
        print(f'Native scenarios {offset+1}-{offset+len(group)} of {len(rows)}...',flush=True)
        part,receipt=execute(folder,c,group,cal,args.executable,args.threads,args.resume)
        manifest=str(folder/'results.parquet')+'.manifest.json'
        receipt['one_thread_replay']=json.loads(invoke(args.executable,'replay',manifest,'--threads',1))
        if not (folder/'replay-bundle').exists():invoke(args.executable,'bundle',manifest,'--out',folder/'replay-bundle')
        receipt['bundle_replay']=json.loads(invoke(args.executable,'replay',folder/'replay-bundle','--threads',args.threads))
        save(folder/'validation.json',receipt)
        print(f'Passed scenarios {offset+1}-{offset+len(group)}.',flush=True)
        return offset,part,receipt
    if args.workers<1 or args.workers>4:raise ValueError('workers must be 1..4')
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for offset,part,receipt in pool.map(batch,range(0,len(rows),3)):
            obs.update({(sid+offset,t):values for (sid,t),values in part.items()})
            proof['batches'].append(receipt)
    proof['independent']=dict(verdict='pass',comparisons=sum(r['independent']['comparisons'] for r in proof['batches']),
        max_absolute_gap=max(r['independent']['max_absolute_gap'] for r in proof['batches']))
    source_path=Path(__file__).with_name('sources.json');sources=json.loads(source_path.read_text())
    save(args.out/'sources.json',sources)
    short=dict(c,months=3);macrocal=calendar(3)
    macrocal['season']=[1.]*4
    values=[r['value'] for r in sources['bankruptcy']['series']]
    macrocal['market']=[v/values[0] for v in values]+[values[-1]/values[0]]
    _,proof['observed_macro_input']=execute(args.out/'observed-macro',short,
        [scenario('neutral'),scenario('observed_macro',macro=1.)],macrocal,args.executable,args.threads,args.resume)
    proof['scripts']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in Path(__file__).parent.glob('*.py')}
    proof['scope']='Synthetic expected-FTE monthly model; no real-data calibration or forecast validation.'
    save(args.out/'validation.json',proof)
    summaries=summarize(obs,c,rows);save(args.out/'annual-summary.json',summaries)
    lines=['Synthetic T&R monthly pilot: 150 initial employees (135 billable + 15 support).',proof['scope'],
        f"{len(rows)} scenarios; {proof['independent']['comparisons']:,} independent comparisons.",
        'Scenario | Year | Revenue | EBITDA | Margin | Ending expected FTE | Billable utilization']
    for r in summaries:
        if r['year'] in (1,5):lines.append(f"{r['scenario']} | {r['year']} | ${r['revenue']/1e6:.2f}m | ${r['ebitda']/1e6:.2f}m | {r['ebitda_margin']:.1%} | {r['ending_expected_fte']:.1f} | {r['billable_role_utilization']:.1%}")
    (args.out/'summary.txt').write_text('\n'.join(lines)+'\n')
    print('\n'.join(lines),flush=True)


if __name__=='__main__':main()
