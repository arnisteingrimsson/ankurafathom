"""Lookup and stateful XMILE conformance: independent references and hand oracles."""
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
from xmile2ir import convert, normalized, Unsupported
FATHOM = str(Path(sys.argv.pop(1)).resolve())
ORACLE = ROOT/'tests/oracles/xmile'
NS = 'http://docs.oasis-open.org/xmile/ns/XMILE/v1.0'


def document(variables, horizon=4, dt=1):
    return f'<xmile xmlns="{NS}" version="1.0"><sim_specs><start>0</start><stop>{horizon}</stop><dt>{dt}</dt></sim_specs><model><variables>{variables}</variables></model></xmile>'


def stock_flow(expression, extras=''):
    return f'<stock name="total"><eqn>0</eqn><inflow>f</inflow></stock><flow name="f"><eqn>{expression}</eqn></flow>'+extras


class FunctionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name)

    def parse(self, text):
        path = self.folder/'source.xmile'
        path.write_text(text)
        return convert(path, units='metadata')

    def run_ir(self, model, metadata):
        path = self.folder/'model.json'
        path.write_text(json.dumps(model))
        result = subprocess.run([FATHOM,'run',str(path)], text=True,capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr)
        names = {v['id']+'_ts': normalized(v['name']) for v in metadata['variables'] if v['kind']=='stock'}
        rows = list(csv.DictReader(io.StringIO(result.stdout)))
        return {(float(r['time']), names[r['output_id']]): float(r['value']) for r in rows}

    def total(self, text):
        model, metadata = self.parse(text)
        return [v for (t,name),v in self.run_ir(model,metadata).items() if name=='total']

    def test_pinned_pysd_trajectories_and_variable_order(self):
        reference = json.loads((ORACLE/'pysd-reference.json').read_text())
        comparisons = 0
        for case in reference['cases']:
            path = ORACLE/case['source']
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),case['source_sha256'])
            for reverse in (False,True):
                text = path.read_text()
                if reverse:
                    root = ET.fromstring(text)
                    variables = root.find('{'+NS+'}model/{'+NS+'}variables')
                    variables[:] = list(reversed(variables))
                    text = ET.tostring(root,encoding='unicode')
                model, metadata = self.parse(text)
                if path.name=='stateful.xmile':
                    self.assertEqual(len(metadata['stateful_calls']),14)
                actual = self.run_ir(model,metadata)
                self.assertEqual(len(actual),len(case['times'])*len(case['stocks']))
                for t, values in zip(case['times'],case['values']):
                    for name, expected in zip(case['stocks'],values):
                        if path.name=='extended_delays.xmile' and name=='stock_fixed_nested':
                            # PySD updates fixed state sequentially: this nested
                            # case observes an updated inner delay one tick early.
                            # Our shared snapshot must preserve the full .75 lag.
                            count = max(0, round(t/.125)-6)
                            exact = .125*(4*count+.125*count*(count-1)/2)
                            self.assertEqual(actual[t,name],exact)
                            if t==.75:
                                self.assertEqual(expected,.5)
                                self.assertEqual(actual[t,name],0)
                            continue
                        self.assertTrue(math.isclose(actual[t,name], expected,rel_tol=2e-12,abs_tol=2e-12), (case['source'],reverse,t,name,actual[t,name],expected))
                        comparisons += 1
        self.assertEqual(comparisons,3834)
        print(f'{comparisons} PySD stock comparisons, including reversed source ordering')

    def test_historical_lookup_signed_trajectory(self):
        path = ORACLE/'corpus/tests/lookups/test_lookups_no-indirect.xmile'
        model, metadata = convert(path,units='metadata')
        actual = self.run_ir(model,metadata)
        self.assertEqual(len(actual),181)
        for reference, delimiter, tolerance in (('output_stella1006.csv',',',2e-12),('output.tab','\t',1e-5)):
            with path.with_name(reference).open(newline='') as stream:
                rows = list(csv.DictReader(stream,delimiter=delimiter))
            self.assertEqual(len(rows),181)
            for row in rows:
                t = float(row['Time'])
                self.assertTrue(math.isclose(actual[t,'accumulation'],float(row['accumulation']),rel_tol=tolerance,abs_tol=2e-12))
        self.assertAlmostEqual(actual[45,'accumulation'],0,places=12)
        self.assertGreater(actual[25,'accumulation'],actual[40,'accumulation'])

    def test_lookup_policies_shapes_and_initialization(self):
        for policy, expected in [('continuous',[0,1,2,5,10]),('extrapolate',[0,-1,0,3,8])]:
            for points in ('<xpts>1,2,3</xpts><ypts>1,3,5</ypts>',
                           '<xscale min="1" max="3"/><ypts>1,3,5</ypts>',
                           '<xpts sep=";">1;2;3</xpts><ypts sep=";">1;3;5</ypts>'):
                # Hand oracle covers embedded extrapolation: PySD 3.14.3's
                # inline builder uses np.interp and ignores that policy.
                gf = f'<gf type="{policy}">{points}</gf>'
                source = document(stock_flow('TIME')).replace('<eqn>TIME</eqn>','<eqn>TIME</eqn>'+gf)
                self.assertEqual(self.total(source),expected)
                named = f'<gf name="curve" type="{policy}">{points}</gf>'
                self.assertEqual(self.total(document(stock_flow('curve(TIME)',named))),expected)
        source = document(stock_flow('0','<gf name="curve"><xpts>1,2,3</xpts><ypts>1,3,5</ypts></gf>'))
        source = source.replace('<stock name="total"><eqn>0','<stock name="total"><eqn>curve(1.5)')
        self.assertEqual(self.total(source),[2]*5)
        source = document(stock_flow('mapped','<aux name="mapped"><eqn>Time</eqn><gf name="curve"><xpts>1,2,3</xpts><ypts>1,3,5</ypts></gf></aux>'))
        self.assertEqual(self.total(source),[0,1,2,5,10])

    def test_stateful_hand_oracles_and_feedback(self):
        for function in ('SMTH1','DELAY1'):
            self.assertEqual(self.total(document(stock_flow(f'{function}(10,2,0)'))),[0,0,5,12.5,21.25])
            self.assertEqual(self.total(document(stock_flow(f'{function}(10,2)'))),[0,10,20,30,40])
        for function in ('SMTH3','DELAY3'):
            self.assertEqual(self.total(document(stock_flow(f'{function}(10,3,0)'))),[0,0,0,0,10])
            self.assertEqual(self.total(document(stock_flow(f'{function}(10,3)'))),[0,10,20,30,40])
        self.assertEqual(self.total(document(stock_flow('s+s','<aux name="s"><eqn>smth1(10,2,0)</eqn></aux>'))),[0,0,10,25,42.5])
        self.assertEqual(self.total(document(stock_flow('s','<aux name="s"><eqn>SMTH1(s+1,1,0)</eqn></aux>'))),[0,0,1,3,6])
        self.assertEqual(self.total(document(stock_flow('SMTH1(DELAY1(10,1,0),1,0)'))),[0,0,0,10,20])
        self.assertEqual(self.total(document(stock_flow('SMTH1(-10,2)'))),[0,-10,-20,-30,-40])
        # Default initialization follows declared stock initial values, not zero.
        extra = '<stock name="source"><eqn>3</eqn></stock>'
        self.assertEqual(self.total(document(stock_flow('SMTH1(source*2,2)',extra))),[0,6,12,18,24])

    def test_extended_delay_hand_oracles(self):
        for function in ('SMTHN','DELAYN'):
            for order in (1,2,3,5,8):
                with self.subTest(function=function,order=order):
                    source = document(stock_flow(f'{function}(10,{order},{order},0)'), horizon=order+2)
                    self.assertEqual(self.total(source),[0]*(order+1)+[10,20])
                    source = document(stock_flow(f'{function}(10,{order},{order})'))
                    self.assertEqual(self.total(source),[0,10,20,30,40])
        for ticks in (1,2,4):
            source = document(stock_flow(f'DELAY(TIME,{ticks},-1)'),horizon=ticks+3)
            expected=[0]
            for k in range(ticks+3): expected.append(expected[-1]+(-1 if k<ticks else k-ticks))
            self.assertEqual(self.total(source),expected)
        self.assertEqual(self.total(document(stock_flow('DELAY(10,2-TIME,0)'))),[0,0,0,10,20])
        # Delays in feedback and in a cascade all read the committed tick.
        self.assertEqual(self.total(document(stock_flow('s','<aux name="s"><eqn>DELAY(s+1,1,0)</eqn></aux>'))),[0,0,1,3,6])
        self.assertEqual(self.total(document(stock_flow('DELAY(DELAY(10,1,0),2,0)'))),[0,0,0,0,10])
        extra='<aux name="n"><eqn>1+1</eqn></aux><aux name="tau"><eqn>n</eqn></aux>'
        self.assertEqual(self.total(document(stock_flow('DELAYN(10,tau,n,0)',extra))),[0,0,0,10,20])

    def test_extended_delay_rejections(self):
        for expression, diagnostic in (
            ('SMTHN(1,2)', 'need input'), ('DELAYN(1,2,2,0,1)', 'need input'),
            ('SMTHN(1,2,0)', 'integer in'), ('SMTHN(1,2,2.5)', 'integer in'),
            ('DELAYN(1,1000,256)', 'integer in'), ('SMTHN(1,2,1+TIME)', 'order must be constant'),
            ('SMTHN(1,2,total+1)', 'order must be constant'),
            ('DELAYN(1,2+TIME,2)', 'DELAYN duration must be constant'),
            ('DELAYN(1,2+total,2)', 'DELAYN duration must be constant'),
            ('DELAY(1,1.5)', 'whole ticks'), ('DELAY(1,1000001)', 'whole ticks'),
            ('DELAY(1,SMTH1(2,1))', 'duration cannot depend')):
            with self.subTest(expression=expression),self.assertRaisesRegex(Unsupported,diagnostic):
                self.parse(document(stock_flow(expression)))

    def test_stateful_rejections(self):
        for expression, diagnostic in (
            ('SMTH1(10)', 'need input'), ('SMTH3(10,2,0,1)', 'need input'),
            ('SMTH1(10,SMTH1(2,1))', 'duration cannot depend on delay outputs'),
            ('SMTH3(10,2)', 'duration must cover'), ('DELAY1(10,0)', 'duration must cover'),
            ('DELAY1(-1,2)', 'nonnegative'), ('DELAY3(1e308,1e308)', 'finite pipeline'),
            ('SMTH1(10,2,1/0)', 'division by zero'), ('SMTH1(10,2,1e308*2)', 'finite'),
            ('SMOOTH(10,2)', 'unsupported function'),
            ('SIN(1)', 'unsupported function'),
            ('SMTH1+1', 'function name must be called')):
            with self.subTest(expression=expression),self.assertRaisesRegex(Unsupported,diagnostic):
                self.parse(document(stock_flow(expression)))
        for extras, expression in (
            ('<aux name="s"><eqn>SMTH1(s+1,1)</eqn></aux>','s'),
            ('<aux name="s"><eqn>SMTH1(q,1)</eqn></aux><aux name="q"><eqn>SMTH1(s,1)</eqn></aux>','s')):
            with self.assertRaisesRegex(Unsupported,'cyclic delay initialization'):
                self.parse(document(stock_flow(expression,extras)))
        self.assertEqual(self.total(document(stock_flow('DELAY1(1,source)','<stock name="source"><eqn>2</eqn></stock>'))), [0,1,2,3,4])
        self.assertEqual(self.total(document(stock_flow('SMTH1(10,TIME+2)'))), [0,10,20,30,40])
        with self.assertRaisesRegex(Unsupported,'duration cannot depend on delay outputs'):
            self.parse(document(stock_flow('SMTH1(1,alias)', '<aux name="alias"><eqn>SMTH1(2,1)</eqn></aux>')))
        with self.assertRaisesRegex(Unsupported,'separate auxiliary'):
            self.parse(document(stock_flow('1')).replace('<stock name="total"><eqn>0', '<stock name="total"><eqn>SMTH1(1,2)'))
        # A syntactically invalid but unused function must not be dropped.
        with self.assertRaisesRegex(Unsupported,'need input'):
            self.parse(document(stock_flow('1','<aux name="unused"><eqn>SMTH1(1)</eqn></aux>')))

    def test_signed_native_ir_and_hybrid_modes(self):
        path = ROOT/'models/signed_flow.ir.json'
        result = subprocess.run([FATHOM,'run',str(path)],text=True,capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr)
        rows = list(csv.DictReader(io.StringIO(result.stdout)))
        self.assertEqual([float(r['value']) for r in rows],[10,10,11,9,11,9,10,10])
        for name in ('hybrid_completion', 'abm_adoption', 'agent_pool_sd'):
            model = json.loads((ROOT/'models'/f'{name}.ir.json').read_text())
            time_unit = model['time']['unit']
            model['sd'].setdefault('parameters',[]).append(dict(id='signed_rate',value=-1,unit='kg/'+time_unit))
            model['sd']['components'].extend([
                dict(id='signed_stock',kind='stock',init=10,unit='kg'),
                dict(id='signed_flow',kind='flow',source=None,destination='signed_stock',
                     expr='signed_rate',unit='kg/'+time_unit,non_negative=False)])
            model['outputs'].append(dict(id='signed_ts',stock='signed_stock'))
            path = self.folder/'hybrid.json'
            path.write_text(json.dumps(model))
            result = subprocess.run([FATHOM,'run',str(path)],text=True,capture_output=True)
            self.assertEqual(result.returncode,0,(name,result.stderr))
            rows = list(csv.DictReader(io.StringIO(result.stdout)))
            values = [r for r in rows if r['output_id']=='signed_ts']
            self.assertGreater(len(values),1)
            for row in values:
                self.assertAlmostEqual(float(row['value']),10-float(row['time']),places=12)
            model['sd']['components'][-1]['non_negative'] = True
            path.write_text(json.dumps(model))
            self.assertNotEqual(subprocess.run([FATHOM,'run',str(path)],capture_output=True).returncode,0)

    def test_lookup_rejections(self):
        good = '<gf name="g"><xpts>1,2,3</xpts><ypts>1,2,3</ypts></gf>'
        for bad in (good.replace('1,2,3</xpts>','1,1,3</xpts>'),
                    good.replace('1,2,3</ypts>','1,2</ypts>'),
                    good.replace('1,2,3</ypts>','1,NaN,3</ypts>'),
                    good.replace('name="g"','name="g" type="discrete"'),
                    good.replace('name="g"','name="g" bogus="yes"'),
                    good.replace('<xpts>1,2,3</xpts>',''),
                    good.replace('<xpts>','<xpts sep=";;">'),
                    good.replace('<xpts>1,2,3</xpts>','<xscale min="2" max="1"/>'),
                    good.replace('<ypts>1,2,3</ypts>','<ypts>1,2,3</ypts><ypts>1,2,3</ypts>')):
            with self.subTest(bad=bad),self.assertRaises(Unsupported):
                self.parse(document(stock_flow('g(Time)',bad)))
        for expr in ('g', 'g()', 'g(1,2)', 'g(1)(2)'):
            with self.subTest(expr=expr),self.assertRaises(Unsupported):
                self.parse(document(stock_flow(expr,good)))
        with self.assertRaisesRegex(Unsupported,'colliding'):
            self.parse(document(stock_flow('g(1)',good+good)))
        with self.assertRaisesRegex(Unsupported,'reserved'):
            self.parse(document(stock_flow('1','<aux name="Time"><eqn>0</eqn></aux>')))


if __name__ == '__main__':
    unittest.main()
