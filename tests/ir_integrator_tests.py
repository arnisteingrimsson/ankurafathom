"""Integrator selection, independent discrete/continuous oracles, and invalid stages."""
import copy
import csv
import io
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
FATHOM=str(Path(sys.argv.pop(1)).resolve())
BASE=json.loads((ROOT/'models/rk4_growth.ir.json').read_text())


class IntegratorTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.folder=Path(self.tmp.name)

    def command(self, model, action='run', overrides=None):
        path=self.folder/'model.json';path.write_text(json.dumps(model))
        cmd=[FATHOM,action,str(path)]
        if overrides is not None:
            experiment=self.folder/'experiment.json'
            experiment.write_text(json.dumps(dict(seed=1,replications=1,scenarios=[dict(id=0,parameters=overrides)])))
            cmd += ['--experiment',str(experiment)]
        return subprocess.run(cmd,text=True,capture_output=True)

    def rows(self, model, overrides=None):
        result=self.command(model,overrides=overrides)
        self.assertEqual(result.returncode,0,result.stderr)
        return {(float(r['time']),r['output_id']):float(r['value']) for r in csv.DictReader(io.StringIO(result.stdout))}

    def rejected(self, model, code, pointer, action='lint'):
        result=self.command(model,action)
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(result.stdout,'')
        self.assertIn((code,pointer),[(d['code'],d['pointer']) for d in json.loads(result.stderr)['diagnostics']])

    def test_stage_times_states_and_observations(self):
        model=copy.deepcopy(BASE);model['time'].update(dt=1,horizon=1)
        rows=self.rows(model)
        # y'=t*y: k1=0, k2=1/2, k3=5/8, k4=13/8 at dt=1.
        self.assertAlmostEqual(rows[1,'level_ts'],79/48,places=14)
        self.assertEqual(rows[1,'rate_ts'],rows[1,'level_ts'])
        model['integrator']='euler'
        self.assertEqual(self.rows(model)[1,'level_ts'],1)
        del model['integrator']
        self.assertEqual(self.rows(model)[1,'level_ts'],1)
        model['integrator']='rk4'
        self.assertEqual(self.rows(model,{'gain':0})[1,'level_ts'],1)
        model['components'].reverse();model['outputs'].reverse()
        self.assertEqual(self.rows(model),rows)

    def test_fourth_order_convergence(self):
        previous=None
        for dt in (.25,.125,.0625,.03125):
            model=copy.deepcopy(BASE);model['time']['dt']=dt
            rows=self.rows(model)
            error=abs(rows[2,'level_ts']-math.exp(2))
            if previous is not None: self.assertTrue(13<previous/error<18,(dt,error,previous/error))
            previous=error
        self.assertLess(previous,1e-6)

    def test_coupled_stage_snapshot(self):
        model=dict(ir_version='0.1',name='oscillator',integrator='rk4',time=dict(unit='1',dt=1,horizon=1),
            components=[dict(id='x',kind='stock',init=1,unit='1',non_negative=False),
                        dict(id='v',kind='stock',init=0,unit='1',non_negative=False),
                        dict(id='dx',kind='flow',source=None,destination='x',expr='v',unit='1',non_negative=False),
                        dict(id='dv',kind='flow',source=None,destination='v',expr='-x',unit='1',non_negative=False)],
            outputs=[dict(id='x_ts',stock='x'),dict(id='v_ts',stock='v')])
        rows=self.rows(model)
        self.assertAlmostEqual(rows[1,'x_ts'],13/24,places=14)
        self.assertAlmostEqual(rows[1,'v_ts'],-5/6,places=14)
        model['components'].reverse()
        self.assertEqual(self.rows(model),rows)

    def test_lookup_stage_queries_and_nominal_dt(self):
        model=copy.deepcopy(BASE)
        model['time'].update(unit='1',dt=1,horizon=1)
        model['parameters']=[]
        for component in model['components']: component['unit']='1'
        model['components'][0]['init']=0
        model['components'][1]['expr']='curve(t)*dt'
        model['components'].append(dict(id='curve',kind='table',x=[0,1],y=[0,2],x_unit='1',unit='1',extrapolate='linear'))
        model['outputs']=[dict(id='level_ts',stock='level')]
        self.assertEqual(self.rows(model)[1,'level_ts'],1)

    def test_method_and_subset_rejections(self):
        for value in ('midpoint','RK4','bad',None,1):
            model=copy.deepcopy(BASE);model['integrator']=value
            self.rejected(model,'IR_INTEGRATOR' if isinstance(value,str) else 'IR_TYPE','/integrator')
        for fixture in ('delay','variable_delay','extended_delay'):
            model=json.loads((ROOT/'models'/f'{fixture}.ir.json').read_text());model['integrator']='rk4'
            index=next(i for i,c in enumerate(model['components']) if c['kind']=='delay')
            self.rejected(model,'IR_INTEGRATOR',f'/components/{index}/kind')
        for fixture in ('process','hybrid_completion','abm_adoption','agent_pool','agent_pool_sd'):
            model=json.loads((ROOT/'models'/f'{fixture}.ir.json').read_text());model['integrator']='rk4'
            self.rejected(model,'IR_FIELD','/integrator')
        for expression in ('gain*t*level*PULSE(0,1)','STEP(gain*t*level,1)','RAMP(gain*level,0,1)'):
            model=copy.deepcopy(BASE);model['components'][1]['expr']=expression
            self.rejected(model,'IR_INTEGRATOR','/components/1/expr')

    def test_midstep_failure_and_retry(self):
        model=copy.deepcopy(BASE);model['time'].update(dt=1,horizon=1)
        model['components'][1]['expr']='gain*t*level*dt/(t-dt/2)'
        self.assertEqual(self.command(model,'lint').returncode,0)
        self.rejected(model,'IR_RUNTIME','',action='run')
        model['components'][1]['expr']='gain*t*level'
        self.assertAlmostEqual(self.rows(model)[1,'level_ts'],79/48,places=14)


if __name__=='__main__': unittest.main()
