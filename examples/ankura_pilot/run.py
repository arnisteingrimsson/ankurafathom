"""Run generated pilot models through CLI or Python and summarize five-year scenarios."""
import argparse
import csv
import json
import math
from pathlib import Path
import subprocess
import sys

def summarize(plan, all_rows):
    monthly={}
    for practice,rows in all_rows:
        by_key={}
        for row in rows:
            key=(int(row['scenario']),int(row['replication']),int(row['time']))
            assert row['time']==key[2] and key[1]==0
            values=by_key.setdefault(key,{})
            assert row['output_id'] not in values and math.isfinite(row['value'])
            values[row['output_id']]=row['value']
        for scenario in map(int,plan['scenario_names']):
            last=by_key[(scenario,0,0)]
            for month in range(1,61):
                current=by_key[(scenario,0,month)]
                values={key.removeprefix('cumulative_'):value-last[key] for key,value in current.items() if key.startswith('cumulative_')}
                values.update({key:current[key] for key in ('headcount','tm_backlog','fixed_backlog')})
                monthly[(scenario,practice,month)]=values;last=current
    practices=[model['practice'] for model in plan['models']]
    annual=[]
    for scenario,name in plan['scenario_names'].items():
        scenario=int(scenario)
        for year in range(1,6):
            for practice in [*practices,'FIRM']:
                selected=practices if practice=='FIRM' else [practice]
                rows=[monthly[(scenario,p,m)] for p in selected for m in range((year-1)*12+1,year*12+1)]
                ending=[monthly[(scenario,p,year*12)] for p in selected]
                revenue=sum(r['revenue'] for r in rows);profit=sum(r['profit'] for r in rows)
                paid=sum(r['paid_hours'] for r in rows);delivered=sum(r['actual_delivery'] for r in rows)
                annual.append(dict(scenario_id=scenario,scenario=name,practice=practice,year=year,
                    revenue_usd=revenue,cost_usd=sum(r['cost'] for r in rows),operating_profit_usd=profit,
                    margin=profit/revenue if revenue else 0,utilization=delivered/paid if paid else 0,
                    headcount_fte=sum(r['headcount'] for r in ending),
                    tm_backlog_baseline_hours=sum(r['tm_backlog'] for r in ending),
                    fixed_backlog_baseline_hours=sum(r['fixed_backlog'] for r in ending)))
    summaries=[]
    for scenario,name in plan['scenario_names'].items():
        scenario=int(scenario)
        deltas=[sum(monthly[(scenario,p,m)]['profit']-monthly[(0,p,m)]['profit'] for p in practices) for m in range(1,61)]
        total=0;had_deficit=False;cumulative=[];deficits=[]
        for delta in deltas:
            total+=delta;had_deficit|=total<0;cumulative.append(total);deficits.append(had_deficit)
        payback=next((i+1 for i,value in enumerate(cumulative) if deficits[i] and value>=0 and all(v>=0 for v in cumulative[i:])),None)
        summaries.append(dict(scenario_id=scenario,scenario=name,
            incremental_npv_usd=sum(d/(1.10**((i+1)/12)) for i,d in enumerate(deltas)),payback_month=payback))
    return monthly,annual,summaries

