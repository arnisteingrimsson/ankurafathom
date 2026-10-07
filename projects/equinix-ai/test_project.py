import copy,json,os,selectors,subprocess,sys,tempfile,time,unittest
from pathlib import Path
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1];sys.path.insert(0,str(ROOT));sys.path.insert(0,str(HERE))
from service import Workshop
from spec import DEFAULT,validate
from application.fathom_service.contracts import APIError,canonical,digest
SUFFIX='-sanitize' if os.environ.get('EQUINIX_SANITIZE')=='1' else ''
class ProjectTests(unittest.TestCase):
 def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.w=Workshop(Path(self.tmp.name),HERE/('native/build/equinix-model'+SUFFIX))
 def tearDown(self):self.w.close();self.tmp.cleanup()
 def config(self,**p):c=copy.deepcopy(DEFAULT);c['parameters'].update(days=60);c['parameters'].update(p);return c
 def run_model(self,c,step=7):
  v=self.w.create(c);s=self.w.get(v['id'])
  while s.status=='paused':s.step(dict(expected_revision=s.revision,days=step))
  self.assertEqual(s.status,'completed');return s
 def test_strict_definition(self):
  for k,v in [('arrivals',-1),('days',True),('sd_dt',0),('dense_share',.9),('seed',float('nan'))]:
   c=self.config();c['parameters'][k]=v
   with self.assertRaises(APIError):validate(c)
  c=self.config();c['sites'][0]['liquid_racks']=100
  with self.assertRaises(APIError):validate(c)
  c=self.config()
  for p in c['providers']:p['enabled']=False
  with self.assertRaises(APIError):validate(c)
 def test_native_hand_schedules(self):
  p=Path(self.tmp.name)/'default.json';p.write_text(json.dumps(DEFAULT));subprocess.run([str(HERE/('native/build/model-tests'+SUFFIX)),str(p)],check=True)
 def test_live_barrier_retry_and_integrity(self):
  v=self.w.create(self.config());s=self.w.get(v['id']);self.assertEqual(v['frame']['time'],0);self.assertEqual(v['frame']['customers'],[])
  with selectors.DefaultSelector() as sel:sel.register(s.process.stdout,selectors.EVENT_READ);self.assertEqual(sel.select(.05),[])
  v=s.step(dict(expected_revision=0,days=1));self.assertEqual(v,s.step(dict(expected_revision=0,days=1)));self.assertEqual(v['frame']['time'],1)
  self.assertEqual(s.record['record_sha256'],digest(canonical({k:v for k,v in s.record.items() if k!='record_sha256'})))
  s.close()
  with self.assertRaises(APIError):s.step(dict(expected_revision=1,days=1))
 def test_observation_and_sd_step_independence(self):
  a=self.run_model(self.config(),7).frames[-1];b=self.run_model(self.config(sd_dt=.1),.5).frames[-1]
  for k,v in a['metrics'].items():
   if v is not None:self.assertAlmostEqual(v,b['metrics'][k],delta=1e-6,msg=k)
  self.assertEqual(a['counts'],b['counts'])
 def test_reproducibility(self):
  a=self.run_model(self.config()).frames;b=self.run_model(self.config()).frames;self.assertEqual(a,b)
 def test_no_demand_cost_ledger(self):
  c=self.config(arrivals=0,days=10);f=self.run_model(c).frames[-1];self.assertEqual(f['metrics']['active'],0);self.assertEqual(f['metrics']['revenue'],0)
  p=c['parameters'];self.assertAlmostEqual(f['metrics']['cost'],10*(p['overhead_day']+(p['qualifiers']+p['designers']+p['installers'])*p['team_cost']),delta=1e-6)
 def test_zero_capacity_prevents_activation(self):
  c=self.config(win_rate=1,patience=10,qualification_days=.1,design_days=.1)
  for site in c['sites']:site['power_kw']=0
  f=self.run_model(c).frames[-1];self.assertEqual(f['metrics']['active'],0);self.assertGreater(f['metrics']['lost'],0);self.assertTrue(f['checks_passed'])
 def test_observations_reject_units_and_show_failure(self):
  s=self.run_model(self.config(days=7));b=dict(source='Synthetic negative control',kind='synthetic',rows=[dict(time=7,metric='active',unit='customer',actual=1000,tolerance=0)]);self.assertFalse(self.w.evidence(s.id,b)['passed'])
  for unit,t in [('USD',7),('customer',99)]:
   b['rows'][0].update(unit=unit,time=t)
   with self.assertRaises(APIError):self.w.evidence(s.id,b)
 def test_strategies_paired_seeds_and_cancel(self):
  e=self.w.experiment(dict(config=self.config(days=5),variable='strategy',values=[0,2],replications=2));deadline=time.monotonic()+30
  while time.monotonic()<deadline:
   e=self.w.experiment_state(e['id'])
   if e['status'] not in ('queued','running'):break
   time.sleep(.02)
  self.assertEqual(e['status'],'completed');self.assertEqual([r['seed'] for r in e['results']],[42,42,43,43]);self.assertEqual(e['finished'],4)
  e=self.w.experiment(dict(config=self.config(days=120),variable='strategy',values=[0,2],replications=3));self.w.experiment_state(e['id'],True)
  deadline=time.monotonic()+10
  while time.monotonic()<deadline:
   e=self.w.experiment_state(e['id'])
   if e['status']=='cancelled':break
   time.sleep(.02)
  self.assertEqual(e['status'],'cancelled')
if __name__=='__main__':unittest.main(verbosity=2)
