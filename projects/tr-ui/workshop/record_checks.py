"""Execute project checks and retain exact commands, outputs and source identities."""
from pathlib import Path
import copy,json,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[3];HERE=ROOT/'projects/tr-ui';sys.path.insert(0,str(ROOT));sys.path.insert(0,str(HERE))
from workshop.spec import DEFAULT
from application.fathom_service.contracts import canonical,digest
out=ROOT/'artifacts/tr-operating-acceptance';out.mkdir(parents=True,exist_ok=True)
c=copy.deepcopy(DEFAULT);(out/'default.json').write_text(json.dumps(c))
commands=[
 [str(ROOT/'.venv-runtime/bin/python'),'projects/tr-ui/workshop/test_workshop.py'],
 [str(HERE/'native/build/workshop-tests-sanitize'),str(out/'default.json')],
 ['node','--check','projects/tr-ui/workshop/app.js'],
 [str(ROOT/'.venv-runtime/bin/python'),'projects/tr-ui/test_project.py'],
]
receipt=dict(created_at=time.time(),checks=[],scope='Project hybrid model, contracts and existing monthly registration. Not a full engine regression or empirical validation.')
for command in commands:
 t=time.monotonic();r=subprocess.run(command,cwd=ROOT,capture_output=True,text=True,timeout=180);receipt['checks'].append(dict(command=command,exit_code=r.returncode,seconds=time.monotonic()-t,stdout=r.stdout,stderr=r.stderr));print(command[-1],r.returncode,flush=True)
# Run the entire 90-day operating model under ASan and UBSan.
t=time.monotonic();r=subprocess.run([str(HERE/'native/build/tr-workshop-sanitize')],input=json.dumps(c)+'\n'+json.dumps(dict(until=2160))+'\n',text=True,capture_output=True,timeout=180)
lines=[json.loads(line) for line in r.stdout.splitlines()];ok=r.returncode==0 and len(lines)==2 and lines[-1].get('complete') and lines[-1].get('checks_passed')
receipt['checks'].append(dict(command=['tr-workshop-sanitize','stdin:default.json, until=2160'],exit_code=r.returncode,passed=bool(ok),seconds=time.monotonic()-t,stderr=r.stderr,final_metrics=lines[-1].get('metrics') if lines else None));print('90-day sanitized run',ok,flush=True)
paths=[*HERE.joinpath('workshop').glob('*.py'),*HERE.joinpath('workshop').glob('*.js'),*HERE.joinpath('workshop').glob('*.html'),*HERE.joinpath('workshop').glob('*.css'),HERE/'native/workshop_runner.cpp',HERE/'native/workshop_tests.cpp',HERE/'server.py',*[ROOT/'include/ankurafathom'/p for p in ['abm/sync_population.hpp','devs/simulator.hpp','sd/model.hpp','rng/philox.hpp']]]
receipt['sources']={str(p.relative_to(ROOT)):digest(p.read_bytes()) for p in paths};receipt['passed']=all(c['exit_code']==0 and c.get('passed',True) for c in receipt['checks']);(out/'verification.json').write_bytes(canonical(receipt));print('Receipt:',out/'verification.json',flush=True)
if not receipt['passed']:raise SystemExit(1)
