"""Bounded history-order acceptance, historical evidence, and failed-order audit."""
import copy
import csv
import hashlib
import json
import math
import subprocess
import sys
import unittest
import xml.etree.ElementTree as ET
import xmile_dialect_tests as common

ROOT=common.ROOT;ORACLE=common.ORACLE;NS=common.NS


class HistoryTests(unittest.TestCase):
    setUp=common.DialectTests.setUp
    parse=common.DialectTests.parse
    command=common.DialectTests.command
    rows=common.DialectTests.rows

    def test_complete_pysd_trajectories_and_source_order(self):
        reference=json.loads((ORACLE/'history-reference.json').read_text())
        case=reference['history2'];source=ORACLE/case['source']
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),case['source_sha256'])
        self.assertEqual(hashlib.sha256((ORACLE/'generate_history.py').read_bytes()).hexdigest(),reference['generator_sha256'])
        count=0
        for reverse in (False,True):
            tree=ET.fromstring(source.read_text());variables=tree.find(NS+'model/'+NS+'variables')
            if reverse:variables[:]=list(reversed(variables))
            model,metadata=self.parse(ET.tostring(tree,encoding='unicode'),'history2')
            self.assertEqual(metadata['delayn_policy'],'history2')
            self.assertTrue(all(c['type']=='history2' for c in model['components'] if c['kind']=='delay'))
            rows=self.rows(model,metadata)
            for time,values in zip(case['times'],case['values']):
                for name,expected in zip(reference['columns'],values):
                    self.assertTrue(math.isclose(rows[time,name],expected,rel_tol=2e-12,abs_tol=2e-12),
                                    (reverse,time,name,rows[time,name],expected))
                    count+=1
        self.assertEqual(count,1386)
        print('1386 PySD order-2 history observations, including reversed source order')

    def test_historical_vensim_output(self):
        evidence=json.loads((ORACLE/'history-reference.json').read_text())['historical_order2']
        for key in ('source','reference'):
            self.assertEqual(hashlib.sha256((ORACLE/evidence[key]).read_bytes()).hexdigest(),evidence[key+'_sha256'])
        # Explicit projection of the source's one DELAY N component. Its order
        # expression initializes to 2; Vensim freezes order at initialization.
        model=dict(ir_version='0.1',name='vensim_history2_projection',
            time=dict(unit='1',dt=1,horizon=100),parameters=[],components=[
                dict(id='lag',kind='delay',type='history2',order=2,duration='4+STEP(2,15)',
                     initial=6,input='-1+STEP(5,5)',unit='1')],outputs=[dict(id='lag_ts',component='lag')])
        rows=self.rows(model)
        with (ORACLE/evidence['reference']).open(newline='') as f: historical=list(csv.DictReader(f,delimiter='\t'))
        self.assertEqual(len(rows),101)
        for row,time,pysd_value in zip(historical,evidence['times'],evidence['values']):
            value=rows[time,'lag_ts']
            self.assertTrue(math.isclose(value,float(row['OutputDelayN']),rel_tol=1e-5,abs_tol=5e-6))
            self.assertTrue(math.isclose(value,pysd_value,rel_tol=2e-12,abs_tol=2e-12))
        self.assertAlmostEqual(rows[15,'lag_ts'],3.94970703125)
        self.assertAlmostEqual(rows[16,'lag_ts'],2.2044219970703125,places=4)

    def test_conservation_report_does_not_accept_other_orders(self):
        report=json.loads((ORACLE/'history-reference.json').read_text())
        self.assertFalse(report['history_all_orders_accepted'])
        self.assertEqual(report['integer_duration_diagnostic'],dict(requested_duration=4.5,stored_duration=4.0))
        self.assertEqual(len(report['conservation']),24)
        for case in report['conservation']:
            expected=case['order']==2 or case['duration_after']==4
            self.assertEqual(case['conserves'],expected)
            self.assertEqual(case['conserves'],case['max_balance_error']<1e-10)
            self.assertLess(abs(case['remaining']),1e-12)
            self.assertAlmostEqual(case['balance_error'],8-case['emitted']-case['remaining'],places=12)
        failed=[c for c in report['conservation'] if not c['conserves']]
        self.assertEqual(len(failed),12)

    def test_history_hand_initialization_overrides_and_failures(self):
        extra='<aux name="tau"><eqn>TIME</eqn><gf><xpts>0,.5,1,1.5</xpts><ypts>4,2,6,6</ypts></gf></aux>'
        extra+='<aux name="input"><eqn>TIME</eqn><gf><xpts>0,.5,1,1.5</xpts><ypts>6,0,3,3</ypts></gf></aux>'
        model,metadata=self.parse(common.document('DELAYN(input,tau,2,2)',extra,horizon=1.5),'history2')
        rows=self.rows(model,metadata)
        for k,(rate,total) in enumerate(zip([2,2,6,7/6],[0,1,2,5])):
            self.assertAlmostEqual(rows[k*.5,'rate'],rate);self.assertEqual(rows[k*.5,'total'],total)
        for initial in ('',' ,-2'):
            model,metadata=self.parse(common.document('DELAYN(-3,4+TIME,2'+initial+')'),'history2')
            self.assertEqual(self.rows(model,metadata)[0,'rate'],-3 if not initial else -2)
        native=json.loads((ROOT/'models/history_delay.ir.json').read_text())
        rows=self.rows(native,overrides={'base_tau':8,'feed':-4})
        self.assertEqual(rows[0,'lag_ts'],2);self.assertEqual(rows[.5,'lag_ts'],2)
        self.assertAlmostEqual(rows[1,'lag_ts'],47/18)
        for order in (1,3,5,255):
            with self.assertRaisesRegex(common.Unsupported,'requires order 2'):
                self.parse(common.document(f'DELAYN(1,256+TIME,{order})'),'history2')
            invalid=copy.deepcopy(native);invalid['components'][2]['order']=order
            result=self.command(invalid,'lint');self.assertNotEqual(result.returncode,0);self.assertIn('IR_DELAY',result.stderr)
        model,_=self.parse(common.document('DELAYN(1,3-TIME,2)',horizon=2.5),'history2')
        result=self.command(model);self.assertNotEqual(result.returncode,0);self.assertEqual(result.stdout,'')
        self.assertIn('IR_DELAY_RUNTIME',result.stderr)
        invalid=copy.deepcopy(native);invalid['integrator']='rk4'
        result=self.command(invalid,'lint');self.assertNotEqual(result.returncode,0);self.assertIn('IR_INTEGRATOR',result.stderr)
        source=self.folder/'cli.xmile';source.write_text(common.document('DELAYN(1,4+TIME,2)'))
        output=self.folder/'cli.json'
        result=subprocess.run([sys.executable,str(ROOT/'tools/xmile2ir.py'),str(source),'--out',str(output),
            '--units','metadata','--delayn-policy','history2'],text=True,capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(json.loads(output.with_suffix('.json.metadata.json').read_text())['delayn_policy'],'history2')


if __name__=='__main__':unittest.main()
