import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from application.examples.make_demo import create
from application.fathom_service.client import Client
from application.fathom_service.contracts import APIError, canonical, capture, describe, loads
from application.fathom_service.core import Store
from application.fathom_service.http import Server, query_results
from application.fathom_service.metrics import Results, compare
from application.fathom_service.openapi import document

ROOT = Path(__file__).resolve().parents[2]
ENGINE = Path(os.environ.get('FATHOM_SERVICE_ENGINE', ROOT/'build-arrow/fathom')).resolve()


def wait(store, key, timeout=30):
    end = time.monotonic()+timeout
    while time.monotonic() < end:
        job = store.get(key)
        if job['status'] in ('completed', 'failed', 'cancelled', 'interrupted'):
            return job
        time.sleep(.02)
    raise AssertionError('job did not finish')


class MetricContract(unittest.TestCase):
    def setUp(self):
        metric = lambda key, aggregation: dict(id=key, output=key, unit='USD', dimensions={}, aggregation=aggregation, interpolation='hold')
        self.description = dict(time={'unit': 'month'}, metrics=[metric('income', 'sum'), metric('cost', 'sum'), metric('cash', 'delta'),
            dict(id='margin', unit='1', dimensions={}, aggregation='ratio', numerator='cost', denominator='income')])
        # Unequal weights: average(1/2, 1/8)=.3125, correct aggregate=2/10=.2.
        rows = [dict(scenario=0, replication=0, output_id=name, time=t, value=v)
                for name, values in [('income', [999, 2, 8]), ('cost', [999, 1, 1]), ('cash', [0, -5, 20])]
                for t, v in enumerate(values)]
        self.results = Results(self.description, rows)

    def test_ratio_and_period_boundaries(self):
        result = self.results.summary({'metrics': ['income', 'cost', 'margin', 'cash'], 'from': 0, 'to': 2,
                                       'thresholds': [{'metric': 'margin', 'operator': 'lte', 'value': .2}]})
        self.assertEqual([v['value'] for v in result['values']], [10, 2, .2, 20])
        self.assertTrue(result['thresholds'][0]['passed'])
        self.assertEqual(self.results.value('income', 0, 0, 1, 2)['value'], 8)

    def test_zero_denominator_is_explicit(self):
        value = self.results.value('margin', 0, 0, 0, 0)
        self.assertIsNone(value['value'])
        self.assertEqual(value['reason'], 'zero_denominator')

    def test_integrals_and_weighted_means_on_irregular_grid(self):
        metrics = [dict(id=k, output='x', unit='hours', dimensions={}, aggregation=aggregation, interpolation=interpolation)
                   for k, aggregation, interpolation in [('hold', 'integral', 'hold'), ('linear', 'integral', 'linear'), ('mean', 'mean', 'hold')]]
        rows = [dict(scenario=0, replication=0, output_id='x', time=t, value=v) for t, v in [(0, 2), (1, 8), (3, 10)]]
        result = Results(dict(time={'unit': 'month'}, metrics=metrics), rows)
        self.assertEqual(result.value('hold', 0, 0, 0, 3)['value'], 18)
        self.assertEqual(result.value('linear', 0, 0, 0, 3)['value'], 23)
        self.assertEqual(result.value('mean', 0, 0, 0, 3)['value'], 6)
        self.assertIsNone(result.value('mean', 0, 0, 0, 0)['value'])

    def test_no_offgrid_or_duplicate_time_summaries(self):
        with self.assertRaises(APIError):
            self.results.value('cash', 0, 0, .5, 2)
        with self.assertRaises(APIError):
            Results(self.description, [dict(scenario=0, replication=0, output_id='cash', time=0, value=1)]*2)

    def test_negative_and_zero_baseline_conventions(self):
        request = dict(baseline_run='a', candidate_run='b', metrics=['cash'], **{'from': 0, 'to': 1})
        result = compare(self.results, self.results, request)['values'][0]
        self.assertEqual(result['percent_change'], 0)
        request['to'] = 0
        self.assertEqual(compare(self.results, self.results, request)['values'][0]['reason'], 'zero_baseline')

    def test_discounted_valuation_and_no_recovery(self):
        description = dict(time={'unit': 'year'}, metrics=[dict(id='cash', output='cash', unit='USD', dimensions={}, aggregation='sum', interpolation='hold')])
        def results(values):
            return Results(description, [dict(scenario=0, replication=0, output_id='cash', time=t, value=v) for t, v in enumerate(values)])
        request = dict(baseline_run='a', candidate_run='b', metrics=['cash'], **{'from': 0, 'to': 2},
                       valuation=dict(metric='cash', annual_discount_rate=.1, time_units_per_year=1, initial_incremental_cash_flow=-100))
        result = compare(results([0, 0, 0]), results([0, 60, 60]), request)['valuation']
        self.assertAlmostEqual(result['incremental_npv'], -100+60/1.1+60/1.1**2)
        self.assertEqual(result['undiscounted_payback_time'], 2)
        result = compare(results([0, 0, 0]), results([0, 20, 20]), request)['valuation']
        self.assertEqual(result['payback_status'], 'not_recovered')
        request['valuation']['initial_incremental_cash_flow'] = 0
        self.assertEqual(compare(results([0, 0, 0]), results([0, 20, 20]), request)['valuation']['payback_status'], 'no_deficit')


