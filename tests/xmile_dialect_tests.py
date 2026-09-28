"""Explicit cascade dialect and grid-aligned XMILE input-function contracts."""
import copy
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

ROOT=Path(__file__).resolve().parents[1]
FATHOM=str(Path(sys.argv.pop(1)).resolve())
sys.path.insert(0,str(ROOT/'tools'))
from xmile2ir import convert, Unsupported
ORACLE=ROOT/'tests/oracles/xmile'
NS='{http://docs.oasis-open.org/xmile/ns/XMILE/v1.0}'


def document(expression, extra='', start=0, dt=.5, horizon=4):
    return f'''<xmile xmlns="{NS[1:-1]}" version="1.0"><sim_specs method="Euler">
    <start>{start}</start><stop>{start+horizon}</stop><dt>{dt}</dt></sim_specs><model><variables>
    <stock name="total"><eqn>0</eqn><inflow>rate</inflow></stock>
    <flow name="rate"><eqn>{expression}</eqn></flow>{extra}</variables></model></xmile>'''


class DialectTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.folder=Path(self.tmp.name)

    def parse(self,source,policy='constant'):
        path=self.folder/'source.xmile';path.write_text(source)
        return convert(path,units='metadata',outputs='all',delayn_policy=policy)

    def command(self,model,action='run',overrides=None):
        path=self.folder/'model.json';path.write_text(json.dumps(model))
        command=[FATHOM,action,str(path)]
        if overrides is not None:
            experiment=self.folder/'experiment.json'
            experiment.write_text(json.dumps(dict(seed=0,replications=1,scenarios=[dict(id=0,parameters=overrides)])))
            command+=['--experiment',str(experiment)]
        return subprocess.run(command,text=True,capture_output=True)

    def rows(self,model,metadata=None,overrides=None):
        result=self.command(model,overrides=overrides)
        self.assertEqual(result.returncode,0,result.stderr)
        names={v['id']+'_ts':v['name'] for v in metadata['variables']} if metadata else {}
        return {(float(r['time']),names.get(r['output_id'],r['output_id'])):float(r['value'])
                for r in csv.DictReader(io.StringIO(result.stdout))}

    def test_expanded_stock_oracle_and_dialect_discrepancy(self):
        reference=json.loads((ORACLE/'delayn-reference.json').read_text())
        self.assertEqual(reference['generator_sha256'],hashlib.sha256((ORACLE/'generate_delayn.py').read_bytes()).hexdigest())
        for name in ('cascade','history_diagnostic'):
            case=reference[name]
            self.assertEqual(case['source_sha256'],hashlib.sha256((ORACLE/case['source']).read_bytes()).hexdigest())
        self.assertGreater(reference['maximum_dialect_gap'],.1)
        source=(ORACLE/reference['history_diagnostic']['source']).read_text()
        with self.assertRaisesRegex(Unsupported,'DELAYN duration must be constant'):self.parse(source)
        comparisons=0
        for reverse in (False,True):
            tree=ET.fromstring(source);variables=tree.find(NS+'model/'+NS+'variables')
            if reverse:variables[:]=list(reversed(variables))
            model,metadata=self.parse(ET.tostring(tree,encoding='unicode'),'cascade')
            self.assertEqual(metadata['delayn_policy'],'cascade')
            self.assertEqual(len(metadata['stateful_calls']),8)
            rows=self.rows(model,metadata)
            for time,values in zip(reference['cascade']['times'],reference['cascade']['values']):
                for name,expected in zip(reference['columns'],values):
                    self.assertTrue(math.isclose(rows[time,name],expected,rel_tol=2e-12,abs_tol=2e-12),
                                    (reverse,time,name,rows[time,name],expected))
                    comparisons+=1
        self.assertEqual(comparisons,1386)
        print('1386 explicit-stock PySD cascade comparisons; history dialect retained only as a discrepancy diagnostic')

    def test_cascade_hand_oracle_and_existing_orders(self):
        extra='<aux name="tau"><eqn>TIME</eqn><gf><xpts>0,.5,1,1.5</xpts><ypts>4,2,6,6</ypts></gf></aux>'
        extra+='<aux name="input"><eqn>TIME</eqn><gf><xpts>0,.5,1,1.5</xpts><ypts>6,0,3,3</ypts></gf></aux>'
        model,metadata=self.parse(document('DELAYN(input,tau,2,2)',extra,horizon=1.5),'cascade')
        rows=self.rows(model,metadata)
        for tick,(rate,amount) in enumerate(zip([2,4,5/3,14/9],[0,1,3,23/6])):
            self.assertAlmostEqual(rows[tick*.5,'rate'],rate)
            self.assertAlmostEqual(rows[tick*.5,'total'],amount)
        for order in (1,3):
            source=document(f'DELAYN(4,3+TIME,{order},1)')
            a,am=self.parse(source,'cascade')
            b,bm=self.parse(source.replace(f'DELAYN(4,3+TIME,{order},1)',f'DELAY{order}(4,3+TIME,1)'))
            self.assertEqual(self.rows(a,am),self.rows(b,bm))

    def test_cascade_policy_and_failures(self):
        with self.assertRaisesRegex(Unsupported,'policy'):self.parse(document('1'),'history')
        for expr in ('DELAYN(1,1+TIME,3)','DELAYN(-1,3+TIME,2)',
                     'DELAYN(1,3+TIME,2+TIME)','DELAYN(1,SMTH1(3,2),2)'):
            with self.subTest(expr=expr),self.assertRaises(Unsupported):self.parse(document(expr),'cascade')
        # Duration becomes invalid only at the final observation. No partial CSV escapes.
        model,_=self.parse(document('DELAYN(1,3-TIME,2)',dt=.5,horizon=2.5),'cascade')
        result=self.command(model)
        self.assertNotEqual(result.returncode,0);self.assertEqual(result.stdout,'')
        self.assertIn('IR_DELAY_RUNTIME',result.stderr)
        model,_=self.parse(document('DELAYN(1-TIME,3+TIME,2,0)'),'cascade')
        result=self.command(model)
        self.assertNotEqual(result.returncode,0);self.assertEqual(result.stdout,'')
        # Initialization must rebuild the material pipeline after overrides.
        model,metadata=self.parse(document('DELAYN(4,3+TIME,2,2)'),'cascade')
        model['parameters']=[dict(id='tau0',value=3,unit='1')]
        next(c for c in model['components'] if c['kind']=='delay')['duration']='tau0+t'
        rows=self.rows(model,metadata,{'tau0':6})
        self.assertEqual(rows[0,'rate'],2)
        self.assertAlmostEqual(rows[.5,'rate'],12/6.5)
        source=self.folder/'cli.xmile';source.write_text(document('DELAYN(1,3+TIME,2)'))
        out=self.folder/'converted.json'
        result=subprocess.run([sys.executable,str(ROOT/'tools/xmile2ir.py'),str(source),'--out',str(out),
                               '--units','metadata','--delayn-policy','cascade'],text=True,capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(json.loads(out.with_suffix('.json.metadata.json').read_text())['delayn_policy'],'cascade')

    def test_source_input_trajectories_and_quantity(self):
        for start in (0,10,-2):
            for dt in (.1,.125,.5,1):
                first=start+2*dt
                extra=f'<aux name="first"><eqn>{first}</eqn></aux><aux name="period"><eqn>{4*dt}</eqn></aux>'
                expressions=[('STEP(3,first)',lambda k:3 if k>=2 else 0),
                    ('RAMP(-2,first)',lambda k:-2*max(k-2,0)*dt),
                    ('PULSE(-3,first,period)',lambda k:-3/dt if k>=2 and (k-2)%4==0 else 0),
                    ('PULSE(5,first)',lambda k:5/dt if k==2 else 0),
                    ('PULSE(5,first,0)',lambda k:5/dt if k==2 else 0),
                    ('STEP(TIME,first)',lambda k:start+k*dt if k>=2 else 0)]
                for expression,rate in expressions:
                    with self.subTest(start=start,dt=dt,expression=expression):
                        model,metadata=self.parse(document(expression,extra,start,dt,20*dt))
                        self.assertEqual(metadata['input_policy'],'euler_grid_aligned_quantity_pulse')
                        rows=self.rows(model,metadata);amount=0
                        # Include final output; the final pulse is not integrated past horizon.
                        for k in range(21):
                            t=start+k*dt
                            key=min((time for time,name in rows if name=='rate'),key=lambda time:abs(time-t))
                            self.assertAlmostEqual(rows[key,'rate'],rate(k),places=11)
                            self.assertAlmostEqual(rows[key,'total'],amount,places=11)
                            amount+=rate(k)*dt
                        if expression.startswith('PULSE(-3'):self.assertAlmostEqual(rows[key,'total'],-15)

    def test_input_initialization_nesting_and_boundary(self):
        source=document('DELAYN(PULSE(4,10),2+STEP(1,11),2)',start=10,dt=.5,horizon=2)
        model,metadata=self.parse(source,'cascade');rows=self.rows(model,metadata)
        self.assertEqual(rows[10,'rate'],8)  # default initial output is quantity/dt
        self.assertEqual(rows[10.5,'rate'],8)
        model,metadata=self.parse(document('PULSE(3,-2,2)',dt=.5))
        rows=self.rows(model,metadata)
        self.assertEqual([rows[t,'total'] for t in (0,.5,2,2.5,4)],[0,3,3,6,6])
        self.assertEqual(rows[4,'rate'],6)
        source=document('STEP(2,0)+RAMP(2,0)+PULSE(2,0)')
        source=source.replace('<eqn>0</eqn>','<eqn>STEP(2,0)+RAMP(2,0)+PULSE(2,0)</eqn>',1)
        model,metadata=self.parse(source)
        self.assertEqual(self.rows(model,metadata)[0,'total'],6)
        # Compute elapsed time before multiplying: slope*tick_count can overflow
        # even though the initial ramp value is representable when dt is small.
        source=document('0',start=.2,dt=.1,horizon=.2)
        source=source.replace('<eqn>0</eqn>','<eqn>RAMP(1e308,0)</eqn>',1)
        model,metadata=self.parse(source)
        self.assertEqual(self.rows(model,metadata)[.2,'total'],1e308*.2)

    def test_input_rejections(self):
        for expression in ('STEP(1,.25)','PULSE(1,0,-1)','PULSE(1,0,.75)',
                           'PULSE(1,0,.1)','PULSE(1,TIME)','STEP(1,total)',
                           'PULSE(1,0,TIME)','STEP(1)','RAMP(1,0,1)','PULSE(1,0,1,2)',
                           'STEP(1,1000001)','PULSE(1,0,1000001)', 'STEP+1'):
            with self.subTest(expression=expression),self.assertRaises(Unsupported):self.parse(document(expression))
        with self.assertRaisesRegex(Unsupported,'Euler'):
            self.parse(document('STEP(1,0)').replace('method="Euler"','method="RK4"'))
        with self.assertRaises(Unsupported):self.parse(document('STEP(1,1000000000)',start=1000000000))

    def test_native_units_overrides_and_restrictions(self):
        model=json.loads((ROOT/'models/xmile_inputs.ir.json').read_text())
        for quantity in (3,-6):
            rows=self.rows(model,overrides={'quantity':quantity})
            amount=0
            for k in range(9):
                t=10+k*.5;rate=(quantity/.5 if k%2==0 else 0)+(2 if k>=2 else 0)+max(t-11,0)
                self.assertEqual(rows[t,'rate'],rate)
                self.assertEqual(rows[t,'amount_ts'],amount)
                amount+=rate*.5
        invalid=copy.deepcopy(model);invalid['integrator']='rk4'
        result=self.command(invalid,'lint');self.assertNotEqual(result.returncode,0);self.assertIn('IR_INTEGRATOR',result.stderr)
        for expr in ('XMILE_PULSE(quantity,quantity,period)','XMILE_PULSE(quantity,first)',
                     'XMILE_RAMP(slope,first,period)','XMILE_STEP(height,quantity)'):
            invalid=copy.deepcopy(model);invalid['components'][1]['expr']=expr
            self.assertNotEqual(self.command(invalid,'lint').returncode,0)
        invalid=copy.deepcopy(model);invalid['parameters'][1]['value']=10.25
        result=self.command(invalid);self.assertNotEqual(result.returncode,0);self.assertEqual(result.stdout,'')
        for name in ('hybrid_completion','abm_adoption','agent_pool_sd'):
            hybrid=json.loads((ROOT/'models'/f'{name}.ir.json').read_text())
            flow=next(c for c in hybrid['sd']['components'] if c['kind']=='flow')
            flow['expr']='XMILE_STEP('+flow['expr']+',0)'
            self.assertNotEqual(self.command(hybrid,'lint').returncode,0)


if __name__=='__main__':unittest.main()
