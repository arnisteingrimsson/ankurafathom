#!/usr/bin/env python3
"""History-order evidence: a bounded pass and explicit conservation failures."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import shutil
import tempfile
from importlib.metadata import version
import pysd
from pysd.py_backend.statefuls import DelayN
from generate import PACKAGES
from generate_delayn import run, COLUMNS

ROOT=Path(__file__).resolve().parent


def generate():
    versions={name:version(name) for name in PACKAGES}
    if versions!=PACKAGES:raise RuntimeError('install pinned oracle packages')
    source=ROOT/'corpus/tests/delays/test_delays.mdl'
    with tempfile.TemporaryDirectory() as directory:
        target=Path(directory)/source.name;shutil.copyfile(source,target)
        result=pysd.read_vensim(str(target)).run(return_columns=['OutputDelayN'])
    with source.with_name('output.tab').open(newline='') as f:
        historical=list(csv.DictReader(f,delimiter='\t'))
    values=result['OutputDelayN'].tolist();times=result.index.tolist()
    if len(historical)!=len(values):raise RuntimeError('historical row count')
    errors=[]
    for row,time,value in zip(historical,times,values):
        expected=float(row['OutputDelayN'])
        if float(row['Time'])!=time or not math.isclose(value,expected,rel_tol=1e-5,abs_tol=5e-6):
            raise RuntimeError('historical order-2 reference mismatch')
        errors.append(abs(value-expected))
    cases=[]
    # Zero input: integral(output)+remaining material must equal 2*4 = 8.
    # Long horizon removes ambiguity about a finite tail; inspect the tail too.
    for dt in (.125,.0625):
        for order in (1,2,3,5):
            for after in (2.,4.,6.):
                duration=[4.]
                delay=DelayN(lambda:0.,lambda:duration[0],lambda:2.,lambda:order,lambda:dt,'probe')
                delay.initialize();outputs=[];samples=[];max_defect=0.
                count=round(256/dt);change=round(1/dt)
                for k in range(count):
                    duration[0]=4. if k<change else after
                    output=float(delay());outputs.append(output*dt)
                    if k in (0,change-1,change,change+1,change+2,count-1):
                        samples.append(dict(tick=k,output=output,scaled_stages=delay.state.tolist(),
                                            durations=delay.times.tolist()))
                    delay.update(delay.state+dt*delay.ddt())
                    remaining=float(sum(delay.state)/order)
                    max_defect=max(max_defect,abs(8-math.fsum(outputs)-remaining))
                emitted=math.fsum(outputs);remaining=float(sum(delay.state)/order)
                cases.append(dict(order=order,dt=dt,duration_after=after,initial_quantity=8.,
                    emitted=emitted,remaining=remaining,balance_error=8-emitted-remaining,
                    max_balance_error=max_defect,conserves=max_defect<1e-10,samples=samples))
    # PySD allocates its duration array using the initial scalar dtype. Keep a
    # separate diagnostic for truncation; the conformance fixture uses 3.0.
    integer_tau=[4]
    integer_delay=DelayN(lambda:0.,lambda:integer_tau[0],lambda:2.,lambda:2,lambda:.125,'integer_probe')
    integer_delay.initialize();integer_tau[0]=4.5
    integer_delay.update(integer_delay.state+.125*integer_delay.ddt())
    integer_diagnostic=dict(requested_duration=4.5,stored_duration=float(integer_delay.times[0]))
    return dict(generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        versions=versions,columns=COLUMNS,history2=run(ROOT/'fixtures/delayn_history2.xmile'),
        historical_order2=dict(source='corpus/tests/delays/test_delays.mdl',
            source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),times=times,values=values,
            reference='corpus/tests/delays/output.tab',
            reference_sha256=hashlib.sha256(source.with_name('output.tab').read_bytes()).hexdigest(),
            max_absolute_error=max(errors),relative_tolerance=1e-5,absolute_tolerance=5e-6),
        conservation=cases,integer_duration_diagnostic=integer_diagnostic,
        history_all_orders_accepted=all(c['conserves'] for c in cases),
        scope='Only order 2 is admitted as a native history delay. Other orders are diagnostic failures, not accepted kernels.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify',action='store_true')
    parser.add_argument('--check-all-orders',action='store_true',help='exit 1 if any conservation gate fails')
    args=parser.parse_args();result=generate();path=ROOT/'history-reference.json'
    if args.verify:
        def check(new,old):
            if isinstance(new,dict):
                if new.keys()!=old.keys():raise RuntimeError('reference fields changed')
                for k in new:check(new[k],old[k])
            elif isinstance(new,list):
                if len(new)!=len(old):raise RuntimeError('reference shape changed')
                for a,b in zip(new,old):check(a,b)
            elif isinstance(new,float):
                if not math.isclose(new,old,rel_tol=2e-12,abs_tol=2e-12):raise RuntimeError('reference values changed')
            elif new!=old:raise RuntimeError('reference metadata changed')
        check(result,json.loads(path.read_text()))
        print('History evidence regenerated; this verifies the report, not acceptance of failed orders')
    elif not args.check_all_orders:path.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    failed=[c for c in result['conservation'] if not c['conserves']]
    print(f'Conservation: {len(result["conservation"])-len(failed)}/{len(result["conservation"])} pass; all-order acceptance: {result["history_all_orders_accepted"]}')
    if args.check_all_orders and failed:raise SystemExit(1)


if __name__=='__main__':main()
