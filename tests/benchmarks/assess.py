"""Join the requested coverage catalog to actual CTest and adapter evidence."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET
from catalog import ROWS

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--junit',type=Path,required=True)
p.add_argument('--adapters',type=Path,required=True,action='append',help='Evidence root; repeat to combine disjoint adapter runs')
p.add_argument('--out',type=Path,required=True)
a=p.parse_args()
root=ET.parse(a.junit).getroot();tests={}
for case in root.findall('.//testcase'):
    name=case.attrib['name']
    assert name not in tests, 'duplicate test name'
    tests[name]='fail' if case.find('failure') is not None or case.find('error') is not None else 'skipped' if case.find('skipped') is not None else 'pass'
assert tests and len(tests)==int(root.attrib['tests']), 'incomplete JUnit file'
evidence={str(a.junit):hashlib.sha256(a.junit.read_bytes()).hexdigest()};rows=[]
for row in ROWS:
    result=dict(row);checks={}
    for test in row['tests']:
        checks[test]=tests.get(test,'missing')
    if row['adapter']:
        paths=[directory/(row['adapter']+'-results')/'validation.json' for directory in a.adapters]
        found=[path for path in paths if path.is_file()]
        assert len(found)<=1, 'ambiguous duplicate adapter evidence: '+row['adapter']
        if found:
            path=found[0]
            report=json.loads(path.read_text());checks[row['adapter']]=report.get('verdict','missing')
            evidence[str(path)]=hashlib.sha256(path.read_bytes()).hexdigest()
        else:checks[row['adapter']]='missing'
    result['checks']=checks
    result['execution']='not_run' if not checks else 'pass' if all(v=='pass' for v in checks.values()) else 'incomplete_or_failed'
    result['coverage']='partial' if checks and row['gap'] else 'declared_scope' if checks else 'unimplemented_or_unassessed'
    rows.append(result)
counts=Counter(r['execution'] for r in rows)
report=dict(verdict='partial',note='Passing executions do not close explicitly listed scope gaps.',
            acceptance_scope='Engine results: numerical accuracy, event timing, state transitions, invariants, convergence and independent reference comparisons',
            out_of_scope=['GUI interactions','Display rendering','Graphical publication artifacts'],
            ctest=dict(total=len(tests),counts=dict(Counter(tests.values()))),
            workload_counts=dict(counts),workloads=rows,evidence_sha256=evidence)
a.out.parent.mkdir(parents=True,exist_ok=True)
with a.out.open('x') as f:json.dump(report,f,indent=2);f.write('\n')
print(json.dumps({k:v for k,v in report.items() if k not in ('workloads','evidence_sha256')}))
if any(v!='pass' for v in tests.values()) or counts['incomplete_or_failed']:
    raise SystemExit(1)
