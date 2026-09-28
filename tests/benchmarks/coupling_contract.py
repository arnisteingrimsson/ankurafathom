"""Off-grid teacup and constant-stock coupling against independent discrete answers."""
import argparse
import json
import math
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('executable',type=Path);p.add_argument('destination',type=Path);a=p.parse_args()
a.destination.mkdir(parents=True,exist_ok=False)
raw=subprocess.check_output([str(a.executable.resolve())],text=True)
(a.destination/'native.json').write_text(raw);runs=json.loads(raw)
assert len(runs)==8 and {(r['decay'],r['dt']) for r in runs}=={(b,h) for b in (False,True) for h in (.5,.25,.125,.0625)}
comparisons=0;errors=[]
for run in runs:
    dt=run['dt'];times=sorted({i*dt for i in range(1,int(4/dt)+1)}|{.13,.37,.91,1.61,2.03,3.77})
    assert [s['time'] for s in run['samples']]==times
    previous=0.;factors=[];maximum=0.
    for sample in run['samples']:
        t=sample['time'];factors.append(1-.4*(t-previous));previous=t
        expected=60*math.prod(factors) if run['decay'] else 2+3*t
        assert sample['clock']==t
        for key in ('coupled','standalone'):
            assert math.isclose(sample[key],expected,rel_tol=2e-13,abs_tol=2e-13),(run['decay'],dt,t,key)
            comparisons+=1
        maximum=max(maximum,abs(sample['coupled']-60*math.exp(-.4*t)))
    if run['decay']:errors.append(maximum)
assert all(b<a for a,b in zip(errors,errors[1:])),errors
report=dict(verdict='pass',cases=8,comparisons=comparisons,teacup_max_continuous_errors=errors,
    scope='Native DEVS ClockedSD and standalone Euler on identical merged event/tick mesh; independent stability-product and linear-stock oracles',
    limitation='Zero pulses split numerical integration intervals. Coarse fixed-grid Euler is not expected to be bit-identical to event-refined Euler.')
(a.destination/'validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
