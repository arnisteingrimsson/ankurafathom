"""Hand trajectories, units, overrides, and failure contracts for varying durations."""
import copy
import csv
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FATHOM = str(Path(sys.argv.pop(1)).resolve())
BASE = json.loads((ROOT/'models/variable_delay.ir.json').read_text())


class VariableDelayTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name)

    def command(self, model, action='run', overrides=None):
        source = self.folder/'model.json'
        source.write_text(json.dumps(model))
        command = [FATHOM,action,str(source)]
        if overrides is not None:
            experiment = self.folder/'experiment.json'
            experiment.write_text(json.dumps(dict(seed=0,replications=1,scenarios=[dict(id=0,parameters=overrides)])))
            command += ['--experiment',str(experiment)]
        return subprocess.run(command,text=True,capture_output=True)

    def rows(self, model, overrides=None):
        result = self.command(model,overrides=overrides)
        self.assertEqual(result.returncode,0,result.stderr)
        return {(float(r['time']),r['output_id']):float(r['value']) for r in csv.DictReader(io.StringIO(result.stdout))}

    def rejected(self, model, code, pointer, action='lint', overrides=None):
        result = self.command(model,action,overrides)
        self.assertNotEqual(result.returncode,0,result.stdout)
        self.assertEqual(result.stdout,'', 'failed run must not publish a partial CSV')
        diagnostics = json.loads(result.stderr)['diagnostics']
        self.assertTrue(any(d['code']==code and d['pointer']==pointer for d in diagnostics),diagnostics)

    def test_material_rate_jumps_without_rescaling_information(self):
        actual = self.rows(BASE)
        expected = [(0,0,10,10),(10,10,10,10),(20,20,5,10),(25,30,6.25,10),(31.25,40,7.1875,10)]
        self.assertEqual(actual, {(float(t),out['id']):v for t,row in enumerate(expected) for out,v in zip(BASE['outputs'],row)})
        reverse = copy.deepcopy(BASE)
        reverse['components'].reverse()
        self.assertEqual(self.rows(reverse),actual)
        reverse['outputs'].reverse()
        self.assertEqual(self.rows(reverse),actual)

    def test_third_order_stages_share_old_values(self):
        model = copy.deepcopy(BASE)
        model['time']['horizon'] = 5
        for parameter in model['parameters'][1:]: parameter['value'] = 3
        for component in model['components']:
            if component['kind']=='delay': component['order']=3
        rows = self.rows(model)
        self.assertEqual([rows[t,'material_out'] for t in range(6)], [10,10,5,5,5,5.625])
        self.assertEqual([rows[t,'material_total_ts'] for t in range(6)], [0,10,20,25,30,35])
        self.assertEqual([rows[t,'signal_out'] for t in range(6)],[10]*6)

    def test_overrides_reinitialize_pipeline_and_do_not_leak(self):
        original = self.rows(BASE)
        override = self.rows(BASE, {'tau':4,'change':4})
        self.assertEqual(override[0,'material_out'],10)
        self.assertEqual(override[2,'material_out'],5)
        self.assertEqual(override[3,'material_out'],5.625)
        self.assertEqual(override[4,'material_total_ts'],30.625)
        self.rejected(BASE,'IR_DELAY_RUNTIME','/components/2/duration',action='run',overrides={'tau':.5})
        self.assertEqual(self.rows(BASE),original)

    def test_stock_driven_duration_uses_current_committed_stock(self):
        model = copy.deepcopy(BASE)
        model['parameters'].append(dict(id='gain',value=.1,unit='day/kg'))
        for component in model['components']:
            if component['kind']=='delay': component['duration']='tau+gain*material_total'
        rows = self.rows(model)
        # t=1: old pipeline remains 20, committed cumulative delivery=10, tau=3.
        self.assertAlmostEqual(rows[1,'material_out'],20/3)
        self.assertAlmostEqual(rows[2,'material_total_ts'],10+20/3)
        # Both rates and stages used tau=3 at t=1; t=2 gets the updated stock.
        self.assertAlmostEqual(rows[2,'material_out'],(20+10-20/3)/(2+.1*(10+20/3)))
        self.assertEqual(rows[2,'signal_out'],10)

    def test_lint_diagnostics(self):
        for duration, code in [('missing','IR_SYMBOL'),('tau+','IR_EXPR'),('intake','IR_UNIT'),
                               ('2','IR_UNIT'),('material','IR_DELAY_DURATION'),
                               ('tau-tau','IR_DELAY'),('tau/(tau-tau)','IR_UNIT'),
                               ('tau/(dt-dt)*dt','IR_DELAY')]:
            model = copy.deepcopy(BASE)
            model['components'][2]['duration']=duration
            with self.subTest(duration=duration): self.rejected(model,code,'/components/2/duration')
        model = copy.deepcopy(BASE)
        model['components'][2]['duration']='curve(material)'
        model['components'].append(dict(id='curve',kind='table',x=[0,10],y=[2,4],x_unit='kg/day',unit='day',extrapolate='clamp'))
        self.rejected(model,'IR_DELAY_DURATION','/components/2/duration')

    def test_general_order_limits_and_fixed_history(self):
        for order in (2,5,8,255):
            model = copy.deepcopy(BASE)
            for component in model['components']:
                if component['kind']=='delay':
                    component.update(order=order,duration=order,initial=0)
            model['time']['horizon']=order+1
            rows=self.rows(model)
            self.assertEqual([rows[t,'material_out'] for t in range(order+2)], [0]*order+[10,10])
            self.assertEqual(rows[order+1,'material_total_ts'],10)
        for order in (0,256,-1,2.5,True):
            model=copy.deepcopy(BASE)
            model['components'][2]['order']=order
            self.rejected(model,'IR_DELAY','/components/2/order')
        model=copy.deepcopy(BASE)
        model['components'][2].update(type='fixed',duration='tau-t',initial=0)
        rows=self.rows(model)
        self.assertEqual([rows[t,'material_out'] for t in range(5)],[0,0,10,10,10])
        override=self.rows(model,{'tau':3})
        self.assertEqual([override[t,'material_out'] for t in range(5)],[0,0,0,10,10])
        self.rejected(model,'IR_DELAY_RUNTIME','/components/2/duration',action='run',overrides={'tau':2.5})
        model['components'][2]['duration']=1.5
        self.rejected(model,'IR_DELAY','/components/2/duration')
        model['components'][2].update(duration=2,order=2)
        self.rejected(model,'IR_DELAY','/components/2/order')

    def test_fixed_failure_and_composition(self):
        model=copy.deepcopy(BASE)
        model['components'][2].update(type='fixed',duration=1,initial=0)
        model['components'][3].update(type='fixed',duration=2,initial=0,input='material')
        rows=self.rows(model)
        self.assertEqual([rows[t,'signal_out'] for t in range(5)],[0,0,0,10,10])
        model['components'].reverse()
        self.assertEqual(self.rows(model),rows)
        model['components'].reverse()
        model['components'][2]['input']='intake*dt/(tau-t)'
        self.rejected(model,'IR_DELAY_RUNTIME','/components/2/input',action='run')

    def test_extended_fixture(self):
        model=json.loads((ROOT/'models/extended_delay.ir.json').read_text())
        rows=self.rows(model)
        for t in range(8):
            self.assertEqual(rows[t,'material_out'],0 if t<2 else 10)
            self.assertEqual(rows[t,'signal_out'],0 if t<5 else 10)
            self.assertEqual(rows[t,'material_total_ts'],10*max(0,t-2))
            self.assertEqual(rows[t,'signal_total_ts'],10*max(0,t-5))

    def test_runtime_duration_failures_include_horizon(self):
        for expr, horizon in [('tau-STEP(change,2)',4),('tau-STEP(change,4)',4),
                              ('tau/(tau-t)*dt',4),('tau-STEP(change,2)',2)]:
            model = copy.deepcopy(BASE)
            model['components'][2]['duration']=expr
            model['time']['horizon']=horizon
            with self.subTest(expr=expr,horizon=horizon):
                lint = self.command(model,'lint')
                self.assertEqual(lint.returncode,0,lint.stderr)
                self.rejected(model,'IR_DELAY_RUNTIME','/components/2/duration',action='run')
        too_short = copy.deepcopy(BASE)
        too_short['parameters'][2]['value'] = -1.5
        self.rejected(too_short,'IR_DELAY_RUNTIME','/components/2/duration',action='run')
        model = copy.deepcopy(BASE)
        model['components'][2]['duration']='tau+STEP(change,2)'
        model['parameters'][2]['value']=1e308
        self.rejected(model,'IR_DELAY_RUNTIME','/components/2/initial',action='run',overrides={'tau':1e308})


if __name__ == '__main__':
    unittest.main()
