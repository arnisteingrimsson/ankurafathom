"""Native mechanism fixtures with hand answers plus independent Decimal reconciliation."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from config import default_config, scenario, validate
from run import execute, calendar, EXE, native, save
from oracle import compare


def level(name='worker',count=1,rate=100.,hours=1.,**kwargs):
    return dict(id=name,count=count,rate=rate,annual_pay=0.,hours_per_job=hours,
                admin_share=0.,exposure=0.,attrition=0.,promotion=0.,bd_eligible=0.,**{}) | kwargs


def fixture(**kwargs):
    c=default_config()
    c.update(months=4,levels=[level()],prospects=1.,win_rate=1.,fixed_share=0.,retainer_share=0.,
        success_share=0.,fixed_eligible_share=1.,realization=1.,fixed_overhead=0.,variable_overhead=0.,
        variable_delivery_cost=0.,employer_load=0.,bonus_share=0.,ai_license=0.,ai_training_cost=0.,
        ai_training_hours=0.,hiring_cost=0.,hourly_disallowance=0.,hourly_holdback=0.,
        approval_lag=0,collection_lag=0,holdback_lag=0,ai_rework=0.,fixed_deposit_share=0.)
    c.update(kwargs);return c


class NativeFixtures(unittest.TestCase):
    def run_case(self,c,rows=None,cal=None):
        rows=rows or [scenario()];cal=cal or calendar(c['months'])
        cal['adoption']=[1.]*(c['months']+1);cal['season']=[1.]*(c['months']+1)
        with tempfile.TemporaryDirectory(prefix='tr-native-') as tmp:
            obs,proof=execute(Path(tmp),c,rows,cal)
        return obs

    def test_fully_loaded_cost(self):
        c=fixture(months=1,prospects=300.,levels=[level(count=3,annual_pay=40000.),
            level('support',rate=0.,hours=0.,annual_pay=12000.,admin_share=1.)],
            employer_load=.2,bonus_share=.1,fixed_overhead=3000.)
        r=self.run_case(c)[(0,1)]
        # 10k billable base + 1k support base, both loaded at 30%, plus 3k overhead.
        self.assertAlmostEqual(r['m_revenue'],30000.)
        self.assertAlmostEqual(r['m_cost'],17300.)
        self.assertAlmostEqual(r['m_ebitda'],12700.)
        self.assertAlmostEqual(r['m_support_comp'],1300.)

    def test_missing_senior_blocks_delivery(self):
        r=self.run_case(fixture(levels=[level(),level('senior',count=0)]))[(0,1)]
        self.assertEqual(r['m_delivery'],0.)
        self.assertEqual(r['backlog_tm'],1.)

    def test_junior_ai_composition(self):
        c=fixture(levels=[level(rate=200.,hours=100.,exposure=1.),level('senior',rate=1000.,hours=20.)])
        r=self.run_case(c,[scenario(),scenario('AI',ai=.5)])
        self.assertAlmostEqual(r[(0,1)]['m_revenue'],40000.)
        self.assertAlmostEqual(r[(1,1)]['m_revenue'],30000.)
        self.assertAlmostEqual(r[(1,1)]['m_hourly_hours'],70.)
        self.assertAlmostEqual(r[(1,1)]['m_revenue']/r[(1,1)]['m_hourly_hours'],30000/70)

    def test_annual_attrition_and_hold(self):
        c=fixture(months=12,levels=[level(count=150,attrition=.2)])
        r=self.run_case(c,[scenario(),scenario('freeze',policy='freeze')])
        self.assertAlmostEqual(r[(0,12)]['l0_fte'],150.)
        self.assertAlmostEqual(r[(1,12)]['l0_fte'],120.)
        self.assertAlmostEqual(r[(1,12)]['cum_exit_l0'],30.)

    def test_hiring_lag_and_onboarding(self):
        c=fixture(prospects=500.,recruitment_cap=2.,hire_lag=2,onboard_productivity=.5)
        r=self.run_case(c,[scenario(policy='responsive')])
        self.assertEqual(r[(0,1)]['m_request_l0'],2.)
        self.assertEqual(r[(0,1)]['m_starts_l0'],0.)
        self.assertEqual(r[(0,2)]['m_starts_l0'],0.)
        self.assertEqual(r[(0,3)]['m_starts_l0'],2.)
        self.assertEqual(r[(0,3)]['m_onboard_l0'],160.)

    def test_promotion_conserves_workforce(self):
        c=fixture(levels=[level(count=10,promotion=.1),level('senior',count=2)])
        r=self.run_case(c)[(0,1)]
        self.assertEqual(r['l0_fte'],9.)
        self.assertEqual(r['l1_fte'],3.)
        self.assertEqual(r['m_fte'],12.)

    def test_holdback_delays_cash_only(self):
        c=fixture(hourly_holdback=.2,holdback_lag=2)
        r=self.run_case(c)
        self.assertEqual(r[(0,1)]['m_revenue'],100.)
        self.assertEqual(r[(0,1)]['m_collections'],80.)
        self.assertEqual(r[(0,1)]['receivables'],20.)
        self.assertEqual(r[(0,2)]['receivables'],40.)
        self.assertEqual(r[(0,3)]['m_collections'],100.)

    def test_disallowance_is_not_holdback(self):
        c=fixture(hourly_disallowance=.1,approval_lag=1)
        r=self.run_case(c)
        self.assertEqual(r[(0,1)]['m_revenue'],100.)
        self.assertEqual(r[(0,1)]['m_collections'],0.)
        self.assertEqual(r[(0,2)]['m_disallowed'],10.)
        self.assertEqual(r[(0,2)]['m_revenue'],90.)
        self.assertEqual(r[(0,2)]['m_collections'],90.)

    def test_deposit_not_revenue(self):
        c=fixture(fixed_share=1.,fixed_deposit_share=.2,levels=[level(count=0)])
        r=self.run_case(c)[(0,1)]
        self.assertEqual(r['m_revenue'],0.)
        self.assertEqual(r['m_collections'],20.)
        self.assertEqual(r['deposit_liability'],20.)

    def test_new_price_erosion_old_contract_preserved(self):
        c=fixture(fixed_share=1.,initial_backlog=1.,levels=[level(exposure=1.)])
        r=self.run_case(c,[scenario(ai=.2,erosion=.5)])[(0,1)]
        self.assertAlmostEqual(r['m_earned_fixed'],190.) # old 100 + new 90
        self.assertAlmostEqual(r['m_delivery_hours'],1.6)

    def test_retainer_and_success_conditions(self):
        c=fixture(retainer_share=.5,success_share=.5,retainer_fee=30000.,success_fee=20000.,success_probability=.5,
                  levels=[level(exposure=1.)])
        r=self.run_case(c,[scenario(),scenario(ai=.5),scenario('failed',success_gate=0.)])
        self.assertEqual(r[(0,1)]['m_revenue'],20000.)
        self.assertEqual(r[(1,1)]['m_revenue'],20000.)
        self.assertEqual(r[(2,1)]['m_revenue'],15000.)

    def test_bd_feedback_lag_and_time_budget(self):
        c=fixture(levels=[level(bd_eligible=1.)],bd_fraction=.25,bd_jobs_per_hour=.01,bd_lag=2)
        r=self.run_case(c,[scenario(bd=1.)])
        self.assertAlmostEqual(r[(0,1)]['m_bd_hours'],39.75)
        self.assertEqual(r[(0,2)]['m_prospects'],1.)
        self.assertAlmostEqual(r[(0,3)]['m_prospects'],1.3975)
        self.assertAlmostEqual(r[(0,1)]['m_paid_l0'],sum(r[(0,1)]['m_'+k+'_l0'] for k in ['delivery','bd','admin','training','onboard','idle']))

    def test_macro_lag_and_disabled_equivalence(self):
        c=fixture(macro_lag=1,macro_elasticity=.5)
        cal=calendar(c['months']);cal['market']=[2.]*(c['months']+1)
        r=self.run_case(c,[scenario(),scenario(macro=1.)],cal)
        self.assertEqual(r[(0,2)]['m_prospects'],1.)
        self.assertEqual(r[(1,1)]['m_prospects'],1.)
        self.assertEqual(r[(1,2)]['m_prospects'],1.5)

    def test_full_automation_zero_task(self):
        c=fixture(levels=[level(exposure=1.)])
        r=self.run_case(c,[scenario(ai=1.)])[(0,1)]
        self.assertEqual(r['m_delivery'],1.)
        self.assertEqual(r['m_revenue'],0.)
        self.assertEqual(r['m_delivery_hours'],0.)

    def test_wrong_hourly_formula_negative_control(self):
        c=fixture(levels=[level(exposure=1.)]);rows=[scenario(ai=.5)];cal=calendar(c['months'])
        with tempfile.TemporaryDirectory(prefix='tr-negative-') as tmp:
            path=Path(tmp);execute(path,c,rows,cal)
            model=path/'inputs/model.json';doc=json.loads(model.read_text())
            next(x for x in doc['components'] if x['id']=='earned_tm')['expr']='done_tm * baseline_value'
            save(model,doc)
            bad,_=native(EXE,model,path/'inputs/experiment.json',path/'wrong.parquet')
            with self.assertRaises(AssertionError):compare(bad,c,rows,cal)

    def test_closed_config_and_contract_eligibility(self):
        with self.assertRaises(ValueError):validate(fixture(extra=1))
        with self.assertRaises(ValueError):validate(fixture(fixed_share=.5,fixed_eligible_share=.25))
        with self.assertRaises(ValueError):validate(fixture(realization=0.))
        with self.assertRaises(ValueError):scenario(ai=1.1)
        with self.assertRaises(RuntimeError):self.run_case(fixture(fixed_eligible_share=.1),[scenario(fixed_shift=.5)])

    def test_resume_rejects_changed_inputs(self):
        c=fixture(months=1);rows=[scenario()];cal=calendar(1)
        with tempfile.TemporaryDirectory(prefix='tr-resume-test-') as tmp:
            path=Path(tmp);first,_=execute(path,c,rows,cal)
            second,_=execute(path,c,rows,cal,resume=True)
            self.assertEqual(first,second)
            with self.assertRaises(ValueError):execute(path,dict(c,prospects=2.),rows,cal,resume=True)


if __name__=='__main__':unittest.main(verbosity=2)
