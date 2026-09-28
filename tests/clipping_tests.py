"""Hand-derived clipping schedules, source priority, and bounded-scope errors."""
import copy
import csv
import io
import json
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
BASE=json.loads((ROOT/'models/clipping.ir.json').read_text())
NS='http://docs.oasis-open.org/xmile/ns/XMILE/v1.0'


def source():
    return f'''<xmile xmlns="{NS}" version="1.0"><sim_specs><start>0</start><stop>2</stop><dt>.5</dt></sim_specs>
    <model><variables>
    <stock name="a"><eqn>2</eqn><non_negative/><inflow>inlet</inflow><outflow>high</outflow><outflow>low</outflow></stock>
    <stock name="b"><eqn>0</eqn><inflow>high</inflow></stock>
    <stock name="c"><eqn>0</eqn><inflow>low</inflow></stock>
    <flow name="inlet"><eqn>3-4*TIME</eqn><non_negative/></flow>
    <flow name="high"><eqn>6</eqn><non_negative/></flow>
    <flow name="low"><eqn>8</eqn><non_negative/></flow>
    </variables></model></xmile>'''


class ClippingTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.folder=Path(self.tmp.name)

    def parse(self,text):
        p=self.folder/'source.xmile';p.write_text(text)
        return convert(p,units='metadata')

    def command(self,model,action='run',overrides=None):
        p=self.folder/'model.json';p.write_text(json.dumps(model))
        command=[FATHOM,action,str(p)]
        if overrides is not None:
            e=self.folder/'experiment.json'
            e.write_text(json.dumps(dict(seed=0,replications=1,scenarios=[dict(id=0,parameters=overrides)])))
            command+=['--experiment',str(e)]
        return subprocess.run(command,text=True,capture_output=True)

    def rows(self,model,overrides=None):
        result=self.command(model,overrides=overrides)
        self.assertEqual(result.returncode,0,result.stderr)
        return {(float(r['time']),r['output_id']):float(r['value']) for r in csv.DictReader(io.StringIO(result.stdout))}

    def rejected(self,model,code,pointer,action='lint'):
        result=self.command(model,action)
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(result.stdout,'')
        self.assertIn((code,pointer),[(d['code'],d['pointer']) for d in json.loads(result.stderr)['diagnostics']])

    def schedule(self,model):
        rows=self.rows(model)
        return [[rows[t,o['id']] for o in model['outputs']] for t in (0,.5,1,1.5,2)]

    def test_native_schedule_priority_overrides_and_reordering(self):
        expected=[[2,0,0],[0,3,.5],[0,3.5,.5],[0,3.5,.5],[0,3.5,.5]]
        self.assertEqual(self.schedule(BASE),expected)
        model=copy.deepcopy(BASE);model['components'][0]['outflow_order'].reverse()
        self.assertEqual(self.schedule(model),[[2,0,0],[0,0,3.5],[0,0,4],[0,0,4],[0,0,4]])
        model=copy.deepcopy(BASE);model['components'].reverse();model['outputs'].reverse()
        self.assertEqual(self.rows(model),self.rows(BASE))
        rows=self.rows(BASE,{'arrival':0,'decline':0})
        self.assertEqual(rows[.5,'first_ts'],2)
        self.assertEqual(rows[.5,'second_ts'],0)

    def test_source_schedule_and_explicit_outflow_order(self):
        model,metadata=self.parse(source())
        expected=self.schedule(BASE)
        self.assertEqual(self.schedule(model),expected)
        self.assertEqual(metadata['clipping_policy'],'euler_current_inflows_priority_acyclic')
        tree=ET.fromstring(source());variables=tree.find('{'+NS+'}model/{'+NS+'}variables')
        variables[:]=list(reversed(variables))
        model,metadata=self.parse(ET.tostring(tree,encoding='unicode'))
        rows=self.rows(model)
        by_name={v['name']:v['id']+'_ts' for v in metadata['variables'] if v['kind']=='stock'}
        self.assertEqual([[rows[t,by_name[n]] for n in ('a','b','c')] for t in (0,.5,1,1.5,2)],expected)
        model,_=self.parse(source().replace('<outflow>high</outflow><outflow>low</outflow>',
                                          '<outflow>low</outflow><outflow>high</outflow>'))
        self.assertEqual(self.schedule(model)[1],[0,0,3.5])

    def test_source_same_tick_clipped_chain(self):
        text=source().replace('<stock name="b"><eqn>0</eqn><inflow>high</inflow>',
            '<stock name="b"><eqn>0</eqn><non_negative/><inflow>high</inflow><outflow>next</outflow>')
        text=text.replace('<inflow>low</inflow>','<inflow>low</inflow><inflow>next</inflow>')
        text=text.replace('</variables>','<flow name="next"><eqn>20</eqn><non_negative/></flow></variables>')
        model,_=self.parse(text)
        self.assertEqual(self.schedule(model),[[2,0,0],[0,0,3.5],[0,0,4],[0,0,4],[0,0,4]])

    def test_native_invalid_contracts(self):
        for key,value,pointer in [('clip_outflows','true','/components/0/clip_outflows'),
                                  ('outflow_order',1,'/components/0/outflow_order')]:
            model=copy.deepcopy(BASE);model['components'][0][key]=value
            self.rejected(model,'IR_TYPE',pointer)
        for order in ([],['high'],['low','high','high'],['inflow','low'],['missing','low']):
            model=copy.deepcopy(BASE);model['components'][0]['outflow_order']=order
            self.rejected(model,'IR_CLIPPING','/components')
        model=copy.deepcopy(BASE);model['components'][0]['non_negative']=False
        self.rejected(model,'IR_CLIPPING','/components/0/clip_outflows')
        model=copy.deepcopy(BASE);model['integrator']='rk4'
        self.rejected(model,'IR_CLIPPING','/components/0/clip_outflows')
        model=copy.deepcopy(BASE);model['components'][3].update(non_negative=False,clip_negative=False)
        self.rejected(model,'IR_CLIPPING','/components')
        model=copy.deepcopy(BASE);model['components'][3]['clip_negative']='true'
        self.rejected(model,'IR_TYPE','/components/3/clip_negative')
        model=copy.deepcopy(BASE);model['components'][3]['non_negative']=False
        self.rejected(model,'IR_CLIPPING','/components/3/clip_negative')
        for fixture in ('hybrid_completion','abm_adoption','agent_pool_sd'):
            model=json.loads((ROOT/'models'/f'{fixture}.ir.json').read_text())
            model['sd']['components'][0]['clip_outflows']=True
            self.rejected(model,'IR_FIELD','/sd/components/0/clip_outflows')

    def test_failure_has_no_partial_trajectory_and_default_policy_is_strict(self):
        model=copy.deepcopy(BASE);model['components'][3]['expr']='arrival*dt/(dt-t)'
        self.rejected(model,'IR_RUNTIME','',action='run')
        model=copy.deepcopy(BASE)
        del model['components'][0]['clip_outflows'];del model['components'][0]['outflow_order']
        self.rejected(model,'IR_RUNTIME','',action='run')
        self.assertEqual(self.schedule(BASE)[-1],[0,3.5,.5])

    def test_source_rejections_are_explicit(self):
        mutations=[
            (source().replace('<sim_specs>','<sim_specs method="RK4">'),'requires Euler'),
            (source().replace('<non_negative/>','<non_negative>true</non_negative>',1),'empty tag'),
            (source().replace('<non_negative/>','<non_negative/><non_negative/>',1),'exactly one'),
            (source().replace('<eqn>2</eqn>','<eqn>-2</eqn>'),'initial value'),
            (source().replace('<flow name="high"><eqn>6</eqn><non_negative/>','<flow name="high"><eqn>6</eqn>'),'incident flows'),
            (source().replace('<eqn>8</eqn>','<eqn>high</eqn>'),'reference clipped flow'),
            (source().replace('</variables>','<aux name="alias"><eqn>high</eqn></aux></variables>'),'reference clipped flow'),
            (source().replace('</variables>','<aux name="x"><eqn>1</eqn><non_negative/></aux></variables>'),'unlinked auxiliary'),
        ]
        for text,reason in mutations:
            with self.subTest(reason=reason),self.assertRaisesRegex(Unsupported,reason): self.parse(text)
        cycle=source().replace('<stock name="b"><eqn>0</eqn><inflow>high</inflow>',
            '<stock name="b"><eqn>0</eqn><non_negative/><inflow>high</inflow><outflow>back</outflow>')
        cycle=cycle.replace('<inflow>inlet</inflow>','<inflow>inlet</inflow><inflow>back</inflow>')
        cycle=cycle.replace('</variables>','<flow name="back"><eqn>0</eqn><non_negative/></flow></variables>')
        with self.assertRaisesRegex(Unsupported,'cyclic clipped'): self.parse(cycle)


if __name__=='__main__': unittest.main()
