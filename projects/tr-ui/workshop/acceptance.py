"""Run the first 12-case operating experiment and verify saved run integrity."""
import copy,json,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];sys.path.insert(0,str(ROOT));sys.path.insert(0,str(ROOT/'projects/tr-ui'))
from workshop.spec import DEFAULT
from workshop.service import Workshop
from application.fathom_service.contracts import canonical,digest
out=ROOT/'artifacts/tr-operating-acceptance';w=Workshop(out/'sessions',ROOT/'projects/tr-ui/native/build/tr-workshop')
try:
 e=w.experiment(dict(config=copy.deepcopy(DEFAULT),variable='license_share',values=[0,.1,.2,.3],replications=3));last=-1
 while True:
  e=w.experiment_state(e['id'])
  if e['finished']!=last:print(f"{e['status']}: {e['finished']}/{e['total']}",flush=True);last=e['finished']
  if e['status'] not in ('queued','running'):break
  time.sleep(.25)
 if e['status']!='completed':raise RuntimeError(e)
 for result in e['results']:
  p=out/'sessions'/result['session']/'session.json';record=json.loads(p.read_text());assert digest(canonical({k:v for k,v in record.items() if k!='record_sha256'}))==record['record_sha256'];prev=None
  for f in record['frames']:
   assert f['previous_sha256']==prev;assert digest(canonical({k:v for k,v in f.items() if k!='sha256'}))==f['sha256'];assert f['checks_passed'];prev=f['sha256']
 summary=dict(scope='12 synthetic 90-day runs: four license shares × three paired seeds; not evidence of real AI effects.',experiment=e,integrity='All run and frame hashes verified',observations=sum(len(json.loads((out/'sessions'/r['session']/'session.json').read_text())['frames']) for r in e['results']))
 (out/'experiment.json').write_bytes(canonical(summary));print('Completed; receipt:',out/'experiment.json',flush=True)
finally:w.close()