class ContractValidation(unittest.TestCase):
    def test_json_rejects_nonfinite_and_duplicate_keys(self):
        for raw in ('{"x":NaN}', '{"x":1,"x":2}', '{'):
            with self.assertRaises(APIError): loads(raw)

    def test_descriptor_unknown_parameter_and_metric_cycle(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = create(Path(tmp)/'demo')
            model = json.loads((directory/'model.json').read_text())
            original = json.loads((directory/'descriptor.json').read_text())
            bad = copy.deepcopy(original)
            bad['parameters'][0]['id'] = 'not_a_parameter'
            with self.assertRaises(APIError): describe(model, bad, directory)
            bad = copy.deepcopy(original)
            bad['metrics'][-1]['numerator'] = bad['metrics'][-1]['id']
            with self.assertRaises(APIError): describe(model, bad, directory)

    def test_capture_rejects_escaping_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = create(Path(tmp)/'demo')
            model = json.loads((directory/'model.json').read_text())
            model['data'] = [{'source': '../secret.csv'}]
            (directory/'model.json').write_text(json.dumps(model))
            with self.assertRaises(APIError): capture(directory/'model.json', directory/'descriptor.json')

    def test_cross_parameter_constraints_and_units(self):
        from application.fathom_service.contracts import validate_overrides
        with tempfile.TemporaryDirectory() as tmp:
            directory = create(Path(tmp)/'demo')
            model = json.loads((directory/'model.json').read_text())
            spec = json.loads((directory/'descriptor.json').read_text())
            spec['constraints'] = [dict(kind='sum_lte', parameters=['fixed_fee', 'productivity'], value=1,
                                        message='Fixture cross-parameter constraint')]
            description = describe(model, spec, directory)
            with self.assertRaises(APIError): validate_overrides(description, {'fixed_fee': 1, 'productivity': .2})
            spec['metrics'][0]['unit'] = 'kg'
            with self.assertRaises(APIError): describe(model, spec, directory)

    def test_bound_defaults_and_source_snapshot(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = create(Path(tmp)/'demo')
            model = json.loads((directory/'model.json').read_text())
            model['data'] = [dict(id='defaults', source='data.csv', schema=dict(key_column='practice', columns=[
                dict(name='practice', type='string', unit=''), dict(name='demand', type='f64', unit='hours')]),
                use=dict(kind='parameter_table', key='demo', parameters=[dict(parameter='demand_hours', column='demand')]))]
            (directory/'model.json').write_text(json.dumps(model))
            (directory/'data.csv').write_text('practice,demand\ndemo,200\n')
            store = Store(directory/'registry.json', Path(tmp)/'state', ENGINE)
            try:
                description = store.model('economics_demo')['description']
                self.assertEqual(description['parameters'][0]['default'], 200)
                (directory/'data.csv').write_text('practice,demand\ndemo,10\n')
                job = store.submit(dict(model_id='economics_demo', model_version=description['model_version']))
                self.assertEqual(wait(store, job['id'])['status'], 'completed')
                value = store.summary(job['id'], {'metrics': ['revenue'], 'from': 0, 'to': 12})['values'][0]['value']
                self.assertEqual(value, 384000)
                captured = store.model('economics_demo')['directory']/'data.csv'
                captured.write_text('practice,demand\ndemo,20\n')
                with self.assertRaises(APIError): store.submit(dict(model_id='economics_demo', model_version=description['model_version']))
            finally: store.close()


class HTTPContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='fathom-api-test-')
        cls.root = Path(cls.temp.name)
        cls.demo = create(cls.root/'demo')
        cls.store = Store(cls.demo/'registry.json', cls.root/'state', ENGINE)
        try:
            cls.server = Server(('127.0.0.1', 0), cls.store, ['http://localhost:5173'])
        except Exception:
            cls.store.close()
            cls.temp.cleanup()
            raise
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f'http://127.0.0.1:{cls.server.server_port}'
        cls.client = Client(cls.base)
        cls.description = cls.client.describe('economics_demo')
        cls.template = dict(model_id='economics_demo', model_version=cls.description['model_version'])
        cls.baseline = cls.client.run(dict(cls.template, overrides={'fixed_fee': 1}, seed=21))['id']
        cls.candidate = cls.client.run(dict(cls.template, overrides={'fixed_fee': 1, 'productivity': .2}, seed=21))['id']
        for key in (cls.baseline, cls.candidate):
            job = cls.client.wait(key)
            assert job['status'] == 'completed', job

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown(); cls.server.server_close(); cls.thread.join(); cls.store.close(); cls.temp.cleanup()

    def raw(self, path, body=None, headers=None):
        data = None if body is None else json.dumps(body).encode()
        req = Request(self.base+path, data=data, headers={'Content-Type': 'application/json', **(headers or {})})
        try:
            with urlopen(req, timeout=30) as response: return response.status, response.headers, response.read()
        except HTTPError as error:
            with error: return error.code, error.headers, error.read()

    def test_discovery_and_openapi_match_actual_responses(self):
        import jsonschema
        code, _, raw = self.raw('/openapi.json')
        self.assertEqual(code, 200)
        api = json.loads(raw)
        self.assertEqual(api['openapi'], '3.1.0')
        for schema, instance in [('Description', self.description), ('Run', self.client.status(self.baseline)),
                                 ('Results', self.client.results(self.baseline, metric='revenue')),
                                 ('Summary', self.client.summary(self.baseline, {'metrics': ['revenue'], 'from': 0, 'to': 12})),
                                 ('Comparison', self.client.compare(dict(baseline_run=self.baseline, candidate_run=self.candidate,
                                      metrics=['profit'], **{'from': 0, 'to': 12}, valuation=dict(metric='profit', annual_discount_rate=.1,
                                      time_units_per_year=12, initial_incremental_cash_flow=-100))))]:
            jsonschema.validate(instance, dict(api, **{'$ref': '#/components/schemas/'+schema}))
        self.assertEqual(api, json.loads((ROOT/'application/openapi.json').read_text()))
        jsonschema.validate(json.loads((self.demo/'descriptor.json').read_text()), dict(api, **{'$ref': '#/components/schemas/Descriptor'}))
        self.assertEqual(self.client.models()['models'][0]['id'], 'economics_demo')

    def test_native_values_slicing_dimensions_and_pagination(self):
        result = self.client.results(self.baseline, metric='revenue', limit=100)
        self.assertEqual(len(result['rows']), 13)
        for row in result['rows']: self.assertEqual(row['value'], 20000*row['time'])
        result = self.client.results(self.baseline, metric='revenue', max_points=3, limit=2, dimension='practice:demo')
        self.assertEqual([r['time'] for r in result['rows']], [0, 6])
        self.assertEqual(result['source_rows'], 13)
        self.assertEqual(result['next_offset'], 2)
        last = self.client.results(self.baseline, metric='revenue', max_points=3, offset=2)
        self.assertEqual(last['rows'][0]['time'], 12)
        self.assertFalse(result['authoritative_for_aggregation'])

    def test_comparison_npv_payback_and_summary_are_independent_of_downsampling(self):
        summary = self.client.summary(self.candidate, {'metrics': ['profit', 'utilization', 'margin'], 'from': 0, 'to': 12})
        self.assertAlmostEqual(summary['values'][0]['value'], 109920)
        self.assertAlmostEqual(summary['values'][1]['value'], .5)
        self.assertAlmostEqual(summary['values'][2]['value'], .458)
        result = self.client.compare(dict(baseline_run=self.baseline, candidate_run=self.candidate,
            metrics=['revenue', 'profit'], **{'from': 0, 'to': 12},
            valuation=dict(metric='profit', annual_discount_rate=0, time_units_per_year=12, initial_incremental_cash_flow=-100)))
        self.assertEqual(result['values'][0]['delta'], 0)
        self.assertAlmostEqual(result['values'][1]['delta'], 1920)
        self.assertAlmostEqual(result['valuation']['incremental_npv'], 1820)
        self.assertEqual(result['valuation']['undiscounted_payback_time'], 1)
        self.assertEqual(result['baseline_provenance']['run_id'], self.baseline)
        self.assertEqual(len(result['baseline_provenance']['native_manifest_id']), 64)

    def test_explain_and_terminal_event_stream(self):
        result = self.client.explain(self.baseline, {'metric': 'revenue', 'from': 0, 'at': 1})
        self.assertIn('explanation', result)
        code, headers, raw = self.raw(f'/v1/runs/{self.baseline}/events')
        self.assertEqual(code, 200)
        self.assertEqual(headers['Content-Type'], 'text/event-stream')
        self.assertIn(b'event: status', raw)
        self.assertIn(b'"status":"completed"', raw)

    def test_cache_and_seed_identity(self):
        hit = self.client.run(dict(self.template, overrides={'fixed_fee': 1}, seed=21))
        self.assertTrue(hit['cache_hit']); self.assertEqual(hit['id'], self.baseline)
        fresh = self.client.run(dict(self.template, overrides={'fixed_fee': 1}, seed=22))
        self.assertFalse(fresh['cache_hit']); self.assertNotEqual(fresh['id'], self.baseline)
        self.assertEqual(self.client.wait(fresh['id'])['status'], 'completed')

    def test_bad_requests_and_browser_origins(self):
        for body in [dict(self.template, overrides={'productivity': 1}), dict(self.template, overrides={'fixed_fee': True}),
                     dict(self.template, overrides={'hidden': 1}), dict(self.template, model_version='stale'),
                     dict(self.template, threads=0), dict(self.template, overrides={}, scenarios=[])]:
            code, _, raw = self.raw('/v1/runs', body)
            self.assertIn(code, (409, 422)); self.assertIn('error', json.loads(raw))
        code, _, _ = self.raw('/v1/models', headers={'Origin': 'https://unrelated.example'})
        self.assertEqual(code, 403)
        code, headers, _ = self.raw('/v1/models', headers={'Origin': 'http://localhost:5173'})
        self.assertEqual(code, 200); self.assertEqual(headers['Access-Control-Allow-Origin'], 'http://localhost:5173')
        for query in ('metric=missing', 'metric=margin', 'from=NaN', 'limit=0', 'scenario=0&scenario=1', 'unknown=1'):
            self.assertEqual(self.raw(f'/v1/runs/{self.baseline}/results?'+query)[0], 422)

    def test_multi_scenario_and_replication_selection(self):
        job = self.client.run(dict(self.template, replications=2, threads=2, scenarios=[
            {'id': 2, 'overrides': {'productivity': .2}}, {'id': 7, 'overrides': {}}]))
        self.assertEqual(self.client.wait(job['id'])['status'], 'completed')
        actual = self.client.summary(job['id'], {'metrics': ['revenue'], 'scenario': 2, 'replication': 1, 'from': 0, 'to': 12})
        self.assertEqual(actual['values'][0]['value'], 192000)
        self.assertEqual(self.client.status(job['id'])['progress']['completed_trajectories'], 4)

    def test_artifact_corruption_cannot_be_served_or_cached(self):
        path = self.root/'state'/'runs'/self.baseline/'results.csv'
        original = path.read_bytes()
        try:
            path.write_bytes(original+b'\n')
            self.assertEqual(self.raw(f'/v1/runs/{self.baseline}/results')[0], 409)
            self.assertEqual(self.raw('/v1/runs', dict(self.template, overrides={'fixed_fee': 1}, seed=21))[0], 409)
        finally: path.write_bytes(original)


class WorkerLifecycle(unittest.TestCase):
    def test_cancel_running_and_queued_restart_and_exclusive_owner(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); demo = create(root/'demo')
            # Controlled slow external process tests cancellation independent of
            # model speed. Native numerical runs are covered by HTTPContract.
            engine = root/'slow-engine'
            engine.write_text('#!/usr/bin/env python3\nimport sys,time\nif sys.argv[1]=="lint": sys.exit(0)\ntime.sleep(30)\n')
            engine.chmod(0o755)
            store = Store(demo/'registry.json', root/'state', engine, workers=1)
            description = store.model('economics_demo')['description']
            request = dict(model_id='economics_demo', model_version=description['model_version'])
            try:
                with self.assertRaises(APIError): Store(demo/'registry.json', root/'state', engine)
                first = store.submit(request)['id']
                until = time.monotonic()+5
                while first not in store.processes and time.monotonic() < until: time.sleep(.01)
                self.assertIn(first, store.processes)
                second = store.submit(dict(request, seed=1))['id']
                self.assertEqual(store.cancel(second)['status'], 'cancelled')
                started = time.monotonic(); store.cancel(first)
                self.assertEqual(wait(store, first)['status'], 'cancelled')
                self.assertLess(time.monotonic()-started, 3)
                for key in (first, second):
                    with self.assertRaises(APIError): store.results(key)
                self.assertEqual(store.cache, {})
            finally: store.close()
            jobfile = root/'state'/'runs'/first/'job.json'
            job = json.loads(jobfile.read_text()); job['status'] = 'running'; jobfile.write_text(json.dumps(job))
            reopened = Store(demo/'registry.json', root/'state', engine)
            try: self.assertEqual(reopened.get(first)['status'], 'interrupted')
            finally: reopened.close()

    def test_completed_cache_survives_restart(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); demo = create(root/'demo')
            store = Store(demo/'registry.json', root/'state', ENGINE)
            request = dict(model_id='economics_demo', model_version=store.model('economics_demo')['description']['model_version'])
            first = store.submit(request)['id']; self.assertEqual(wait(store, first)['status'], 'completed'); store.close()
            store = Store(demo/'registry.json', root/'state', ENGINE)
            try:
                again = store.submit(request)
                self.assertTrue(again['cache_hit']); self.assertEqual(again['id'], first)
            finally: store.close()

    def test_failed_engine_does_not_publish_or_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); demo = create(root/'demo')
            engine = root/'failed-engine'
            engine.write_text('#!/usr/bin/env python3\nimport sys\nif sys.argv[1]=="lint": sys.exit(0)\nprint("deliberate worker failure")\nsys.exit(2)\n')
            engine.chmod(0o755)
            store = Store(demo/'registry.json', root/'state', engine)
            try:
                request = dict(model_id='economics_demo', model_version=store.model('economics_demo')['description']['model_version'])
                key = store.submit(request)['id']; job = wait(store, key)
                self.assertEqual(job['status'], 'failed')
                self.assertEqual(job['error']['code'], 'ENGINE_ERROR')
                self.assertIn('deliberate worker failure', job['error']['message'])
                with self.assertRaises(APIError): store.results(key)
                self.assertEqual(store.cache, {})
            finally: store.close()


if __name__ == '__main__': unittest.main()
