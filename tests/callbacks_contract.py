"""Public Python progress/cancellation: ordering, parity, GIL and exception ownership."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import struct
import sys
import threading
import traceback

ROOT = Path(__file__).resolve().parents[1]


def raises(kind, function):
    try:
        function()
    except kind as error:
        return error
    raise AssertionError(f'expected {kind.__name__}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('package', type=Path)
    parser.add_argument('--without-arrow', action='store_true')
    args = parser.parse_args()
    sys.path.insert(0, str(args.package.resolve()))
    import ankurafathom as af
    model = af.Model.from_json((ROOT/'models/decay.ir.json').read_bytes())
    experiment = af.Experiment({'seed': 42, 'replications': 2, 'scenarios': [
        {'id': 17, 'parameters': {}}, {'id': 65535, 'parameters': {}}]})
    checks = parity = 0

    def bits(table):
        return [(r['scenario'], r['replication'], struct.pack('!d', r['time']),
                 r['output_id'], struct.pack('!d', r['value'])) for r in table.to_pylist()]

    def success(loaded, exp=None, **kwargs):
        if args.without_arrow:
            error = raises(af.FathomError, lambda: af.run(loaded, exp, **kwargs))
            assert error.status == 7 and error.code == 'FATHOM_UNAVAILABLE'
            return None
        return af.run(loaded, exp, **kwargs)

    # Each boundary, including zero and the final boundary, can cancel. Retry the
    # same model immediately to exercise worker joins and disposal of partial data.
    for threads in (1, 8, 32):
        for exp, total in ((None, 1), (experiment, 4)):
            for cancel_at in range(total + 1):
                calls = []
                caller = threading.get_ident()

                def cancel(completed, count):
                    assert threading.get_ident() == caller
                    assert count == total and completed == len(calls)
                    calls.append((completed, count))
                    return completed != cancel_at

                error = raises(af.FathomError, lambda: af.run(model, exp, threads=threads,
                    provenance=True, progress=cancel))
                assert error.status == 8 and error.code == 'FATHOM_CANCELLED'
                assert error.pointer == '' and error.truncated == 0
                assert calls == [(i, total) for i in range(cancel_at + 1)]
                success(model, exp, threads=threads)
                checks += 1

    # Both argument and return types are strict; no accidental truthiness calls.
    for value in (0, False, 'callback', object()):
        raises(TypeError, lambda: af.run(model, progress=value)); checks += 1
    for value in (None, 0, 1, 'yes', [], object()):
        error = raises(TypeError, lambda: af.run(model, experiment, threads=8,
            progress=lambda *_: value))
        assert 'return a bool' in str(error)
        checks += 1

    # Preserve the original exception object and traceback, including BaseException,
    # after C cancellation has joined workers. Exercise start, middle and end.
    for kind in (ValueError, KeyboardInterrupt, SystemExit):
        for boundary in (0, 2, 4):
            original = kind('callback marker')
            calls = []

            def fail_callback(completed, total):
                calls.append(completed)
                if completed == boundary:
                    raise original
                return True

            error = raises(kind, lambda: af.run(model, experiment, threads=8, progress=fail_callback))
            assert error is original and calls == list(range(boundary + 1))
            assert any(frame.name == 'fail_callback' for frame in traceback.extract_tb(error.__traceback__))
            success(model)
            checks += 1

    # Validation rejects before progress, while a runtime failure leaves only the
    # canonical successful prefix. A failure cannot fabricate a final callback.
    calls = []
    def record(completed, total):
        calls.append((completed, total))
        return True
    raises(af.FathomError, lambda: af.run(model, af.Experiment(b'{'), progress=record))
    assert calls == []
    overflow = af.Experiment({'seed': 0, 'replications': 2, 'scenarios': [
        {'id': 0, 'parameters': {}}, {'id': 1, 'parameters': {'decay_rate': 1e308}}]})
    for threads in (1, 8, 32):
        calls.clear()
        error = raises(af.FathomError, lambda: af.run(model, overflow, threads=threads, progress=record))
        assert error.status not in (7, 8) and calls == [(0, 4), (1, 4), (2, 4)]
        checks += 1
    checks += 1

    # Nested native calls deliberately leave a failed TLS record; the outer run
    # must still succeed. Concurrent runs exercise independent contexts and GIL
    # release while waiting for native worker joins.
    def work(index):
        caller = threading.get_ident()
        calls = []
        def progress(completed, total):
            assert threading.get_ident() == caller
            assert completed == len(calls) and total == 4
            calls.append(completed)
            success(model)
            error = raises(af.FathomError, lambda: af.Model.from_json(b'{'))
            assert error.code == 'IR_JSON'
            return not (index % 2 and completed == 2)
        if index % 2:
            error = raises(af.FathomError, lambda: af.run(model, experiment, threads=8, progress=progress))
            assert error.status == 8 and calls == [0, 1, 2]
        else:
            success(model, experiment, threads=8, progress=progress)
            assert calls == [0, 1, 2, 3, 4]
        return True
    with ThreadPoolExecutor(max_workers=4) as pool:
        assert all(pool.map(work, range(12)))
    checks += 12

    # Exact binary64 parity across execution modes, seed extremes, thread counts,
    # and provenance; callbacks must not alter simulation or the numeric receipt.
    fixtures = ['decay.ir.json', 'stochastic_process.ir.json', 'typed_abm_rates.ir.json',
                'agent_stock_sd.ir.json', 'agent_pool.ir.json', 'hybrid_completion.ir.json']
    if not args.without_arrow:
        fixtures += ['data/parameter_decay.ir.json', 'data/seasonal_stock.ir.json', 'data/workforce.ir.json']
    for name in fixtures:
        path = ROOT/'models'/name
        loaded = af.Model.from_json(path.read_bytes(), base_directory=path.parent)
        for exp, total, threads, seed in ((None, 1, 1, 0), (experiment, 4, 1, 0),
                                        (experiment, 4, 8, (1 << 64) - 1), (experiment, 4, 32, 42)):
            calls.clear()
            actual = success(loaded, exp, threads=threads, seed=seed, provenance=True, progress=record)
            assert calls == [(i, total) for i in range(total + 1)]
            if not args.without_arrow:
                expected = af.run(loaded, exp, threads=threads, seed=seed, provenance=True)
                assert bits(actual) == bits(expected)
                a, b = [json.loads(t.schema.metadata[b'ankurafathom.manifest']) for t in (actual, expected)]
                assert a['result'] == b['result'] and a['inputs'] == b['inputs']
            parity += 1
    # Scenario expansion uses the same callback total for all declarative designs.
    for kind in ('grid', 'lhs', 'sobol'):
        exp = af.Experiment((ROOT/f'models/decay.{kind}.experiment.json').read_bytes())
        calls.clear()
        success(model, exp, threads=8, progress=record)
        total = calls[0][1]
        assert calls == [(i, total) for i in range(total + 1)]
        checks += 1
    print(f'Execution callbacks: {parity} mode/parity cases; {checks} cancellation, exception, validation and concurrency controls pass')


if __name__ == '__main__':
    main()
