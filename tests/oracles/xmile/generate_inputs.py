#!/usr/bin/env python3
"""Independent exact-rational event enumeration for the explicit next_tick policy."""
import argparse
from fractions import Fraction as F
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parent


def generate():
    cases=[]
    for origin in (0,10,-2):
        for dt in (F('0.1'),F('0.125'),F('0.5'),F(1)):
            for offset in (F('-0.25'),F('0.25'),F('0.5'),F('0.75')):
                first=origin+offset*dt
                for function in ('STEP','RAMP','PULSE'):
                    for period in ((F(0),F('.5'),F('1.5'),F('2.25')) if function=='PULSE' else (F(0),)):
                        amount=F(3) if function=='STEP' else F(-2) if function=='RAMP' else F(-3)
                        expression=f'{function}({float(amount)},{float(first)}'+(f',{float(period*dt)}' if function=='PULSE' else '')+')'
                        source=f'''<xmile xmlns="http://docs.oasis-open.org/xmile/ns/XMILE/v1.0" version="1.0"><sim_specs method="Euler"><start>{origin}</start><stop>{float(origin+16*dt)}</stop><dt>{float(dt)}</dt></sim_specs><model><variables><stock name="total"><eqn>0</eqn><inflow>rate</inflow></stock><flow name="rate"><eqn>{expression}</eqn></flow></variables></model></xmile>'''
                        events={}
                        if function=='PULSE':
                            event=first
                            # Enumerate event times and assign each to ceil(event/dt).
                            # This does not use the production cumulative-floor algorithm.
                            while event<=origin+16*dt:
                                offset_ticks=(event-origin)/dt
                                bucket=-(-offset_ticks.numerator//offset_ticks.denominator)
                                events[bucket]=events.get(bucket,0)+1
                                if not period:break
                                event+=period*dt
                        times=[];rates=[];totals=[];total=F(0)
                        for k in range(17):
                            time=origin+k*dt
                            rate=amount*events.get(k,0)/dt if function=='PULSE' else amount if function=='STEP' and time>=first else amount*max(F(0),time-first) if function=='RAMP' else F(0)
                            times.append(float(time));rates.append(float(rate));totals.append(float(total));total+=dt*rate
                        cases.append(dict(source_xml=source,source_sha256=hashlib.sha256(source.encode()).hexdigest(),
                                          function=function,times=times,rates=rates,totals=totals))
    return dict(policy='euler_next_tick_quantity_pulse',oracle='Exact rational enumeration of events and Euler sums; no Fathom imports',
                generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),cases=cases)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--verify',action='store_true');args=parser.parse_args()
    result=generate();path=ROOT/'input-reference.json'
    if args.verify:
        if json.loads(path.read_text())!=result:raise RuntimeError('rational input reference changed')
        print('Rational next-tick source reference verified')
    else:path.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')


if __name__=='__main__':main()
