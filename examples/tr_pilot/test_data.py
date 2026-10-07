import copy
from datetime import date
from pathlib import Path
import tempfile
import unittest
import math
import pyarrow as pa
import pyarrow.parquet as pq
from data import generate,calibrate,apply_calibration
from config import default_config,validate


class DataTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='tr-data-');self.path=Path(self.tmp.name)/'data'
        self.truth=generate(self.path)
    def tearDown(self):self.tmp.cleanup()
    def rewrite(self,name,fn):
        p=self.path/(name+'.parquet');rows=pq.read_table(p).to_pylist();fn(rows);pq.write_table(pa.Table.from_pylist(rows),p)
    def test_recovery_and_dirty_cases(self):
        f=calibrate(self.path);lo,hi=f['win_rate_95_wilson']
        self.assertLess(lo,.45);self.assertGreater(hi,.45)
        self.assertLess(abs(f['estimates']['prospects']-100.),8.)
        self.assertLess(abs(f['estimates']['realization']-.9),.003)
        for k in ['missing_fee_type','slipped_expected_close','open_asof']:self.assertGreater(f['diagnostics'][k],0)
        c=validate(apply_calibration(default_config(),f));self.assertEqual(sum(l['count'] for l in c['levels']),150)
    def test_no_future_or_truth_leakage(self):
        before=calibrate(self.path);(self.path/'planted-truth.json').write_text('{"win_rate": 0.999}')
        def corrupt(rows):
            for r in rows:
                if r['Timestamp']>=date(2025,1,1):r['StageName']='Closed Lost'
        self.rewrite('opportunity_history',corrupt)
        def stale(rows):
            for r in rows:r['StageName']='Closed Won';r['Probability']=1.
        self.rewrite('opportunities',stale)
        after=calibrate(self.path)
        self.assertEqual(before['estimates'],after['estimates']);self.assertEqual(before['levels'],after['levels'])
    def test_duplicate_rejected(self):
        self.rewrite('opportunities',lambda rows:rows.append(dict(rows[0])))
        with self.assertRaises(ValueError):calibrate(self.path)
    def test_bad_timesheet_rejected(self):
        self.rewrite('timesheets',lambda rows:rows[0].update(DeliveryHours=41.))
        with self.assertRaises(ValueError):calibrate(self.path)
    def test_unknown_employee_rejected(self):
        self.rewrite('timesheets',lambda rows:rows[0].update(EmployeeId='unknown'))
        with self.assertRaises(ValueError):calibrate(self.path)
    def test_unknown_fee_rejected(self):
        self.rewrite('timesheets',lambda rows:rows[0].update(FeeType='unsupported'))
        with self.assertRaises(ValueError):calibrate(self.path)
    def test_negative_pay_rejected(self):
        self.rewrite('roster',lambda rows:rows[0].update(AnnualBasePay=-1.))
        with self.assertRaises(ValueError):calibrate(self.path)
    def test_missing_history_rejected(self):
        self.rewrite('opportunity_history',lambda rows:rows.__setitem__(slice(None),[r for r in rows if r['OpportunityId']!='O0']))
        with self.assertRaises(ValueError):calibrate(self.path)
    def test_seed_reproducibility(self):
        other=Path(self.tmp.name)/'other';generate(other)
        for p in self.path.glob('*.parquet'):self.assertEqual(p.read_bytes(),(other/p.name).read_bytes())
    def test_synthetic_future_cohort(self):
        # Fit ends before any holdout opportunity is created. Six months of births
        # get at least six months to mature; permanently open deals stay in denominator.
        fit=calibrate(self.path)
        ids={r['Id'] for r in pq.read_table(self.path/'opportunities.parquet').to_pylist()
             if date(2025,1,1)<=r['CreatedDate']<date(2025,7,1)}
        wins={r['OpportunityId'] for r in pq.read_table(self.path/'opportunity_history.parquet').to_pylist()
              if r['OpportunityId'] in ids and r['Timestamp']<date(2026,1,1) and r['StageName']=='Closed Won'}
        p=fit['estimates']['win_rate'];observed=len(wins)/len(ids)
        # Predeclared 3-sigma sampling tolerance including training uncertainty.
        tolerance=3*math.sqrt(p*(1-p)*(1/len(ids)+1/fit['mature_cohort']))
        self.assertLess(abs(observed-p),tolerance)


if __name__=='__main__':unittest.main(verbosity=2)
