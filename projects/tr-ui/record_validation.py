"""Run the project checks and retain commands, output, exit codes and source identities."""
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import sys

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]

def hashes():
    paths=[p for p in HERE.iterdir() if p.suffix in ('.py','.js','.mjs','.html','.css')]
    paths+=list((HERE/'native').glob('*.cpp'))
    paths+=[ROOT/'include/ankurafathom/ir/model.hpp',ROOT/'src/ir.cpp']
    import os
    return {os.path.relpath(p,HERE):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(paths)}

if __name__=='__main__':
    before=hashes();records=[]
    for command in ([sys.executable,'projects/tr-ui/test_project.py'],[sys.executable,'projects/tr-ui/test_validation.py'],
                    [sys.executable,'projects/tr-ui/test_live.py'],[str(HERE/'native/build/observer-tests'),str(ROOT)],
                    [str(HERE/'native/build/observer-tests-sanitize'),str(ROOT)],
                    ['node','--check','projects/tr-ui/live.js'],['node','projects/tr-ui/test_ui.mjs'],['node','--check','projects/tr-ui/app.js'],['node','--check','projects/tr-ui/validation.js']):
        started=datetime.datetime.now(datetime.timezone.utc).isoformat()
        result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True,timeout=120)
        records.append(dict(command=command,started_utc=started,exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr))
        print(result.stdout+result.stderr,end='')
    receipt=dict(created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),source_sha256=before,
                 source_unchanged_during_checks=before==hashes(),checks=records,
                 scope='Project contracts, live compute barriers and native parity, native observer normal/sanitizer checks, historical evaluation arithmetic and saved-result integration. Not a full engine regression or observed business validation.',
                 author_model_identity=None,reviewer_model_identity=None,
                 provenance='Commands executed by record_validation.py; author/reviewer AI identities were not supplied.')
    (HERE/'verification.json').write_text(json.dumps(receipt,indent=2)+'\n')
    sys.exit(0 if receipt['source_unchanged_during_checks'] and all(c['exit_code']==0 for c in records) else 1)
