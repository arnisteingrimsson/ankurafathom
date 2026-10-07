"""Numerical and negative-control tests for project evidence; no engine assertions mocked as proof."""
import copy
import json
import sys
import tempfile
import shutil
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from application.fathom_service.metrics import Results
from application.fathom_service.contracts import APIError
from validation import evaluate_history,reconcile


def reader():
    description=dict(time=dict(unit='month'),metrics=[dict(id=k,unit=u,output=k,aggregation='delta' if k=='revenue' else 'last',dimensions={},interpolation='hold') for k,u in [('revenue','USD'),('headcount','person'),('utilization','1')]])
    return Results(description,[dict(scenario=0,replication=0,time=t,output_id=k,value=v) for t in range(5) for k,v in [('revenue',100*t),('headcount',10),('utilization',.5)]])


def request():
    return dict(run_id='fixture',scenario=0,dataset=dict(label='Test observations',source='Hand-calculated fixture',kind='synthetic',start_month='2020-01',calibration_through=2,heldout_attestation=False,
        thresholds={'revenue':dict(unit='USD',mae_max=5)},rows=[dict(month=i,revenue=v) for i,v in enumerate([100,100,80,120],1)]))


class HistoricalTests(unittest.TestCase):
    def test_hand_calculated_errors_and_partition(self):
        r=evaluate_history(reader(),request());a,b=r['summaries']
        self.assertEqual(a['mae'],0);self.assertEqual(b['mae'],20);self.assertEqual(b['rmse'],20)
        self.assertEqual(b['bias'],0);self.assertEqual(b['wape'],.2);self.assertFalse(r['holdout_within_threshold'])
        self.assertEqual([x['partition'] for x in r['observations']],['calibration']*2+['holdout']*2)
    def test_zero_actual_denominator_is_undefined(self):
        q=request()
        for row in q['dataset']['rows']:row['revenue']=0
        r=evaluate_history(reader(),q);self.assertIsNone(r['summaries'][1]['wape']);self.assertEqual(r['summaries'][1]['mae'],100)
    def test_threshold_boundary_inclusive_and_no_fitting(self):
        q=request();q['dataset']['thresholds']['revenue']['mae_max']=20
        r=evaluate_history(reader(),q);self.assertTrue(r['holdout_within_threshold']);self.assertEqual(r['observations'][2]['values']['revenue']['simulated'],100)
    def test_synthetic_never_called_historical_validation(self):
        r=evaluate_history(reader(),request());self.assertIn('not historical accuracy',r['interpretation'])
    def test_observed_independence_explicit(self):
        q=request();q['dataset']['kind']='observed';q['dataset']['heldout_attestation']=True
        r=evaluate_history(reader(),q);self.assertIn('not independently verified',r['interpretation'])
    def test_rejects_invalid_alignment_units_and_values(self):
        mutations=[lambda q:q['dataset']['rows'][1].update(month=1),lambda q:q['dataset']['rows'][1].update(month=3),
            lambda q:q['dataset']['rows'][1].update(revenue=None),lambda q:q['dataset']['rows'][1].update(revenue=float('nan')),
            lambda q:q['dataset']['rows'][1].update(revenue=True),lambda q:q['dataset']['rows'][1].update(unknown=7),
            lambda q:q['dataset']['thresholds']['revenue'].update(unit='EUR'),lambda q:q['dataset'].update(calibration_through=4),
            lambda q:q['dataset'].update(start_month='2020-13'),lambda q:q['dataset'].update(heldout_attestation='yes'),
            lambda q:q['dataset']['thresholds']['revenue'].update(mae_max=-1),lambda q:q.update(scenario=99)]
        for mutate in mutations:
            q=request();mutate(q)
            with self.subTest(q=q),self.assertRaises(APIError):evaluate_history(reader(),q)
    def test_utilization_fraction_units_checked(self):
        q=request();q['dataset']['thresholds']={'utilization':dict(unit='1',mae_max=.1)}
        q['dataset']['rows']=[dict(month=i,utilization=.5) for i in range(1,5)]
        self.assertTrue(evaluate_history(reader(),q)['holdout_within_threshold'])
        q['dataset']['rows'][1]['utilization']=50
        with self.assertRaises(APIError):evaluate_history(reader(),q)
    def test_wrong_financial_formula_is_detected(self):
        values={'cum_ebitda':30,'cum_revenue':100,'cum_cost':70,'cum_earned':110,'cum_disallowed':10,'cum_cash_flow':20,'cum_collections':90}
        r=Results(dict(metrics=[]),[dict(scenario=0,replication=0,time=1,output_id=k,value=v) for k,v in values.items()])
        self.assertTrue(all(c['verdict']=='pass' for c in reconcile(r)))
        r.series[(0,0,'cum_ebitda')]=[(1,170)]
        checks=reconcile(r);self.assertEqual(checks[0]['verdict'],'fail');self.assertEqual(checks[0]['worst']['absolute_gap'],140)

class SavedRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from server import package,ROOT
        from application.fathom_service.core import Store
        from validation import Validation
        cls.temp=tempfile.TemporaryDirectory();cls.path=Path(cls.temp.name)
        source=ROOT/'artifacts/tr-pilot-150-validated-20260930'
        cls.key='b90426f9dfe245fcbefe02f60a10819c'
        package(source/'batch-00/inputs/model.json',cls.path/'package')
        shutil.copytree(ROOT/'artifacts/tr-ui-state/service/runs'/cls.key,cls.path/'state/runs'/cls.key)
        cls.store=Store(cls.path/'package/registry.json',cls.path/'state',ROOT/'build-arrow/fathom')
        cls.service=Validation(cls.store,source,cls.path/'history')
    @classmethod
    def tearDownClass(cls):cls.store.close();cls.temp.cleanup()
    def test_catalog_has_bound_defaults_and_run_identities(self):
        c=self.service.catalog();self.assertEqual(len(c['model']['components']),779)
        self.assertTrue(c['pilot']['inputs_match_current']);self.assertTrue(c['pilot']['receipt_matches_preview'])
        p={p['id']:p for p in c['model']['parameters']};self.assertEqual(p['fixed_share']['effective_value'],.15)
        self.assertEqual(c['agent_provenance']['status'],'not_recorded')
    def test_saved_native_evidence_and_playback_aggregation(self):
        evidence=self.service.evidence(self.key);self.assertEqual(len(evidence['checks']),23)
        self.assertTrue(all(c['verdict']=='pass' for c in evidence['reconciliations']))
        monthly=self.service.playback(self.key,1);annual=self.service.playback(self.key,12)
        self.assertEqual(len(monthly['frames']),60);self.assertEqual(len(annual['frames']),5)
        for scenario in (0,1):
            revenue=sum(f['scenarios'][scenario]['values']['revenue'] for f in monthly['frames'][:12])
            self.assertAlmostEqual(revenue,annual['frames'][0]['scenarios'][scenario]['values']['revenue'],delta=1e-6)
            self.assertEqual(monthly['frames'][11]['scenarios'][scenario]['values']['headcount'],annual['frames'][0]['scenarios'][scenario]['values']['headcount'])
        with self.assertRaises(APIError):self.service.playback(self.key,2)
    def test_persisted_history_rejects_changed_receipt(self):
        q=request();q['run_id']=self.key
        receipt=self.service.history(q);self.assertEqual(self.service.history_list(self.key)['evaluations'][0],receipt)
        path=self.path/'history'/(receipt['receipt_id']+'.json');original=path.read_bytes()
        try:
            receipt['holdout_within_threshold']=True;path.write_text(json.dumps(receipt))
            with self.assertRaises(APIError):self.service.history_list(self.key)
        finally:path.write_bytes(original)
    def test_modified_result_is_rejected(self):
        path=self.store.state/'runs'/self.key/'results.csv';original=path.read_bytes()
        try:
            path.write_bytes(original+b'changed')
            with self.assertRaises(APIError):self.service.evidence(self.key)
        finally:path.write_bytes(original)

if __name__=='__main__':unittest.main(verbosity=2)
