"""Live execution: compute barriers, idempotence, stop, integrity and full native parity."""
import csv
import copy
import shutil
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import selectors
import tempfile
import unittest
from server import package,ROOT,HERE
from live import LiveSessions,LiveSession
from verify_live import verify
from application.fathom_service.core import Store
from application.fathom_service.contracts import APIError,canonical,digest

class LiveTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.temp=tempfile.TemporaryDirectory();cls.path=Path(cls.temp.name)
  source=ROOT/'artifacts/tr-pilot-150-validated-20260930/batch-00/inputs/model.json'
  package(source,cls.path/'package')
  cls.store=Store(cls.path/'package/registry.json',cls.path/'service',ROOT/'build-arrow/fathom')
  cls.live=LiveSessions(cls.store,cls.path/'live',HERE/'native/build/tr-live')
  cls.version=cls.store.model('ankura_tr')['description']['model_version']
 @classmethod
 def tearDownClass(cls):cls.live.close();cls.store.close();cls.temp.cleanup()
 def create(self,overrides=None):return self.live.create(dict(model_version=self.version,overrides=overrides or {}))
 def test_initialization_does_not_advance_without_command(self):
  initial=self.create();session=self.live.get(initial['id'])
  try:
   self.assertEqual(initial['revision'],0);self.assertEqual(len(session.frames),1)
   with selectors.DefaultSelector() as selector:
    selector.register(session.process.stdout,selectors.EVENT_READ)
    self.assertEqual(selector.select(timeout=.1),[],'Native process produced an unsolicited future observation')
   self.assertEqual(session.revision,0)
   one=session.step(0);self.assertEqual(one['revision'],1);self.assertEqual(len(session.frames),2)
   self.assertEqual(session.step(0),one,'Retry advanced the engine twice')
   with self.assertRaises(APIError):session.step(5)
   session.close()
   with self.assertRaises(APIError):session.step(1)
  finally:session.close()
 def test_configuration_rejected_before_worker_start(self):
  count=len(self.live.sessions)
  with self.assertRaises(APIError):self.create(dict(fixed_shift=.5))
  with self.assertRaises(APIError):self.live.create(dict(model_version='stale',overrides={}))
  self.assertEqual(len(self.live.sessions),count)
 def test_reset_creates_new_initial_state(self):
  a=self.create();first=self.live.get(a['id']);first.step(0);first.close()
  b=self.create();second=self.live.get(b['id'])
  try:
   self.assertNotEqual(a['id'],b['id']);self.assertEqual(b['revision'],0);self.assertEqual(a['frame']['outputs'],b['frame']['outputs'])
  finally:second.close()
 def test_native_failure_stops_at_first_failing_month(self):
  model=copy.deepcopy(self.store.model('ankura_tr'))
  inputs=self.path/'failure-model';shutil.copytree(model['directory'],inputs)
  p=inputs/'model.json';native=json.loads(p.read_text());native['checks'].append(dict(id='planted_failure',kind='assert',expr='cum_revenue == zero_u',absolute_tolerance=0,relative_tolerance=0));p.write_text(json.dumps(native))
  model['directory']=inputs
  folder=self.path/'failure-session';folder.mkdir()
  session=LiveSession(folder,model,dict(overrides={}),HERE/'native/build/tr-live')
  try:
   failed=session.step(0);self.assertEqual(failed['status'],'failed');self.assertEqual(failed['revision'],1)
   self.assertFalse(failed['frame']['checks_passed']);self.assertIsNotNone(session.process.poll())
   with self.assertRaises(APIError):session.step(1)
  finally:session.close()
 def test_record_detects_changed_controls(self):
  initial=self.create();session=self.live.get(initial['id'])
  try:
   verify(session.record);changed=copy.deepcopy(session.record);changed['parameters']['ai']=.9
   with self.assertRaises(AssertionError):verify(changed)
  finally:session.close()
 def test_full_trajectories_match_verified_batch(self):
  reference=ROOT/'artifacts/tr-ui-state/service/runs/b90426f9dfe245fcbefe02f60a10819c'
  job=json.loads((reference/'job.json').read_text());expected={0:{},1:{}}
  with (reference/'results.csv').open() as file:
   for r in csv.DictReader(file):expected[int(r['scenario'])][(int(float(r['time'])),r['output_id'])]=float(r['value'])
  def evaluate(scenario):
   initial=self.create(scenario['parameters']);session=self.live.get(initial['id'])
   try:
    for month in range(60):session.step(month)
    self.assertEqual(session.status,'completed');verify(session.record);actual={}
    previous=None
    for frame in session.frames:
     self.assertTrue(frame['checks_passed']);self.assertEqual(len(frame['checks']),23)
     self.assertEqual(frame['previous_sha256'],previous)
     raw={k:v for k,v in frame.items() if k!='sha256'};self.assertEqual(digest(canonical(raw)),frame['sha256']);previous=frame['sha256']
     for output,value in frame['outputs'].items():actual[(frame['month'],output)]=value
    self.assertEqual(actual,expected[scenario['id']])
    print(f"Scenario {scenario['id']}: {len(actual)} native values exactly match saved batch; 61 barriers and 1403 live assertion checks",flush=True)
    with self.assertRaises(APIError):session.step(60)
   finally:session.close()
  with ThreadPoolExecutor(max_workers=2) as pool:list(pool.map(evaluate,job['effective_parameters']))

if __name__=='__main__':unittest.main(verbosity=2)