def run(inputs,output,*,executable=None,package=None,threads=1):
    import pyarrow.parquet as pq
    inputs=Path(inputs).resolve();output=Path(output).resolve()
    plan=json.loads((inputs/'pilot.json').read_text());output.mkdir(parents=True,exist_ok=False)
    if package:
        sys.path.insert(0,str(Path(package).resolve()))
    if executable is None:
        import ankurafathom as af
        experiment=af.Experiment((inputs/plan['experiment']).read_bytes())
    all_rows=[];receipts=[];validations=[];explanations=[];diagrams=[]
    for index,model in enumerate(plan['models']):
        path=inputs/model['model'];result=output/f'practice-{index}.parquet'
        if executable is not None:
            validation_path=output/f'practice-{index}.validation.json'
            subprocess.run([str(Path(executable).resolve()),'check',str(path),'--experiment',str(inputs/plan['experiment']),
                            '--threads',str(threads),'--out',str(validation_path)],check=True,capture_output=True,text=True)
            validation=json.loads(validation_path.read_text())
            if validation['verdict']!='pass': raise ValueError('pilot requires passing declared validation')
            validations.append(validation)
            subprocess.run([str(Path(executable).resolve()),'run',str(path),'--experiment',str(inputs/plan['experiment']),
                            '--threads',str(threads),'--out',str(result)],check=True,capture_output=True,text=True)
            table=pq.read_table(result)
            receipts.append(json.loads(Path(str(result)+'.manifest.json').read_text()))
            if validation['inputs']!=receipts[-1]['inputs'] or validation['result']!=receipts[-1]['result']:
                raise ValueError('validation and run identities differ')
        else:
            loaded=af.Model.from_json(path.read_bytes(),base_directory=path.parent)
            table=af.run(loaded,experiment,threads=threads,provenance=True)
            pq.write_table(table,result)
            # Preserve the exact API receipt; API-memory replay is a separate gate.
            receipt=json.loads(table.schema.metadata[b'ankurafathom.manifest'])
            Path(str(result)+'.manifest.json').write_text(json.dumps(receipt,indent=2)+'\n')
            receipts.append(receipt)
        if executable is not None:
            stem=f'practice-{index}-structure'
            visualized=subprocess.run([str(Path(executable).resolve()),'viz',str(path),
                '--out',str(output/stem),'--output','cumulative_profit',
                '--manifest',str(result)+'.manifest.json'],check=True,capture_output=True,text=True)
            graph=json.loads(visualized.stdout)
            if graph['manifest_id']!=receipts[-1]['id'] or graph['model_sha256']!=receipts[-1]['inputs']['model']['canonical_sha256']:
                raise ValueError('diagram/run identity mismatch')
            diagrams.append(dict(practice=model['practice'],path=stem+'/index.html'))
            for scenario,label in ((0,'baseline'),(1,'copilot')):
                for metric in ('revenue','profit'):
                    stem=f'practice-{index}-{label}-{metric}'
                    explained=subprocess.run([str(Path(executable).resolve()),'explain',str(result),
                        '--output','cumulative_'+metric,'--from','0','--at','12','--scenario',str(scenario),
                        '--threads',str(threads),'--format','html','--out',str(output/(stem+'.html'))],
                        check=True,capture_output=True,text=True)
                    receipt=json.loads(explained.stdout)
                    if receipt['manifest_id']!=receipts[-1]['id']: raise ValueError('explanation/run identity mismatch')
                    (output/(stem+'.json')).write_text(json.dumps(receipt,indent=2)+'\n')
                    explanations.append(dict(practice=model['practice'],scenario=label,metric=metric,path=stem+'.html'))
        all_rows.append((model['practice'],table.select(['scenario','replication','time','output_id','value']).to_pylist()))
    monthly,annual,summaries=summarize(plan,all_rows)
    for filename,rows in [('yearly.csv',annual),('summary.csv',summaries)]:
        with (output/filename).open('w',newline='') as file:
            writer=csv.DictWriter(file,fieldnames=rows[0]);writer.writeheader();writer.writerows(rows)
    evidence=dict(scope=plan['scope'],interface='cli' if executable else 'python',
        source_sha256=plan['source_sha256'],model_manifests=[m['id'] for m in receipts],
        practices=len(plan['models']),scenarios=len(plan['scenario_names']),months=60,
        monthly_records=len(monthly),annual_records=len(annual),
        validation=dict(verdict='pass' if validations else 'not-run',scope='standalone SD declarations at output grid',
            reports=[f'practice-{i}.validation.json' for i in range(len(validations))],
            samples=sum(c.get('samples',0) for v in validations for c in v['checks'])),explanations=explanations,diagrams=diagrams)
    (output/'run.json').write_text(json.dumps(evidence,indent=2)+'\n')
    lines=['# Synthetic Ankura scenario preview','',
        'Five years, two fee models, demand-constrained delivery and explicit intervention costs. '
        'Inputs and effects are synthetic assumptions; this is not a forecast of Ankura.', '',
        '| Scenario | Incremental firm NPV (10% discount) | Payback month |',
        '|---|---:|---:|']
    for row in summaries[:16]:
        lines.append(f"| {row['scenario']} | ${row['incremental_npv_usd']:,.0f} | {row['payback_month'] or '—'} |")
    if validations:
        lines += ['', f"Native declared validation: **pass**, {evidence['validation']['samples']:,} rule evaluations. "
            'Accounting, fee-specific work balances, capacity, nonnegative stocks and cumulative monotonicity '
            'were checked across all scenarios. Per-practice validation JSON records exact inputs, tolerances and coverage.', '']
    else:
        lines += ['', 'Native declared validation was not run by this Python-only preview; run the CLI check separately.', '']
    if diagrams:
        lines += ['', 'Model structure (static declarations; run inputs verified):', '']
        lines += [f"- [{v['practice']}: stock/flow and profit dependencies]({v['path']})" for v in diagrams]
    if explanations:
        lines += ['', 'First-year explanations (verified re-execution):', '', '| Practice | Scenario | Output |', '|---|---|---|']
        lines += [f"| {v['practice']} | {v['scenario']} | [{v['metric']}]({v['path']}) |" for v in explanations]
    lines += ['', 'Yearly revenue, operating profit, margin, utilization, headcount and fee-specific backlog '
        'are in `yearly.csv`. Native observations and provenance are in the per-practice Parquet files.', '',
        'This reproduces the bounded Phase A economics model through the general SD runtime. '
        'The broader M7 validation suite, hybrid explanation, hybrid structure diagrams and general native reporting and full M8 acceptance review remain open. Hiring, attrition, '
        'stochastic deal pipelines and estimated causal effects are not represented in this preview.', '']
    (output/'report.md').write_text('\n'.join(lines))
    return evidence

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('inputs',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--executable',type=Path);p.add_argument('--package',type=Path);p.add_argument('--threads',type=int,default=1)
    a=p.parse_args();print(json.dumps(run(a.inputs,a.output,executable=a.executable,package=a.package,threads=a.threads),indent=2))
