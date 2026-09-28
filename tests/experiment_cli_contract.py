"""CLI thread-count determinism and failure-safe file publication."""
import csv
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile

EXE = str(Path(sys.argv[1]).resolve())
ROOT = Path(__file__).resolve().parents[1]


def require(ok, why):
    if not ok:
        raise ValueError(why)


def run(*args):
    return subprocess.run([EXE, 'run', *map(str, args), '--no-manifest'], capture_output=True)


def successful(*args):
    result = run(*args)
    require(result.returncode==0, result.stderr.decode())
    return result.stdout


def failure(code, *args):
    result = run(*args)
    require(result.returncode!=0 and not result.stdout, 'failed run published stdout')
    report = json.loads(result.stderr)
    require(report['diagnostics'][0]['code']==code, result.stderr.decode())


def main():
    with tempfile.TemporaryDirectory(prefix='fathom-experiment-') as directory:
        root = Path(directory);model = root/'model.json';experiment = root/'experiment.json';output = root/'results.csv'
        spec = json.loads((ROOT/'models/stochastic_process.ir.json').read_text())
        spec['time'].update(dt=.5, horizon=4)
        spec['components'][0]['exponential']['count'] = 6
        model.write_text(json.dumps(spec))
        definition = dict(seed=987, replications=3, scenarios=[dict(id=i, parameters={}) for i in range(1000)])
        experiment.write_text(json.dumps(definition))
        baseline = successful(model, '--experiment', experiment, '--threads', 1)
        for threads in (2, 7, 16):
            require(successful(model, '--experiment', experiment, '--threads', threads)==baseline, '1000-scenario CSV changed with thread count')
        decoded = list(csv.DictReader(io.StringIO(baseline.decode())))
        addresses = sorted({(int(r['scenario']), int(r['replication'])) for r in decoded})
        require(addresses==[(s, r) for s in range(1000) for r in range(3)], 'missing/aliased scenario address')
        require([(int(r['scenario']), int(r['replication'])) for r in decoded]==sorted((int(r['scenario']), int(r['replication'])) for r in decoded), 'CSV merge order')
        maximum = 2**64-1
        overridden = successful(model, '--experiment', experiment, '--threads', 7, '--seed', maximum)
        definition['seed'] = maximum;experiment.write_text(json.dumps(definition))
        require(successful(model, '--experiment', experiment, '--threads', 2)==overridden and overridden!=baseline, 'seed override/address contract')
        single = successful(model, '--seed', maximum)
        require(successful(model, '--threads', 256, '--seed', maximum)==single, 'single-model seed/thread contract')
        require(single!=successful(model), 'single-model seed override ignored')
        # Each trajectory owns runtime state for all currently exposed execution modes.
        small = root/'small.json';small.write_text(json.dumps(dict(seed=73, replications=3, scenarios=[dict(id=i, parameters={}) for i in range(12)])))
        fixtures = ['decay.ir.json', 'abm_adoption.ir.json', 'agent_stock_sd.ir.json', 'typed_abm_async.ir.json',
                    'typed_abm_rates.ir.json', 'typed_stochastic_process.ir.json', 'typed_resource_process.ir.json']
        verified = []
        for name in fixtures:
            path = ROOT/'models'/name
            require(path.exists(), 'missing execution-mode fixture: '+name)
            a = successful(path, '--experiment', small, '--threads', 1)
            require(successful(path, '--experiment', small, '--threads', 7)==a, 'execution-mode isolation: '+name)
            verified.append(name)
        sentinel = b'previous complete result\n';output.write_bytes(sentinel)
        invalid = [('0', '--threads'), ('257', '--threads'), ('1.5', '--threads'), ('-1', '--threads'), ('+2', '--threads'),
                   ('', '--threads'), ('18446744073709551616', '--seed'), ('-1', '--seed'), ('nan', '--seed')]
        for value, option in invalid:
            failure('IR_USAGE', model, option, value, '--out', output)
            require(output.read_bytes()==sentinel, 'invalid options truncated result')
        failure('IR_USAGE', model, '--threads', 1, '--threads', 2, '--out', output)
        failure('IR_USAGE', model, '--seed', 0, '--seed', 1, '--out', output)
        broken = json.loads((ROOT/'models/decay.ir.json').read_text());broken['parameters'][0]['value'] = 100
        broken_model = root/'broken.json';broken_model.write_text(json.dumps(broken))
        failure('IR_RUNTIME', broken_model, '--out', output)
        require(output.read_bytes()==sentinel, 'simulation failure truncated previous result')
        # A failed ensemble cannot publish its already successful prefix.
        failed_experiment = root/'failed.json';failed_experiment.write_text(json.dumps(dict(seed=0, replications=1,
            scenarios=[dict(id=0, parameters={'decay_rate': .2}), dict(id=1, parameters={'decay_rate': 100})])))
        for threads in (1, 7):
            failure('IR_RUNTIME', ROOT/'models/decay.ir.json', '--experiment', failed_experiment, '--threads', threads, '--out', output)
            require(output.read_bytes()==sentinel, 'failed ensemble published partial file')
        original = model.read_bytes()
        for alias in (root/'symlink.json', root/'hardlink.json'):
            if alias.name.startswith('sym'):
                alias.symlink_to(model)
            else:
                alias.hardlink_to(model)
            failure('IR_USAGE', model, '--out', alias)
            require(model.read_bytes()==original, 'input alias overwritten')
        failure('IR_USAGE', model, '--experiment', experiment, '--out', experiment)
        target_directory = root/'directory';target_directory.mkdir();(target_directory/'keep').write_text('intact')
        failure('IR_IO', model, '--out', target_directory)
        require((target_directory/'keep').read_text()=='intact' and not list(root.glob('.fathom-*.tmp-*')), 'failed rename leaked temp or changed destination')
        failure('IR_IO', model, '--out', root/'missing'/'results.csv')
        require(successful(model, '--seed', maximum, '--out', output)==b'' and output.read_bytes()==single, 'atomic result content mismatch')
        require(not list(root.glob('.fathom-*.tmp-*')), 'successful publication leaked temporary file')
        report = dict(passed=True, scenarios=1000, replications=3, thread_counts=[1, 2, 7, 16], observations=len(decoded),
                      csv_sha256=hashlib.sha256(baseline).hexdigest(), execution_modes=verified,
                      failure_safe_publication=True, seed_override=True)
        if len(sys.argv)>2:
            Path(sys.argv[2]).write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report))

if __name__=='__main__':
    main()
