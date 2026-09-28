"""Pure observation expressions: units, snapshots, overrides, and failure contracts."""
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
BASE = json.loads((ROOT/'models/expression_outputs.ir.json').read_text())


class OutputTests(unittest.TestCase):
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

    def rejected(self, model, code, pointer, action='lint'):
        result = self.command(model,action)
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(result.stdout,'')
        self.assertIn((code,pointer),[(d['code'],d['pointer']) for d in json.loads(result.stderr)['diagnostics']])

    def test_no_stock_trajectories_and_overrides(self):
        for rate in (2,5):
            rows = self.rows(BASE,{'rate':rate})
            for t in (0,.5,1,1.5,2):
                self.assertEqual(rows[t,'quantity'],rate*t)
                self.assertEqual(rows[t,'arithmetic'],5)
                self.assertEqual(rows[t,'clock'],t)
        model = copy.deepcopy(BASE)
        model['outputs'].reverse()
        self.assertEqual(self.rows(model),self.rows(BASE))

    def test_stock_delay_table_and_tick_snapshot(self):
        model=json.loads((ROOT/'models/variable_delay.ir.json').read_text())
        before=self.rows(model)
        model['components'].append(dict(id='identity',kind='table',x=[0,100],y=[0,100],x_unit='kg',unit='kg',extrapolate='linear'))
        model['outputs'].extend([dict(id='combined',expr='material+signal',unit='kg/day'),
                                dict(id='observed',expr='identity(material_total)+intake*dt',unit='kg')])
        rows=self.rows(model)
        for key,value in before.items(): self.assertEqual(rows[key],value)
        for t in range(5):
            self.assertEqual(rows[t,'combined'],rows[t,'material_out']+rows[t,'signal_out'])
            self.assertEqual(rows[t,'observed'],rows[t,'material_total_ts']+10)
        model['components'].reverse()
        model['outputs'].reverse()
        self.assertEqual(self.rows(model),rows)

    def test_lint_rejections(self):
        for output,code,pointer in (
            (dict(id='x',expr='rate*t'), 'IR_MISSING','/outputs/0/unit'),
            (dict(id='x',expr='rate*t',unit='day'), 'IR_UNIT','/outputs/0/unit'),
            (dict(id='x',expr='missing',unit='1'), 'IR_SYMBOL','/outputs/0/expr'),
            (dict(id='x',expr='x',unit='1'), 'IR_SYMBOL','/outputs/0/expr'),
            (dict(id='x',expr='1+',unit='1'), 'IR_EXPR','/outputs/0/expr'),
            (dict(id='x',expr='1',unit='1',stock='s'), 'IR_OUTPUT','/outputs/0'),
            (dict(id='x',stock='s',unit='1'), 'IR_FIELD','/outputs/0/unit')):
            model=copy.deepcopy(BASE);model['outputs']=[output]
            with self.subTest(output=output): self.rejected(model,code,pointer)

    def test_runtime_failure_at_horizon_publishes_no_partial_rows(self):
        model=copy.deepcopy(BASE)
        model['outputs'].append(dict(id='fails_at_horizon',expr='rate*t/(2*dt+2*dt-t)',unit='kg/day'))
        self.assertEqual(self.command(model,'lint').returncode,0)
        self.rejected(model,'IR_OUTPUT_RUNTIME','/outputs/3/expr',action='run')
        model['outputs'].pop()
        self.assertEqual(self.rows(model),self.rows(BASE))


if __name__=='__main__':
    unittest.main()
