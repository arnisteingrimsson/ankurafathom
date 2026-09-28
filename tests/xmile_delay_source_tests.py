"""Absolute clocks, filtered flow references and pinned complete delay trajectories."""
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
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[1]
FATHOM=str(Path(sys.argv.pop(1)).resolve())
sys.path.insert(0,str(ROOT/'tools'))
from xmile2ir import convert, Unsupported
ORACLE=ROOT/'tests/oracles/xmile/corpus/tests/delay_xmile'
SOURCE=(ORACLE/'test_delay_xmile.xmile').read_text()
NS='{http://docs.oasis-open.org/xmile/ns/XMILE/v1.0}'
BASE=json.loads((ROOT/'models/nonzero_start.ir.json').read_text())


class DelaySourceTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.folder=Path(self.tmp.name)

    def parse(self,text=SOURCE,outputs='all'):
        p=self.folder/'source.xmile';p.write_text(text)
        return convert(p,units='metadata',outputs=outputs)

    def command(self,model,action='run',overrides=None):
        p=self.folder/'model.json';p.write_text(json.dumps(model))
        command=[FATHOM,action,str(p)]
        if overrides is not None:
            e=self.folder/'experiment.json'
            e.write_text(json.dumps(dict(seed=0,replications=1,scenarios=[dict(id=0,parameters=overrides)])))
            command+=['--experiment',str(e)]
        return subprocess.run(command,text=True,capture_output=True)

    def rows(self,model,overrides=None):
        r=self.command(model,overrides=overrides)
        self.assertEqual(r.returncode,0,r.stderr)
        return {(float(v['time']),v['output_id']):float(v['value']) for v in csv.DictReader(io.StringIO(r.stdout))}

    def rejected(self,model,code,pointer,action='lint'):
        r=self.command(model,action)
        self.assertNotEqual(r.returncode,0)
        self.assertEqual(r.stdout,'')
        self.assertIn((code,pointer),[(d['code'],d['pointer']) for d in json.loads(r.stderr)['diagnostics']])

    def test_pinned_complete_trajectories_both_orders(self):
        with (ORACLE/'output.tab').open(newline='') as f:
            reference=list(csv.DictReader(f,delimiter='\t'))
        count=0
        for reverse in (False,True):
            tree=ET.fromstring(SOURCE)
            variables=tree.find(NS+'model/'+NS+'variables')
            if reverse: variables[:]=list(reversed(variables))
            model,metadata=self.parse(ET.tostring(tree,encoding='unicode'))
            self.assertEqual(model['time'],dict(unit='1',start=1,dt=1,horizon=12))
            self.assertEqual(metadata['outputs'],'all')
            self.assertEqual(len(model['outputs']),8)
            rows=self.rows(model)
            names={v['id']+'_ts':v['name'] for v in metadata['variables']}
            self.assertEqual(len(rows),104)
            for r in reference:
                time=float(r['Time'])
                for output,name in names.items():
                    actual=rows[time,output];expected=float(r[name])
                    self.assertTrue(math.isclose(actual,expected,rel_tol=1e-12,abs_tol=1e-12),
                                    (reverse,time,name,actual,expected))
                    count+=1
        self.assertEqual(count,208)
        print('208 pinned delay-source observations (26 stocks, 156 auxiliaries, 26 flows)')
        model,metadata=self.parse(outputs='default')
        self.assertEqual(len(model['outputs']),1)
        self.assertEqual(metadata['outputs'],'stocks_only')

    def test_active_sign_filter_default_initial_and_delay_release(self):
        text=SOURCE.replace('Converter_1*Stock_1','3-TIME')
        model,metadata=self.parse(text)
        rows=self.rows(model);ids={v['name']:v['id']+'_ts' for v in metadata['variables']}
        for time in range(1,14):
            self.assertEqual(rows[time,ids['Flow 2']],max(3-time,0))
            self.assertEqual(rows[time,ids['Stock 1']],100 if time==1 else 102 if time==2 else 103)
            expected=max(8-time,0) if time>=6 else 2
            self.assertEqual(rows[time,ids['Delay Flow 2']],expected)
            self.assertEqual(rows[time,ids['Delay Flow 2 20']],expected if time>=6 else -20)
        # Default initialization must also filter a negative initial rate.
        model,metadata=self.parse(SOURCE.replace('Converter_1*Stock_1','TIME-3'))
        rows=self.rows(model);ids={v['name']:v['id']+'_ts' for v in metadata['variables']}
        for time in range(1,14):
            self.assertEqual(rows[time,ids['Delay Flow 2']],max(time-8,0))
        # Aliases must preserve filtering in both initialized and runtime values.
        text=SOURCE.replace('Converter_1*Stock_1','TIME-3').replace('DELAY(Flow_2,','DELAY(alias,')
        text=text.replace('</variables>','<aux name="alias"><eqn>Flow_2</eqn></aux></variables>')
        model,metadata=self.parse(text)
        rows=self.rows(model);ids={v['name']:v['id']+'_ts' for v in metadata['variables']}
        for time in range(1,14):
            self.assertEqual(rows[time,ids['alias']],max(time-3,0))
            self.assertEqual(rows[time,ids['Delay Flow 2']],max(time-8,0))

    def test_absolute_clock_units_delays_overrides_and_rk4(self):
        rows=self.rows(BASE)
        for index,(amount,lagged) in enumerate(zip([0,5,10.25,15.75,21.5],[-1,-1,5,5.25,5.5])):
            time=10+index*.5
            self.assertEqual(rows[time,'clock'],time)
            self.assertEqual(rows[time,'amount_ts'],amount)
            self.assertEqual(rows[time,'lagged_ts'],lagged)
        rows=self.rows(BASE,{'slope':2})
        self.assertEqual(rows[12,'amount_ts'],43)
        self.assertEqual(rows[12,'lagged_ts'],11)
        model=copy.deepcopy(BASE);model['integrator']='rk4'
        model['components'].pop();model['outputs'].pop()
        for start in (10,-2):
            model['time']['start']=start
            # Pure NONNEGATIVE is compatible with RK4 and has no nominal dt dependency.
            model['components'][1]['expr']='slope*t'
            model['components'][1]['non_negative']=False
            model['components'][0]['non_negative']=False
            rows=self.rows(model)
            for i in range(5):
                t=start+i*.5
                self.assertAlmostEqual(rows[t,'amount_ts'],(t*t-start*start)/2)
            model['components'][1]['expr']='NONNEGATIVE(slope*t)'
            rows=self.rows(model)
            self.assertEqual(rows[start+2,'amount_ts'],0 if start<0 else 22)

    def test_source_time_initialization_and_shift(self):
        text=SOURCE.replace('<eqn>100</eqn>','<eqn>TIME*100</eqn>').replace('<start>1</start>','<start>2</start>')
        model,metadata=self.parse(text)
        rows=self.rows(model);ids={v['name']:v['id']+'_ts' for v in metadata['variables']}
        self.assertEqual(rows[2,ids['Stock 1']],200)
        self.assertEqual(rows[2,ids['Delay Stock 1']],200)
        self.assertEqual(rows[2,ids['Delay Flow 2']],4)
        self.assertEqual(rows[3,ids['Stock 1']],204)
        model,metadata=self.parse(SOURCE.replace('<start>1</start>','<start>-2</start>').replace('<stop>13</stop>','<stop>10</stop>'))
        rows=self.rows(model);stock=next(o['id'] for o in model['outputs'] if 'stock' in o)
        self.assertAlmostEqual(rows[10,stock],100*1.02**12)

    def test_clock_and_expression_rejections(self):
        for value in ('1',True,None):
            model=copy.deepcopy(BASE);model['time']['start']=value
            self.rejected(model,'IR_TYPE','/time/start')
        for start,dt,horizon,method in [(1e20,1,2,'euler'),(1e308,1e308,1e308,'euler'),(2**53,2,4,'rk4')]:
            model=copy.deepcopy(BASE);model['time'].update(start=start,dt=dt,horizon=horizon);model['integrator']=method
            self.rejected(model,'IR_TIME','/time/start')
        for fixture in ('process','hybrid_completion','abm_adoption','agent_pool','agent_pool_sd'):
            model=json.loads((ROOT/'models'/f'{fixture}.ir.json').read_text());model['time']['start']=0
            self.rejected(model,'IR_FIELD','/time/start')
        model=copy.deepcopy(BASE);model['time']['horizon']=1e-12
        self.rejected(model,'IR_TIME','/time')
        model=copy.deepcopy(BASE);model['outputs'][1]['expr']='NONNEGATIVE(slope)'
        self.rejected(model,'IR_UNIT','/outputs/1/unit')
        model=copy.deepcopy(BASE);model['outputs'][1]['expr']='NONNEGATIVE(t/(t-t))'
        model['outputs'][1]['unit']='1'
        self.rejected(model,'IR_OUTPUT_RUNTIME','/outputs/1/expr',action='run')
        # Duration initialization must use actual start; zero would have been valid.
        model=copy.deepcopy(BASE);model['components'][2]['duration']='dt-t'
        self.rejected(model,'IR_DELAY','/components/2/duration')

    def test_vendor_allowlist_and_bounded_flow_references(self):
        for old,new in [('isee:instantaneous_flows="false"','isee:instantaneous_flows="true"'),
                        ('isee:restore_on_start="false"','isee:restore_on_start="true"'),
                        ('enabled="false"','enabled="true"'),('isee:simulation_delay="0.115385"','isee:simulation_delay="-1"'),
                        ('<isee:prefs ','<isee:prefs unknown="1" '),('<default_format/>','<default_format><unknown/></default_format>'),
                        ('<start>1</start>','<start>13</start>')]:
            with self.subTest(new=new),self.assertRaises(Unsupported): self.parse(SOURCE.replace(old,new))
        # The same filtered rate is not safe to inline once source-stock limits apply.
        text=SOURCE.replace('<inflow>Flow_2</inflow>','<outflow>Flow_2</outflow>')
        with self.assertRaisesRegex(Unsupported,'reference clipped flow'):
            self.parse(text,outputs='default')
        with self.assertRaisesRegex(Unsupported,'stock-limited'):
            self.parse(text)
        with self.assertRaisesRegex(Unsupported,'outputs'):
            self.parse(outputs='unknown')
        source=self.folder/'cli.xmile';source.write_text(SOURCE)
        out=self.folder/'cli.json'
        r=subprocess.run([sys.executable,str(ROOT/'tools/xmile2ir.py'),str(source),'--units','metadata','--outputs','all','--out',str(out)],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(len(json.loads(out.read_text())['outputs']),8)


if __name__=='__main__': unittest.main()
