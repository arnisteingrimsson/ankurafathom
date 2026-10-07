"""Recovery, missing-truth, row-order and malformed-export controls for this adapter."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import pyarrow.parquet as pq

from data import calibrate, generate, write_table


class CalibrationContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.source = Path(cls.temp.name)/'exports'
        generate(cls.source,dict(fte=4,pipeline_hours=800.,fixed_share=.4,
            win_rates=[.25,.5,.6],tm_realization=.8,fixed_realization=.95,seasonality=[1.]*12))
        cls.truth=json.loads((cls.source/'truth.json').read_text())
        # Calibration must work without any planted-truth file.
        (cls.source/'truth.json').unlink()
        cls.calibration=calibrate(cls.source)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_recovers_nondefault_parameters(self):
        c=self.calibration
        self.assertEqual(c['count_win_rates'],[.25,.5,.6])
        for key,value in dict(fte=4,pipeline_hours=800.,win_rate=.6,fixed_share=.4,
                              tm_rate=200.,fixed_rate=237.5,paid_hours=160.,delivery_share=.8).items():
            self.assertAlmostEqual(c[key],value,places=8)
        self.assertEqual(c['open_opportunities_excluded'],20)
        self.assertEqual(c['seasonality'],[1.]*12)
        for row,expected in zip(c['monthly'],self.truth['monthly']):
            self.assertAlmostEqual(row['utilization'],expected['utilization'],places=10)

    def mutate(self, name, transform, rejected=True):
        path=self.source/f'{name}.parquet'
        original=path.read_bytes()
        try:
            rows=pq.read_table(path).to_pylist()
            write_table(path,transform(copy.deepcopy(rows)))
            if rejected:
                with self.assertRaises(ValueError):calibrate(self.source)
            else:
                result=calibrate(self.source)
                for key in result.keys()-{'source_hashes'}:
                    self.assertEqual(result[key],self.calibration[key])
        finally:
            path.write_bytes(original)

    def test_row_order_does_not_change_estimates(self):
        self.mutate('timesheets',lambda rows:rows[::-1],False)

    def test_duplicate_opportunity_rejected(self):
        self.mutate('opportunities',lambda rows:rows+[rows[0]])

    def test_orphan_employee_rejected(self):
        def corrupt(rows):
            rows[0]['EmployeeId']='missing';return rows
        self.mutate('timesheets',corrupt)

    def test_wrong_recognized_revenue_rejected(self):
        def corrupt(rows):
            next(r for r in rows if r['Billable'])['RecognizedAmount']+=100;return rows
        self.mutate('timesheets',corrupt)

    def test_nonfinite_hours_rejected(self):
        def corrupt(rows):
            rows[0]['Hours']=float('nan');return rows
        self.mutate('timesheets',corrupt)

    def test_missing_terminal_history_rejected(self):
        def corrupt(rows):
            index=next(i for i,r in enumerate(rows) if r['StageName']=='Closed Won')
            return rows[:index]+rows[index+1:]
        self.mutate('opportunity_history',corrupt)

    def test_unsupported_workforce_change_rejected(self):
        def corrupt(rows):
            rows[0]['TerminationDate']='2024-01-01';return rows
        self.mutate('roster',corrupt)

    def test_overcapacity_generation_rejected(self):
        with self.assertRaises(ValueError):
            generate(Path(self.temp.name)/'invalid',dict(fte=2,pipeline_hours=10000.))


if __name__=='__main__':
    unittest.main()
