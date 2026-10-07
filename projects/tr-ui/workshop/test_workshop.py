import copy,json,selectors,subprocess,sys,tempfile,time,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];HERE=ROOT/'projects/tr-ui';sys.path.insert(0,str(ROOT));sys.path.insert(0,str(HERE))
from workshop.spec import DEFAULT,validate
from workshop.service import Workshop
from application.fathom_service.contracts import APIError,canonical,digest
RUNNER=HERE/'native/build/tr-workshop'
class OperatingTests(unittest.TestCase):
 def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.w=Workshop(Path(self.tmp.name),RUNNER)
 def tearDown(self):self.w.close();self.tmp.cleanup()
 def config(self,**params):
  c=copy.deepcopy(DEFAULT);c['parameters'].update(days=14);c['parameters'].update(params);return c
 def run_model(self,c,hours=24):
  v=self.w.create(c);s=self.w.get(v['id'])
  while s.status=='paused':s.step(dict(expected_revision=s.revision,hours=hours))
  self.assertEqual(s.status,'completed');return s
 def test_validation(self):
  for field,value in [('win_rate',1.2),('seed',True),('sd_dt',0),('arrival_min',11),('training_hours',float('nan'))]:
   c=self.config();c['parameters'][field]=value
   with self.assertRaises(APIError):validate(c)
  c=self.config();c['levels'][0]['count']=1.5
  with self.assertRaises(APIError):validate(c)
 def test_idle_step_retry_integrity(self):
  v=self.w.create(self.config());s=self.w.get(v['id']);self.assertEqual(v['frame']['time'],0);self.assertEqual(len(v['frame']['agents']),150)
  with selectors.DefaultSelector() as sel:
   sel.register(s.process.stdout,selectors.EVENT_READ);self.assertEqual(sel.select(.05),[])
  v=s.step(dict(expected_revision=0,hours=8));self.assertEqual(v['frame']['time'],8);self.assertEqual(s.step(dict(expected_revision=0,hours=8)),v)
  self.assertEqual(s.record['record_sha256'],digest(canonical({k:v for k,v in s.record.items() if k!='record_sha256'})))
  with self.assertRaises(APIError):s.step(dict(expected_revision=9,hours=8))
  s.close()
  with self.assertRaises(APIError):s.step(dict(expected_revision=1,hours=8))
 def test_observation_and_finance_step_independence(self):
  a=self.run_model(self.config(days=7),24).frames[-1];b=self.run_model(self.config(days=7,sd_dt=.25),1).frames[-1]
  for k,v in a['metrics'].items():
   if v is not None:self.assertAlmostEqual(v,b['metrics'][k],delta=1e-6,msg=k)
  self.assertEqual(a['queues'],b['queues']);self.assertTrue(a['checks_passed']);self.assertTrue(b['checks_passed'])
 def test_reproducibility_and_no_demand_ledger(self):
  c=self.config(days=7,prospects_day=0,bd_conversion=0,license_share=0)
  a=self.run_model(c).frames[-1];b=self.run_model(c).frames[-1]
  self.assertEqual(a,b);self.assertEqual(a['metrics']['revenue'],0);self.assertEqual(a['metrics']['backlog'],0)
  salary=sum(l['count']*l['salary'] for l in c['levels']);cost=(salary*(1+c['parameters']['cost_load'])+c['parameters']['overhead'])*7/365
  self.assertAlmostEqual(a['metrics']['cost'],cost,delta=1e-6)
 def test_training_license_skill_and_conservation(self):
  c=self.config(days=7,license_share=1,usage=0)
  # Remove all senior reviewers: completed work must remain zero.
  c['levels'][5]['count']=0;c['levels'][6]['count']=0
  a=self.run_model(c).frames[-1]
  self.assertEqual(a['metrics']['completed'],0);self.assertTrue(a['checks_passed']);self.assertGreater(a['metrics']['training_hours'],0)
  self.assertGreater(a['metrics']['proficiency'],c['parameters']['initial_proficiency'])
  self.assertTrue(all(not x['using_ai'] for x in a['agents']))
 def test_evidence_rejects_bad_points_and_reports_failures(self):
  s=self.run_model(self.config(days=1));b=dict(source='Synthetic negative control',kind='synthetic',rows=[dict(time=24,metric='revenue',unit='USD',actual=-100,tolerance=0)])
  receipt=self.w.evidence(s.id,b);self.assertFalse(receipt['passed']);self.assertEqual(receipt['config_sha256'],s.record['config_sha256'])
  for metric,actual,t in [('revenue',float('nan'),24),('unknown',0,24),('revenue',0,25)]:
   b['rows']=[dict(time=t,metric=metric,unit='USD',actual=actual,tolerance=0)]
   with self.assertRaises(APIError):self.w.evidence(s.id,b)
 def test_experiments_paired_seeds_and_cancel(self):
  e=self.w.experiment(dict(config=self.config(days=1),variable='license_share',values=[0,.3],replications=2))
  deadline=time.monotonic()+30
  while time.monotonic()<deadline:
   e=self.w.experiment_state(e['id'])
   if e['status'] not in ('queued','running'):break
   time.sleep(.01)
  self.assertEqual(e['status'],'completed');self.assertEqual(e['finished'],4);self.assertEqual([r['seed'] for r in e['results']],[42,42,43,43])
  e=self.w.experiment(dict(config=self.config(days=90),variable='license_share',values=[0,.3],replications=2));self.w.experiment_state(e['id'],True)
  deadline=time.monotonic()+10
  while time.monotonic()<deadline:
   e=self.w.experiment_state(e['id'])
   if e['status']=='cancelled':break
   time.sleep(.01)
  self.assertEqual(e['status'],'cancelled');self.assertLess(e['finished'],4)
 def test_zero_effect_ai_negative_control(self):
  a=self.run_model(self.config(days=7,license_share=0,training_hours=0,license_cost=0,ai_saving=0,review_extra=0)).frames[-1]
  b=self.run_model(self.config(days=7,license_share=1,training_hours=0,license_cost=0,ai_saving=0,review_extra=0)).frames[-1]
  for k in ('completed','backlog','delivery_hours','revenue','cost','utilization'):
   self.assertAlmostEqual(a['metrics'][k],b['metrics'][k],delta=1e-6,msg=k)
 def test_role_observations_and_units(self):
  s=self.run_model(self.config(days=1));b=dict(source='Assumed roster fixture',kind='synthetic',rows=[dict(time=24,metric='headcount_level0',unit='person',actual=18,tolerance=0)])
  self.assertTrue(self.w.evidence(s.id,b)['passed']);b['rows'][0]['unit']='USD'
  with self.assertRaises(APIError):self.w.evidence(s.id,b)
 def test_native_hand_schedules(self):
  p=Path(self.tmp.name)/'default.json';p.write_text(json.dumps(DEFAULT));subprocess.run([str(HERE/'native/build/workshop-tests'),str(p)],check=True)
if __name__=='__main__':unittest.main(verbosity=2)
