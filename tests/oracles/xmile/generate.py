#!/usr/bin/env python3
"""Pinned PySD oracle: reads source XMILE directly; never imports Fathom code."""
import argparse
import hashlib
from importlib.metadata import version
import json
import math
from pathlib import Path
import shutil
import tempfile
import warnings

import pysd

ROOT = Path(__file__).resolve().parent
PACKAGES = {'pysd': '3.14.3', 'numpy': '2.4.3', 'scipy': '1.17.1',
            'pandas': '3.0.6', 'xarray': '2026.7.0', 'lxml': '6.1.3',
            'parsimonious': '0.11.0', 'black': '26.5.1'}
STOCKS = ['stock_'+s for s in ('smooth_one','smooth_three','delay_one','delay_three',
          'smooth_default','smooth_three_default','delay_default','delay_three_default',
          'nested','feedback','shared','independent','stock_default')]
CASES = {'stateful.xmile': STOCKS, 'lookups.xmile': ['clamped','extrapolated','composition'],
         'variable_delays.xmile': ['clock_stock', 'stock_smth1_default', 'stock_smth1_explicit', 'stock_smth3_default', 'stock_smth3_explicit', 'stock_delay1_default', 'stock_delay1_explicit', 'stock_delay3_default', 'stock_delay3_explicit', 'stock_nested', 'stock_stock_tau', 'stock_feedback_tau']}

CASES['extended_delays.xmile'] = ['stock_smthn_2_explicit', 'stock_smthn_2_default', 'stock_delayn_2_explicit', 'stock_delayn_2_default', 'stock_smthn_5_explicit', 'stock_smthn_5_default', 'stock_delayn_5_explicit', 'stock_delayn_5_default', 'stock_smthn_8_explicit', 'stock_smthn_8_default', 'stock_delayn_8_explicit', 'stock_delayn_8_default', 'stock_fixed_one', 'stock_fixed_three', 'stock_fixed_default', 'stock_fixed_variable', 'stock_fixed_nested', 'stock_variable_smooth']


def generate():
    actual = {name: version(name) for name in PACKAGES}
    if actual != PACKAGES:
        raise RuntimeError(f'Install pinned requirements: {actual}')
    cases = []
    for filename, columns in CASES.items():
        source = ROOT/'fixtures'/filename
        with tempfile.TemporaryDirectory() as directory:
            copy = Path(directory)/filename
            shutil.copyfile(source, copy)
            with warnings.catch_warnings():
                # PySD warns on intentionally exercised out-of-range queries.
                warnings.filterwarnings('ignore', message='(?s).*extrapolating data.*', category=UserWarning)
                model = pysd.read_xmile(str(copy))
                result = model.run(return_columns=columns)
            values = result[columns].to_numpy().tolist()
            if not all(math.isfinite(x) for row in values for x in row):
                raise RuntimeError(f'{filename}: nonfinite oracle')
            cases.append(dict(source='fixtures/'+filename, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                              stocks=columns, times=result.index.tolist(), values=values))
    return dict(oracle='PySD direct XMILE Euler', versions=PACKAGES, cases=cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    generated = generate()
    target = ROOT/'pysd-reference.json'
    if args.verify:
        stored = json.loads(target.read_text())
        if generated['versions'] != stored['versions'] or len(generated['cases']) != len(stored['cases']):
            raise RuntimeError('reference metadata mismatch')
        for new, old in zip(generated['cases'],stored['cases']):
            if {k:v for k,v in new.items() if k!='values'} != {k:v for k,v in old.items() if k!='values'}:
                raise RuntimeError('source/time/column mismatch')
            if len(new['values']) != len(old['values']):
                raise RuntimeError('row count mismatch')
            for a,b in zip(new['values'],old['values']):
                if len(a)!=len(b) or not all(math.isclose(x,y,rel_tol=2e-12,abs_tol=2e-12) for x,y in zip(a,b)):
                    raise RuntimeError('regenerated PySD trajectory mismatch')
        print('Pinned PySD reference regeneration verified')
    else:
        target.write_text(json.dumps(generated,indent=2,allow_nan=False)+'\n')
        print(f'Wrote {target.name}')


if __name__ == '__main__':
    main()
