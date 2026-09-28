"""Pilot IR vs separate Phase A economics implementation and public interfaces."""
import argparse
import csv
import io
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'examples/ankura_pilot'))
from generate import generate
from run import run, summarize

p=argparse.ArgumentParser();p.add_argument('executable',type=Path);p.add_argument('probe',type=Path)
p.add_argument('--package',type=Path);p.add_argument('--schema',action='store_true');a=p.parse_args();exe=a.executable.resolve()
import pyarrow.parquet as pq
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);inputs=root/'inputs';plan=generate(inputs)
    if a.schema:
        from jsonschema import Draft202012Validator
        validator=Draft202012Validator(json.loads((ROOT/'ir/schema/ankurafathom-ir.schema.json').read_text()))
        for model in plan['models']:validator.validate(json.loads((inputs/model['model']).read_text()))
    outputs=[]
    for name,threads,executable in [('cli-1',1,exe),('cli-8',8,exe)]+([('python-32',32,None)] if a.package else []):
        output=root/name;run(inputs,output,executable=executable,package=a.package,threads=threads);outputs.append(output)
    # Native reports must describe the complete grid, never just a successful process exit.
    for output in outputs:
        evidence=json.loads((output/'run.json').read_text())
        if evidence['interface']=='cli':
            assert evidence['validation']['verdict']=='pass' and evidence['validation']['samples']==25184
            assert len(evidence['explanations'])==8 and len(evidence['diagrams'])==2
            for i in range(2):
                graph=json.loads((output/f'practice-{i}-structure/graph.json').read_text())
                manifest=json.loads((output/f'practice-{i}.parquet.manifest.json').read_text())
                assert graph['manifest_id']==manifest['id'] and graph['model_sha256']==manifest['inputs']['model']['canonical_sha256']
                nodes={n['id']:n for n in graph['dependencies']['nodes']}
                assert any(n['kind']=='parameter-data' for n in nodes.values())
                series=[n for n in nodes.values() if n['kind']=='exogenous-series']
                assert series and all(n['declaration'] in manifest['inputs']['data'] for n in series)
                assert any(e['role']=='binds' for e in graph['dependencies']['edges'])
                if a.schema:
                    Draft202012Validator(json.loads((ROOT/'ir/schema/visualization_report.schema.json').read_text())).validate(graph)
                validation=json.loads((output/f'practice-{i}.validation.json').read_text())
                assert len([c for c in validation['checks'] if 'kind' in c])==13
                assert all(c['verdict']=='pass' for c in validation['checks'])
                if a.schema:
                    Draft202012Validator(json.loads((ROOT/'ir/schema/validation_report.schema.json').read_text())).validate(validation)
        else:assert evidence['validation']['verdict']=='not-run'
    # A realistic accounting defect is caught by the model's own declared rule.
    broken=json.loads((inputs/plan['models'][0]['model']).read_text())
    next(c for c in broken['components'] if c['id']=='profit_rate')['expr']='revenue_rate - cost_rate + tool_operating_cost'
    broken_path=inputs/'broken-accounting.ir.json';broken_path.write_text(json.dumps(broken))
    rejected=subprocess.run([str(exe),'check',str(broken_path),'--experiment',str(inputs/plan['experiment'])],capture_output=True,text=True)
    assert rejected.returncode==1,rejected.stderr
    failure=next(c for c in json.loads(rejected.stdout)['checks'] if c['id']=='profit_identity')
    assert failure['code']=='CHECK_ASSERT' and failure['pointer']=='/checks/0'
    assert failure['first_failure']['time']==1 and failure['first_failure']['scenario']==0
    tables=[];comparisons=0;worst=0.
    for index,model in enumerate(plan['models']):
        reference=None
        for output in outputs:
            table=pq.read_table(output/f'practice-{index}.parquet').select(['scenario','replication','time','output_id','value'])
            if reference is None:reference=table
            assert table.equals(reference),(model['practice'],output)
        tables.append((model['practice'],reference.to_pylist()))
    monthly,annual,summaries=summarize(plan,tables)
    # Explanations agree with the independently summarized first-year figures.
    explanation_comparisons=0
    for output in outputs:
        evidence=json.loads((output/'run.json').read_text())
        for link in evidence['explanations']:
            receipt=json.loads((output/Path(link['path']).with_suffix('.json')).read_text())
            row=next(r for r in annual if r['practice']==link['practice'] and r['scenario']==link['scenario'] and r['year']==1)
            expected=row['revenue_usd' if link['metric']=='revenue' else 'operating_profit_usd']
            assert math.isclose(receipt['value'],expected,rel_tol=1e-10,abs_tol=2e-6)
            assert math.isclose(receipt['accounting']['flow_total'],expected,rel_tol=1e-10,abs_tol=2e-6)
            assert abs(receipt['accounting']['rounding_residual'])<2e-6
            assert receipt['verification']['artifact']=='verified'
            if a.schema:Draft202012Validator(json.loads((ROOT/'ir/schema/explanation_report.schema.json').read_text())).validate(receipt)
            explanation_comparisons+=1
    result=subprocess.run([str(a.probe.resolve()),str(inputs/'synthetic_practices.csv')],text=True,capture_output=True,check=True)
    for row in csv.DictReader(io.StringIO(result.stdout)):
        actual=monthly[(int(row['scenario']),row['practice'],int(row['month']))]
        for key in actual:
            expected=float(row[key]);difference=abs(actual[key]-expected);worst=max(worst,difference)
            assert math.isclose(actual[key],expected,rel_tol=1e-10,abs_tol=2e-6),(row,key,actual[key],difference)
            comparisons+=1
    reference_annual=root/'reference-yearly.csv';reference_summary=root/'reference-summary.csv'
    subprocess.run([str(exe),'--input',str(inputs/'synthetic_practices.csv'),'--output',str(reference_annual),'--summary',str(reference_summary)],check=True)
    expected={(r['scenario'],r['practice'],int(r['year'])):r for r in csv.DictReader(reference_annual.open())}
    for row in annual:
        ref=expected[(row['scenario'],row['practice'],row['year'])]
        for key in row.keys()-{'scenario_id','scenario','practice','year'}:
            assert math.isclose(row[key],float(ref[key]),rel_tol=1e-10,abs_tol=2e-6),(key,row,ref)
            comparisons+=1
    expected={r['scenario']:r for r in csv.DictReader(reference_summary.open())}
    for row in summaries:
        ref=expected[row['scenario']]
        assert math.isclose(row['incremental_npv_usd'],float(ref['incremental_npv_usd']),rel_tol=1e-10,abs_tol=2e-6),(row,ref)
        assert str(row['payback_month'] or '')==ref['payback_month'],(row,ref)
    # Every practice's accounting identities hold month by month across all moves.
    practices={r['name']:r for r in csv.DictReader((inputs/'synthetic_practices.csv').open())}
    for scenario in range(16):
        for name,practice in practices.items():
            previous_tm=float(practice['initial_tm_backlog_hours']);previous_fixed=float(practice['initial_fixed_backlog_hours'])
            for month in range(1,61):
                row=monthly[(scenario,name,month)]
                assert math.isclose(row['tm_backlog'],previous_tm+row['tm_won']-row['tm_delivered'],abs_tol=2e-6)
                assert math.isclose(row['fixed_backlog'],previous_fixed+row['fixed_won']-row['fixed_delivered'],abs_tol=2e-6)
                assert math.isclose(row['profit'],row['revenue']-row['cost'],abs_tol=2e-6)
                assert 0<=row['actual_delivery']<=row['paid_hours']*float(practice['delivery_share'])+2e-6
                previous_tm=row['tm_backlog'];previous_fixed=row['fixed_backlog']
    # Independent business extremes must survive the declarative translation.
    edge_source=root/'extremes.csv'
    specimen={key:float(value) for key,value in next(iter(practices.values())).items() if key!='name'}
    specimen.update(fte=10,paid_hours_per_fte_month=160,delivery_share=.75,realized_rate=200,
        monthly_pipeline_hours=800,win_rate=1,fixed_fee_share=0,annual_pay_per_fte=100000,
        variable_cost_per_hour=100,annual_demand_growth=0,ai_task_share=1,
        initial_tm_backlog_hours=0,initial_fixed_backlog_hours=0)
    extreme=[dict(name='TM',**specimen),dict(name='FixedFee',**dict(specimen,fixed_fee_share=1)),
             dict(name='NoDemand',**dict(specimen,monthly_pipeline_hours=0)),dict(name='NoCapacity',**dict(specimen,fte=0))]
    with edge_source.open('w',newline='') as file:
        writer=csv.DictWriter(file,fieldnames=['name',*specimen]);writer.writeheader();writer.writerows(extreme)
    edge_inputs=root/'extreme-inputs';edge_plan=generate(edge_inputs,edge_source)
    edge_output=root/'extreme-output';run(edge_inputs,edge_output,executable=exe,threads=8)
    edge_tables=[(model['practice'],pq.read_table(edge_output/f'practice-{i}.parquet').to_pylist()) for i,model in enumerate(edge_plan['models'])]
    edge_monthly,_,_=summarize(edge_plan,edge_tables)
    for scenario in range(16):
        for month in range(1,61):
            for name in ('NoDemand','NoCapacity'): assert edge_monthly[(scenario,name,month)]['revenue']==0
    assert edge_monthly[(1,'TM',12)]['revenue']<edge_monthly[(0,'TM',12)]['revenue']
    assert math.isclose(edge_monthly[(1,'FixedFee',12)]['revenue'],edge_monthly[(0,'FixedFee',12)]['revenue'],abs_tol=2e-6)
    assert edge_monthly[(1,'FixedFee',12)]['profit']>edge_monthly[(0,'FixedFee',12)]['profit']
    print(f'Pilot: {len(outputs)} interfaces/thread configurations; {comparisons} Phase A value comparisons; '
          f'1,920 monthly accounting/capacity gates; 16 NPV/payback comparisons; '
          f'1,920 zero-demand/capacity and 3 pricing controls; {explanation_comparisons} first-year explanations; maximum monthly absolute gap {worst:.12g}')
