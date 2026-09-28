#!/usr/bin/env python3
"""Direct PySD explicit-stock oracle; also retain its DELAYN dialect discrepancy."""
import argparse
import hashlib
from importlib.metadata import version
import json
import math
from pathlib import Path
import shutil
import tempfile
import pysd
from generate import PACKAGES

ROOT=Path(__file__).resolve().parent
NAMES=['one_default','one_explicit','three','five','gradual','feedback','inner','nested','alias']
COLUMNS=['clock_stock','forcing','tau']+[x for name in NAMES for x in (name,'stock_'+name)]

def run(source):
    with tempfile.TemporaryDirectory() as directory:
        copy=Path(directory)/source.name;shutil.copyfile(source,copy)
        result=pysd.read_xmile(str(copy)).run(return_columns=COLUMNS)
        values=result[COLUMNS].to_numpy().tolist()
        if not all(math.isfinite(x) for row in values for x in row):raise RuntimeError('nonfinite oracle')
        return dict(source='fixtures/'+source.name,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                    times=result.index.tolist(),values=values)

def generate():
    versions={name:version(name) for name in PACKAGES}
    if versions!=PACKAGES:raise RuntimeError('install pinned oracle packages')
    cascade=run(ROOT/'fixtures/delayn_cascade_expanded.xmile')
    history=run(ROOT/'fixtures/delayn_variable.xmile')
    gap=max(abs(a-b) for left,right in zip(cascade['values'],history['values']) for a,b in zip(left,right))
    if gap<.1:raise RuntimeError('expected the variable-duration dialects to disagree')
    return dict(oracle='PySD explicit Euler stocks and flows, current-duration material cascade',
                versions=versions,columns=COLUMNS,cascade=cascade,history_diagnostic=history,
                maximum_dialect_gap=gap,generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--verify',action='store_true');args=parser.parse_args()
    result=generate();p=ROOT/'delayn-reference.json'
    if args.verify:
        old=json.loads(p.read_text())
        def compare(a,b):
            if isinstance(a,dict):
                if a.keys()!=b.keys():raise RuntimeError('oracle fields changed')
                for key in a:compare(a[key],b[key])
            elif isinstance(a,list):
                if len(a)!=len(b):raise RuntimeError('oracle shape changed')
                for x,y in zip(a,b):compare(x,y)
            elif isinstance(a,float):
                if not math.isclose(a,b,rel_tol=2e-12,abs_tol=2e-12):raise RuntimeError('oracle values changed')
            elif a!=b:raise RuntimeError('oracle provenance changed')
        compare(result,old);print('Explicit-cascade oracle and history-dialect diagnostic verified')
    else:p.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n');print('Wrote',p.name)
if __name__=='__main__':main()
