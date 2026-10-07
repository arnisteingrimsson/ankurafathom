"""Project registration and saved-example contracts; no changes to the platform."""
import copy
import json
from pathlib import Path
import shutil
import tempfile
import unittest
import pyarrow as pa
import pyarrow.parquet as pq
from server import package,ROOT,HERE
from application.fathom_service.contracts import APIError,describe,validate_overrides

SOURCE=ROOT/'artifacts/tr-pilot-150-validated-20260930'

class ProjectContract(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.path=Path(self.temp.name)
        shutil.copytree(SOURCE/'batch-00/inputs',self.path/'inputs')
        shutil.copyfile(SOURCE/'config.json',self.path/'config.json')
    def tearDown(self):self.temp.cleanup()
    def register(self):
        metadata=package(self.path/'inputs/model.json',self.path/'registered')
        description=describe(json.loads((self.path/'inputs/model.json').read_text()),
            json.loads((self.path/'registered/descriptor.json').read_text()),self.path/'inputs')
        return metadata,description
    def test_fixed_fee_bound_rejected_before_execution(self):
        metadata,d=self.register();self.assertAlmostEqual(metadata['fee_shift_maximum'],.1)
        validate_overrides(d,dict(fixed_shift=.1))
        with self.assertRaises(APIError):validate_overrides(d,dict(fixed_shift=.11))
        self.assertEqual(metadata['initial_fte'],150)
    def test_bound_table_has_precedence_over_literals(self):
        p=self.path/'inputs/parameters.parquet';t=pq.read_table(p)
        t=t.set_column(t.schema.get_field_index('fixed_eligible_share'),'fixed_eligible_share',pa.array([.18]))
        pq.write_table(t,p)
        metadata,d=self.register();self.assertAlmostEqual(metadata['fee_shift_maximum'],.03)
        with self.assertRaises(APIError):validate_overrides(d,dict(fixed_shift=.04))
    def test_conflicting_workforce_policies_rejected(self):
        _,d=self.register()
        with self.assertRaises(APIError):validate_overrides(d,dict(policy_hold=1,policy_responsive=1))
    def test_changed_timestep_rejected_for_monthly_transition_model(self):
        p=self.path/'inputs/model.json';model=json.loads(p.read_text());model['time']['dt']=.5
        p.write_text(json.dumps(model))
        with self.assertRaises(APIError):self.register()
    def test_revenue_breakdown_reconciles(self):
        preview=json.loads((HERE/'preview.json').read_text())
        for example in preview['examples'].values():
            for year in example['annual']:
                values={v['metric']:v for v in year['values']}
                for side in ('baseline','candidate'):
                    earned=sum(values[k][side] for k in ('earned_tm','earned_fixed','earned_retainer','earned_success'))
                    self.assertAlmostEqual(earned-values['disallowed'][side],values['revenue'][side],delta=1e-6)
    def test_saved_examples_match_verified_annual_results(self):
        preview=json.loads((HERE/'preview.json').read_text())
        rows=json.loads((SOURCE/'annual-summary.json').read_text())
        look={(r['scenario'],r['year']):r for r in rows}
        names={'ai':('baseline_responsive','ai20%_responsive_erosion50%'),
          'growth':('baseline_responsive','ai20_pipeline20'),'bd':('baseline_responsive','ai20%_responsive_bd'),
          'freeze':('baseline_freeze','ai20%_freeze_erosion50%'),'none':('baseline_responsive','baseline_responsive')}
        metrics=dict(revenue='revenue',ebitda='ebitda',margin='ebitda_margin',headcount='ending_expected_fte',
                     utilization='billable_role_utilization',cash_flow='operating_cash',collections='collections')
        for key,(baseline,candidate) in names.items():
            for year,comparison in enumerate(preview['examples'][key]['annual'],1):
                values={v['metric']:v for v in comparison['values']}
                for side,name in [('baseline',baseline),('candidate',candidate)]:
                    expected=look[(name,year)]
                    for metric,column in metrics.items():self.assertAlmostEqual(values[metric][side],expected[column],delta=1e-6)
                    for level in expected['levels']:
                        self.assertAlmostEqual(values['headcount_'+level['level']][side],level['ending_expected_fte'],delta=1e-9)

if __name__=='__main__':unittest.main(verbosity=2)
