"""Next-tick source policy: rational oracle, initialization, units and failures."""
import copy
import hashlib
import json
import math
import subprocess
import sys
import unittest
import xml.etree.ElementTree as ET
import xmile_dialect_tests as common

ROOT=common.ROOT;ORACLE=common.ORACLE;NS=common.NS


class InputTests(unittest.TestCase):
    setUp=common.DialectTests.setUp
    command=common.DialectTests.command
    rows=common.DialectTests.rows

    def parse(self,text,policy='next_tick',delayn='constant'):
        path=self.folder/'source.xmile';path.write_text(text)
        return common.convert(path,units='metadata',outputs='all',input_policy=policy,delayn_policy=delayn)

    def test_rational_event_and_stock_oracle(self):
        report=json.loads((ORACLE/'input-reference.json').read_text());count=0
        self.assertEqual(report['generator_sha256'],hashlib.sha256((ORACLE/'generate_inputs.py').read_bytes()).hexdigest())
        self.assertEqual(len(report['cases']),288)
        for case in report['cases']:
            self.assertEqual(case['source_sha256'],hashlib.sha256(case['source_xml'].encode()).hexdigest())
            for reverse in (False,True):
                tree=ET.fromstring(case['source_xml']);variables=tree.find(NS+'model/'+NS+'variables')
                if reverse:variables[:]=list(reversed(variables))
                model,metadata=self.parse(ET.tostring(tree,encoding='unicode'))
                self.assertEqual(metadata['input_policy'],report['policy'])
                rows=self.rows(model,metadata)
                times=sorted({time for time,name in rows})
                self.assertEqual(len(times),17)
                for time,expected_time,rate,total in zip(times,case['times'],case['rates'],case['totals']):
                    self.assertAlmostEqual(time,expected_time,places=12)
                    for name,expected in [('rate',rate),('total',total)]:
                        self.assertTrue(math.isclose(rows[time,name],expected,rel_tol=2e-12,abs_tol=2e-12),(case['source_xml'],time,name,rows[time,name],expected))
                        count+=1
        self.assertEqual(count,19584)
        print('19584 rational next-tick observations, including reversed source ordering')

    def test_initialization_and_delays(self):
        source=common.document('PULSE(4,-.125,.25)',dt=.5,horizon=1)
        source=source.replace('<eqn>0</eqn>','<eqn>PULSE(4,-.125,.25)</eqn>',1)
        model,metadata=self.parse(source);rows=self.rows(model,metadata)
        self.assertEqual(rows[0,'total'],8);self.assertEqual(rows[0,'rate'],8)
        self.assertEqual(rows[.5,'total'],12);self.assertEqual(rows[.5,'rate'],16)
        for policy in ('cascade','history2'):
            model,metadata=self.parse(common.document('DELAYN(PULSE(4,-.125,.25),2,2)',dt=.5,horizon=1),delayn=policy)
            self.assertEqual(self.rows(model,metadata)[0,'rate'],8)
        for expression,expected in [('STEP(2,10.25)',0),('RAMP(2,9.75)',.5),('PULSE(4,9.75)',8)]:
            source=common.document('0',start=10,dt=.5,horizon=1).replace('<eqn>0</eqn>',f'<eqn>{expression}</eqn>',1)
            model,metadata=self.parse(source);self.assertEqual(self.rows(model,metadata)[10,'total'],expected)
        # Multiple arrivals in a tick use the shared tick quantity, not future state.
        model,metadata=self.parse(common.document('PULSE(total+1,.125,.25)',dt=.5,horizon=1.5))
        rows=self.rows(model,metadata)
        self.assertEqual([rows[t,'total'] for t in (0,.5,1,1.5)],[0,0,2,8])

    def test_units_overrides_and_guards(self):
        model=json.loads((ROOT/'models/offgrid_inputs.ir.json').read_text())
        for quantity in (3,-2):
            rows=self.rows(model,overrides={'quantity':quantity})
            self.assertEqual([rows[t,'total_ts'] for t in (10,10.5,11,11.5,12)], [0,0,2*quantity,4*quantity,6*quantity])
            self.assertEqual(rows[12,'rate'],4*quantity) # visible, never integrated past horizon
        for expression in ('XMILE_NEXT_PULSE(quantity,quantity,period,origin)',
                           'XMILE_NEXT_PULSE(quantity,first,period)',
                           'XMILE_NEXT_STEP(quantity,first,origin)'):
            bad=copy.deepcopy(model);bad['components'][1]['expr']=expression
            self.assertNotEqual(self.command(bad,'lint').returncode,0)
        for field,value in [('first',1e30),('period',-1),('period',1e-10),('origin',10.125)]:
            result=self.command(model,overrides={field:value})
            self.assertNotEqual(result.returncode,0);self.assertEqual(result.stdout,'')
        bad=copy.deepcopy(model);bad['integrator']='rk4'
        self.assertNotEqual(self.command(bad,'lint').returncode,0)
        for mode in ('hybrid_completion','abm_adoption','agent_pool_sd'):
            bad=json.loads((ROOT/'models'/f'{mode}.ir.json').read_text())
            flow=next(c for c in bad['sd']['components'] if c['kind']=='flow')
            flow['expr']='XMILE_NEXT_STEP('+flow['expr']+',0,0)'
            self.assertNotEqual(self.command(bad,'lint').returncode,0)

    def test_policy_rejections_and_cli(self):
        with self.assertRaises(common.Unsupported):self.parse(common.document('1'),policy='unknown')
        for expr in ('PULSE(1,0,-1)','PULSE(1,0,1e-10)','PULSE(1,0,1e10)',
                     'STEP(1,TIME)','PULSE(1,0,TIME)','PULSE(1,1e8,1)',
                     'PULSE(1,0,.000001)'):
            with self.subTest(expr=expr),self.assertRaises(common.Unsupported):self.parse(common.document(expr))
        with self.assertRaises(common.Unsupported):self.parse(common.document('STEP(1,.25)').replace('Euler','RK4'))
        with self.assertRaises(common.Unsupported):self.parse(common.document('PULSE(1,.25)'),policy='grid')
        path=self.folder/'cli.xmile';path.write_text(common.document('PULSE(1,.25)'))
        output=self.folder/'cli.json'
        result=subprocess.run([sys.executable,str(ROOT/'tools/xmile2ir.py'),str(path),'--out',str(output),
                               '--units','metadata','--input-policy','next_tick'],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(json.loads(output.with_suffix('.json.metadata.json').read_text())['input_policy'],'euler_next_tick_quantity_pulse')


if __name__=='__main__':unittest.main()
