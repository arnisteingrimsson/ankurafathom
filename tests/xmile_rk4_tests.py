"""RK4 source mappings use closed forms and independent SciPy, never Euler PySD."""
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
from xmile2ir import convert, Unsupported, normalized
ORACLE=ROOT/'tests/oracles/xmile'
NS='http://docs.oasis-open.org/xmile/ns/XMILE/v1.0'


def document(variables, method='RK4'):
    return f'<xmile xmlns="{NS}" version="1.0"><sim_specs method="{method}"><start>0</start><stop>2</stop><dt>1</dt></sim_specs><model><variables>{variables}</variables></model></xmile>'


class RK4Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.folder=Path(self.tmp.name)

    def parse(self,text):
        p=self.folder/'source.xmile';p.write_text(text)
        return convert(p,units='metadata')

    def run_ir(self,model,metadata):
        p=self.folder/'model.json';p.write_text(json.dumps(model))
        result=subprocess.run([FATHOM,'run',str(p)],text=True,capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr)
        names={v['id']+'_ts':normalized(v['name']) for v in metadata['variables']}
        return {(float(r['time']),names[r['output_id']]):float(r['value']) for r in csv.DictReader(io.StringIO(result.stdout))}

    def test_source_trajectories_against_independent_scipy(self):
        references=json.loads((ROOT/'tests/oracles/sd/reference.json').read_text())
        count=0
        for name in ('sir','time_growth'):
            reference=next(m for m in references['models'] if m['name']==name)
            source=ORACLE/'fixtures'/f'rk4_{name}.xmile'
            for reverse in (False,True):
                tree=ET.fromstring(source.read_text())
                if reverse:
                    variables=tree.find('{'+NS+'}model/{'+NS+'}variables')
                    variables[:]=list(reversed(variables))
                model,metadata=self.parse(ET.tostring(tree,encoding='unicode'))
                self.assertEqual(model['integrator'],'rk4')
                self.assertEqual(metadata['method'],'RK4')
                previous=None
                for divisor in (1,2,4):
                    model['time']['dt']=reference['base_dt']/divisor
                    rows=self.run_ir(model,metadata)
                    error=0
                    for t,values in zip(reference['times'],reference['values']):
                        for stock,expected in zip(reference['stocks'],values):
                            error=max(error,abs(rows[t,stock]-expected))
                            count+=1
                        if name=='sir':
                            self.assertAlmostEqual(sum(rows[t,s] for s in reference['stocks']),1,places=13)
                            self.assertTrue(all(rows[t,s]>=0 for s in reference['stocks']))
                    if previous is not None:
                        self.assertTrue(13<previous/error<19,(name,divisor,previous/error))
                    previous=error
                self.assertLess(previous,1e-6)
        self.assertEqual(count,1272)
        print(f'{count} RK4 observations scored against pinned SciPy references with fourth-order convergence')

    def test_upstream_rk4_source_and_euler_export_discrepancy(self):
        source=ORACLE/'corpus/tests/zeroled_decimals/test_zeroled_decimals.xmile'
        expected_functions={
            'stock0':lambda t:.34, 'stock1neg':lambda t:-.1*t,
            'stock1pos':lambda t:.4+.4*t, 'stockmixed':lambda t:-.6777*t-t*t/2,
            'stocknegatives':lambda t:.25+.66997*t, 'stockpositives':lambda t:1.06*t}
        with source.with_name('output.tab').open(newline='') as stream:
            historical=list(csv.DictReader(stream,delimiter='\t'))
        self.assertEqual(len(historical),11)
        for reverse in (False,True):
            tree=ET.fromstring(source.read_text())
            if reverse:
                variables=tree.find('{'+NS+'}model/{'+NS+'}variables')
                variables[:]=list(reversed(variables))
            model,metadata=self.parse(ET.tostring(tree,encoding='unicode'))
            self.assertEqual(model['integrator'],'rk4')
            self.assertEqual(len(metadata['promoted_aux_flows']),10)
            actual=self.run_ir(model,metadata)
            self.assertEqual(len(actual),66)
            for t in range(11):
                for name,solution in expected_functions.items():
                    self.assertTrue(math.isclose(actual[t,name],solution(t),rel_tol=2e-13,abs_tol=2e-13))
            # Explicit diagnostic rerun, not the imported source method.
            euler=copy.deepcopy(model);euler['integrator']='euler'
            legacy=self.run_ir(euler,metadata)
            mismatches=0
            for row in historical:
                t=float(row['Time'])
                for name in expected_functions:
                    self.assertTrue(math.isclose(legacy[t,name],float(row[name]),rel_tol=2e-12,abs_tol=2e-12))
                    if name=='stockmixed':
                        self.assertAlmostEqual(float(row[name])-actual[t,name],t/2,places=12)
                        mismatches+=t>0
                    else:
                        self.assertTrue(math.isclose(actual[t,name],float(row[name]),rel_tol=2e-12,abs_tol=2e-12))
            self.assertEqual(mismatches,10)

    def test_linked_auxiliary_conservation_sharing_and_metadata(self):
        variables='<stock name="a"><eqn>10</eqn><outflow>q</outflow></stock><stock name="b"><eqn>0</eqn><inflow>q</inflow></stock><aux name="q"><eqn>a</eqn></aux><aux name="unused"><eqn>1</eqn></aux>'
        for method in ('Euler','RK4','rK4'):
            model,metadata=self.parse(document(variables,method))
            self.assertEqual(len(metadata['promoted_aux_flows']),1)
            self.assertEqual(len([c for c in model['components'] if c['kind']=='flow']),1)
            rows=self.run_ir(model,metadata)
            for t in range(3): self.assertAlmostEqual(rows[t,'a']+rows[t,'b'],10,places=13)
            self.assertAlmostEqual(rows[1,'a'],0 if method=='Euler' else 3.75,places=13)
        with self.assertRaisesRegex(Unsupported,'multiply connected'):
            self.parse(document(variables.replace('<inflow>q</inflow>','<inflow>q</inflow>'*2)))
        with self.assertRaisesRegex(Unsupported,'unknown'):
            self.parse(document(variables.replace('<inflow>q','<inflow>a')))
        model,metadata=self.parse(document('<aux name="a"><eqn>TIME*TIME</eqn></aux>'))
        self.assertEqual(metadata['outputs'],'auxiliaries_only')
        self.assertEqual([v for v in self.run_ir(model,metadata).values()],[0,1,4])

    def test_unsupported_methods_and_stateful_rk4(self):
        for method in ('RK2','RK45','gear, rk4'):
            with self.assertRaisesRegex(Unsupported,'only Euler and RK4'):
                self.parse(document('<aux name="a"><eqn>1</eqn></aux>',method))
        for expression in ('SMTH1(1,2)','DELAY(1,2)','SMTHN(1,4,2)','DELAYN(1,4,2)'):
            with self.assertRaisesRegex(Unsupported,'require Euler'):
                self.parse(document(f'<aux name="a"><eqn>{expression}</eqn></aux>'))


if __name__=='__main__': unittest.main()
