"""Typed ABM CLI against hand recurrences, event histories and invalid contracts."""
import copy
import csv
import io
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def word(seed, scenario, replication, agent, generation, stream):
    mask=(1<<32)-1
    c=[agent & mask, (agent>>32)|(stream<<16), scenario|(replication<<16), generation]
    k=[seed & mask, seed>>32]
    for _ in range(10):
        a,b=0xD2511F53*c[0],0xCD9E8D57*c[2]
        c=[(b>>32)^c[1]^k[0], b & mask, (a>>32)^c[3]^k[1], a & mask]
        k=[(k[0]+0x9E3779B9)&mask,(k[1]+0xBB67AE85)&mask]
    return c[0]


def main():
    exe = str(Path(sys.argv[1]).resolve())
    sync = json.loads((ROOT / 'models/typed_abm_sync.ir.json').read_text())
    asynchronous = json.loads((ROOT / 'models/typed_abm_async.ir.json').read_text())
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'model.json'

        def call(model, mode='run', valid=True):
            path.write_text(json.dumps(model))
            result = subprocess.run([exe, mode, str(path)], capture_output=True, text=True)
            if not valid:
                assert result.returncode != 0, 'invalid model accepted'
                error = json.loads(result.stderr)['diagnostics'][0]
                assert error['code'].startswith('IR_') and error['pointer'].startswith('/'), error
                return
            assert result.returncode == 0, result.stderr
            if mode == 'lint':
                return
            return {(float(r['time']), r['output_id']): float(r['value']) for r in csv.DictReader(io.StringIO(result.stdout))}

        rows = call(sync)
        a, b = 1, 3
        for k in range(5):
            assert rows[k/2, 'first'] == a and rows[k/2, 'total'] == a+b
            assert rows[k/2, 'ready'] == int(k % 2 == 0) and rows[k/2, 'active'] == 2
            a, b = 2*(a+2), 2*(b+2)
        baseline = call(asynchronous)
        for k in range(9):
            t = k/2
            assert baseline[t, 'state'] == (0 if t < .5 else 1 if t < 2.5 else 2)
            assert baseline[t, 'completed'] == int(t >= 1.5)+int(t >= 2.5)
            assert baseline[t, 'busy'] == int(.5 <= t < 2.5) and baseline[t, 'active'] == 2
        reordered = copy.deepcopy(asynchronous)
        reordered['components'][0]['messages'].reverse()
        reordered['components'][0]['chart']['transitions'].reverse()
        assert call(reordered) == baseline
        dense = copy.deepcopy(asynchronous); dense['time']['dt'] = .25
        dense_rows = call(dense)
        assert all(dense_rows[key] == value for key, value in baseline.items())
        for mutate in [
            lambda m: m['components'][0]['fields'].append(m['components'][0]['fields'][0]),
            lambda m: m['components'][0]['agents'][0].__setitem__('completed', True),
            lambda m: m['components'][0]['agents'][0].__setitem__('unknown', 1),
            lambda m: m['components'][0]['chart']['transitions'][1].__setitem__('target', 99),
            lambda m: m['components'][0]['chart']['transitions'][1].__setitem__('duration', 'ready'),
            lambda m: m['components'][0]['chart']['transitions'][0].__setitem__('guard', 'duration'),
            lambda m: m['components'][0]['chart']['transitions'][1]['assign'][0].__setitem__('field', 'state'),
            lambda m: m['components'][0]['messages'].append(m['components'][0]['messages'][0]),
            lambda m: m['components'][0]['messages'][0].__setitem__('agent', 99),
            lambda m: m['outputs'][0].__setitem__('field', 'missing'),
            lambda m: m.__setitem__('links', []),
        ]:
            broken = copy.deepcopy(asynchronous); mutate(broken); call(broken, 'lint', False)
        invalid = copy.deepcopy(sync)
        invalid['components'][0]['phases'][0]['assign'][0]['expr'] = 'work/2'
        call(invalid, valid=False)  # No silent integer rounding at runtime.
        empty = copy.deepcopy(sync); empty['components'][0]['agents'] = []
        empty['outputs'] = [{'id':'active','metric':'active'},{'id':'total','metric':'sum','field':'work'}]
        assert all(v == 0 for v in call(empty).values())
        overflow=copy.deepcopy(sync)
        overflow['components'][0]['agents'][0]['work']=9007199254740991
        overflow['components'][0]['agents'][1]['work']=1
        call(overflow,valid=False)
        # Independent Python Philox + renewal calendar across experiment contexts.
        rates=json.loads((ROOT/'models/typed_abm_rates.ir.json').read_text())
        experiment={'seed':7319,'replications':3,'scenarios':[{'id':0,'parameters':{}},{'id':4,'parameters':{'hazard':0}},{'id':9,'parameters':{'hazard':2.5}}]}
        experiment_path=Path(directory)/'experiment.json'; experiment_path.write_text(json.dumps(experiment))
        path.write_text(json.dumps(rates))
        command=[exe,'run',str(path),'--experiment',str(experiment_path)]
        result=subprocess.run(command,capture_output=True,text=True)
        assert result.returncode==0,result.stderr
        assert subprocess.run(command,capture_output=True,text=True).stdout==result.stdout
        actual={(int(r['scenario']),int(r['replication']),float(r['time']),r['output_id']):float(r['value']) for r in csv.DictReader(io.StringIO(result.stdout))}
        for scenario,hazard in [(0,.7),(4,0),(9,2.5)]:
            for replication in range(3):
                events=[]
                for agent in range(2):
                    times=[]; now=0; generation=0
                    while hazard:
                        now+=-math.log((word(7319,scenario,replication,agent,generation,71)+.5)/4294967296)/hazard
                        if now>5: break
                        times.append(now); generation+=1
                    events.append(times)
                for k in range(11):
                    counts=[sum(t<=k/2 for t in times) for times in events]
                    for name,value in [('first',counts[0]),('second',counts[1]),('total',sum(counts))]:
                        assert actual[scenario,replication,k/2,name]==value
        experiment['scenarios'][0]['parameters']['hazard']=-1
        experiment_path.write_text(json.dumps(experiment))
        result=subprocess.run(command,capture_output=True,text=True)
        assert result.returncode!=0 and json.loads(result.stderr)['diagnostics'][0]['code']=='IR_ABM'
    print('Typed ABM CLI: hand recurrences/confluent histories, permutations, observation density and rejection contracts passed')


if __name__ == '__main__':
    main()
