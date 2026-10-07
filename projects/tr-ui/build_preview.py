"""Create a traceable saved-example pack using the shared application metric rules."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import sys
import pyarrow.parquet as pq
from server import package,ROOT,HERE
from application.fathom_service.contracts import describe
from application.fathom_service.metrics import Results,compare

PAIRS={'ai':('baseline_responsive','ai20%_responsive_erosion50%'),
       'growth':('baseline_responsive','ai20_pipeline20'),
       'bd':('baseline_responsive','ai20%_responsive_bd'),
       'freeze':('baseline_freeze','ai20%_freeze_erosion50%'),
       'none':('baseline_responsive','baseline_responsive')}


def build(source,engine):
    proof=json.loads((source/'validation.json').read_text())
    if proof['independent']['verdict']!='pass':raise ValueError('Source run did not pass')
    scenarios=json.loads((source/'scenarios.json').read_text());results={};evidence={}
    needed=set(x for pair in PAIRS.values() for x in pair)
    with tempfile.TemporaryDirectory(prefix='tr-ui-preview-') as tmp:
        modelpath=source/'batch-00/inputs/model.json';directory=Path(tmp)/'package'
        metadata=package(modelpath,directory)
        description=describe(json.loads(modelpath.read_text()),json.loads((directory/'descriptor.json').read_text()),modelpath.parent)
        metrics=[m['id'] for m in description['metrics']]
        for offset in range(0,len(scenarios),3):
            subset=scenarios[offset:offset+3]
            if not any(s['name'] in needed for s in subset):continue
            folder=source/f'batch-{offset//3:02d}';file=folder/'results.parquet'
            subprocess.run([str(engine),'verify-results',str(file),'--embedded'],check=True,capture_output=True)
            receipt=json.loads((folder/'validation.json').read_text())
            if receipt['independent']['verdict']!='pass':raise ValueError('Unverified batch')
            reader=Results(description,pq.read_table(file,columns=['scenario','replication','time','output_id','value']).to_pylist())
            for sid,s in enumerate(subset):
                if s['name'] in needed:
                    results[s['name']]=(reader,sid,s)
                    evidence[s['name']]=dict(manifest_id=receipt['native']['manifest_id'],
                        result_identity=receipt['one_thread_replay']['result'],batch=folder.name)
        examples={}
        for key,(left,right) in PAIRS.items():
            b,bs,_=results[left];c,cs,s=results[right]
            controls={k:s[k] for k in ('ai','erosion','bd','macro','fixed_shift','demand_scale','success_gate')}
            controls.update(policy_hold=int(s['policy']=='hold'),policy_responsive=int(s['policy']=='responsive'))
            request=dict(baseline_run=left,candidate_run=right,baseline_scenario=bs,candidate_scenario=cs,metrics=metrics)
            annual=[compare(b,c,dict(request,**{'from':12*(y-1),'to':12*y})) for y in range(1,6)]
            valuation=compare(b,c,dict(request,**{'from':0,'to':60},valuation=dict(metric='cash_flow',annual_discount_rate=.10,time_units_per_year=12,initial_incremental_cash_flow=0)))['valuation']
            examples[key]=dict(controls=controls,annual=annual,valuation=valuation,
                provenance=dict(source='Verified synthetic pilot · 2026-09-30',baseline=evidence[left],candidate=evidence[right],
                    calculation='Shared application metrics v1.0; full monthly observations',synthetic=True))
        return dict(schema_version=1,recorded_date='2026-09-30',project=metadata,examples=examples,
                    source_validation_sha256=hashlib.sha256((source/'validation.json').read_bytes()).hexdigest(),
                    input_sha256={name:hashlib.sha256((modelpath.parent/name).read_bytes()).hexdigest() for name in ['model.json',*sorted({b['source'] for b in json.loads(modelpath.read_text())['data']})]})


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=ROOT/'artifacts/tr-pilot-150-validated-20260930');p.add_argument('--engine',type=Path,default=ROOT/'build-arrow/fathom');p.add_argument('--out',type=Path,default=HERE/'preview.json');a=p.parse_args()
    a.out.write_text(json.dumps(build(a.source,a.engine),indent=2,allow_nan=False)+'\n');print(a.out)
