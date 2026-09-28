"""Pinned cross-tool trajectories plus importer rejection and CLI contracts."""
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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
from xmile2ir import convert, normalized, Unsupported
FATHOM = str(Path(sys.argv.pop(1)).resolve())
ORACLE = ROOT/'tests/oracles/xmile'
NS = 'http://docs.oasis-open.org/xmile/ns/XMILE/v1.0'


def document(variables=None, spec=None):
    if variables is None:
        variables = '<stock name="s"><eqn>2*a</eqn><outflow>f</outflow></stock><flow name="f"><eqn>s/tau</eqn></flow><aux name="a"><eqn>5</eqn></aux><aux name="tau"><eqn>2</eqn></aux>'
    if spec is None:
        spec = '<start>0</start><stop>2</stop><dt>1</dt>'
    return f'<xmile xmlns="{NS}" version="1.0"><sim_specs>{spec}</sim_specs><model><variables>{variables}</variables></model></xmile>'


class ImportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name)

    def convert_text(self, text):
        source = self.folder/'input.xmile'
        source.write_text(text)
        return convert(source, units='metadata')

    def run_ir(self, model, success=True):
        path = self.folder/'model.json'
        path.write_text(json.dumps(model))
        result = subprocess.run([FATHOM,'run',str(path)], text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return list(csv.DictReader(io.StringIO(result.stdout))) if success else result

    def test_corpus_hashes_and_trajectories(self):
        manifest = json.loads((ORACLE/'manifest.json').read_text())
        coverage = json.loads((ORACLE/'coverage.json').read_text())
        self.assertEqual(coverage['commit'], manifest['commit'])
        self.assertEqual(coverage['importer_sha256'], hashlib.sha256((ROOT/'tools/xmile2ir.py').read_bytes()).hexdigest())
        self.assertEqual(coverage['total_xmile_files'], 67)
        self.assertEqual(coverage['counts'], {'importable': 8, 'rejected': 59})
        self.assertEqual({case['path'] for case in coverage['cases'] if case['status']=='importable'},
                         {'tests/delay_xmile/test_delay_xmile.xmile', 'samples/teacup/teacup_w_diagram.xmile', 'samples/teacup/teacup.xmile', 'samples/SIR/SIR.xmile', 'samples/SIR/SIR_reciprocal-dt.xmile', 'tests/lookups/test_lookups_no-indirect.xmile', 'tests/eval_order/eval_order.xmile', 'tests/zeroled_decimals/test_zeroled_decimals.xmile'})
        for case in coverage['cases']:
            if case['path'] in manifest['files']:
                self.assertEqual(case['sha256'], manifest['files'][case['path']])
        for path, digest in manifest['files'].items():
            self.assertEqual(hashlib.sha256((ORACLE/'corpus'/path).read_bytes()).hexdigest(), digest)
        total = 0
        for relative in ('samples/teacup/teacup_w_diagram.xmile','samples/teacup/teacup.xmile','samples/SIR/SIR.xmile','samples/SIR/SIR_reciprocal-dt.xmile'):
            with self.subTest(model=relative):
                source = ORACLE/'corpus'/relative
                model, metadata = convert(source, units='metadata')
                self.assertEqual(metadata['unit_policy'], 'metadata_only')
                names = {v['id']+'_ts': normalized(v['name']) for v in metadata['variables'] if v['kind']=='stock'}
                actual = self.run_ir(model)
                for reference, value_rtol, time_atol in (
                        ('output_stella1006.csv', 5.1e-12, .00050000001),
                        ('output.csv', 1e-5, 1e-9)):
                    with source.with_name(reference).open(newline='') as stream:
                        expected = [{normalized(k): float(v) for k,v in row.items()} for row in csv.DictReader(stream)]
                    self.assertEqual(len(actual),len(expected)*len(names))
                    for index, row in enumerate(expected):
                        # Stella prints time to 3 decimal places, Vensim to 6 significant digits.
                        self.assertTrue(math.isclose(index*model['time']['dt'],row['time'],rel_tol=5.1e-6 if reference=='output.csv' else 0,abs_tol=time_atol))
                        for j, (output, name) in enumerate(names.items()):
                            observed = actual[index*len(names)+j]
                            self.assertEqual(observed['output_id'],output)
                            self.assertEqual(float(observed['time']),index*model['time']['dt'])
                            self.assertTrue(math.isclose(float(observed['value']),row[name],rel_tol=value_rtol,abs_tol=1e-13), (relative,reference,index,name,observed['value'],row[name]))
                            total += 1
        self.assertEqual(total,39376)
        print(f'Compared {total} stock observations against pinned Stella and Vensim exports')

    def test_diagram_clipping_mapping(self):
        model, metadata = convert(ORACLE/'corpus/samples/teacup/teacup_w_diagram.xmile',units='metadata')
        self.assertEqual(metadata['clipping_policy'], 'euler_current_inflows_priority_acyclic')
        self.assertTrue(any(c.get('clip_outflows') for c in model['components']))
        self.assertTrue(any(c.get('clip_negative') for c in model['components']))

    def test_upstream_auxiliary_trajectory(self):
        source=ORACLE/'corpus/tests/eval_order/eval_order.xmile'
        model,metadata=convert(source,units='metadata')
        self.assertEqual(model['components'],[])
        self.assertEqual(metadata['variables'][0]['name'],'auxiliary')
        with source.with_name('output.csv').open(newline='') as stream:
            expected=list(csv.DictReader(stream))
        actual=self.run_ir(model)
        self.assertEqual(len(actual),len(expected))
        for a,b in zip(actual,expected):
            self.assertEqual(float(a['time']),float(b['Time']))
            self.assertEqual(float(a['value']),float(b['auxiliary']))

    def test_stock_free_auxiliaries_and_state(self):
        source=document('<aux name="a"><eqn>b+TIME</eqn></aux><aux name="b"><eqn>4-5+6</eqn></aux>')
        model,_=self.convert_text(source)
        self.assertEqual([float(r['value']) for r in self.run_ir(model)],[5,5,6,5,7,5])
        source=document('<aux name="a"><eqn>DELAY(TIME,1,-1)</eqn></aux>')
        model,_=self.convert_text(source)
        self.assertEqual([float(r['value']) for r in self.run_ir(model)],[-1,0,1])
        with self.assertRaisesRegex(Unsupported,'cyclic'):
            self.convert_text(document('<aux name="a"><eqn>b</eqn></aux><aux name="b"><eqn>a</eqn></aux>'))

    def test_constant_initialization_and_shared_stock_state(self):
        model, _ = self.convert_text(document())
        self.assertEqual([float(r['value']) for r in self.run_ir(model)],[10,5,2.5])
        variables = '<stock name="a"><eqn>8</eqn><outflow>transfer</outflow></stock><stock name="b"><eqn>2</eqn><inflow>transfer</inflow></stock><flow name="transfer"><eqn>(a-b)/2</eqn></flow>'
        model, _ = self.convert_text(document(variables))
        self.assertEqual([float(r['value']) for r in self.run_ir(model)],[8,2,5,5,5,5])

    def test_alias_units_and_dynamic_auxiliaries(self):
        variables = '<stock name="Tank Level"><eqn>10</eqn><outflow>Drain</outflow><units>litre</units></stock><flow name="Drain"><eqn>helper</eqn><units>litre/hour</units></flow><aux name="helper"><eqn>tank_level/2</eqn></aux>'
        model, metadata = self.convert_text(document(variables))
        self.assertEqual([float(r['value']) for r in self.run_ir(model)],[10,5,2.5])
        self.assertEqual(metadata['variables'][0]['name'],'Tank Level')
        self.assertEqual(metadata['variables'][0]['unit'],'litre')
        model, _ = self.convert_text(document(variables.replace('tank_level','"TANK LEVEL"')))
        self.assertEqual([float(r['value']) for r in self.run_ir(model)],[10,5,2.5])

    def test_rejections(self):
        source = document()
        mutations = [
            (source.replace('<start>0','<start>2'), 'positive horizon'),
            (source.replace('<dt>1','<dt>0'), 'positive'),
            (source.replace('<dt>1','<dt>0.3'), 'whole ticks'),
            (source.replace('<dt>1','<dt>1e-320'), 'whole ticks'),
            (source.replace('<sim_specs>','<sim_specs method="RK45">'), 'Euler'),
            (source.replace('<dt>','<dt reciprocal="yes">'), 'reciprocal'),
            (source.replace('<eqn>5','<eqn>tau').replace('<eqn>2</eqn>','<eqn>a</eqn>'), 'cyclic'),
            (source.replace('<eqn>2*a','<eqn>s'), 'cyclic initialization'),
            (source.replace('name="tau"','name="A"'), 'colliding'),
            (source.replace('<eqn>5','<eqn>SIN(1)'), 'unsupported function'),
            (source.replace('<eqn>5','<eqn>2**3'), 'unsupported equation syntax'),
            (source.replace('<eqn>5','<eqn>5//2'), 'unsupported equation syntax'),
            (source.replace('<eqn>5','<eqn>missing'), 'unknown symbol'),
            (source.replace('<eqn>5','<eqn>1/0'), 'division by zero'),
            (source.replace('<eqn>5','<eqn>1e309'), 'finite'),
            (source.replace('<outflow>f</outflow>',''), 'unconnected flow'),
            (source.replace('<outflow>f</outflow>','<outflow>f</outflow>'*2), 'multiply connected'),
            (source.replace('<outflow>f','<outflow>s'), 'unknown'),
            (source.replace('<eqn>5</eqn>','<eqn>5</eqn><gf/>'), 'exactly one ypts'),
            (source.replace('<stock name="s">','<stock name="s"><non_negative/>'), 'non_negative'),
            (source.replace('<eqn>5</eqn>','<eqn>5</eqn><eqn>6</eqn>'), 'exactly one eqn'),
            (source.replace('<eqn>5</eqn>','<eqn type="array">5</eqn>'), 'attributes'),
            (source.replace('<model>','<dimensions><dim name="x"/></dimensions><model>'), 'dimensions'),
            ('<!DOCTYPE xmile [<!ENTITY x "1">]>'+source, 'document/entity'),
        ]
        for text, diagnostic in mutations:
            with self.subTest(diagnostic=diagnostic), self.assertRaisesRegex(Unsupported,diagnostic):
                self.convert_text(text)
        with self.assertRaisesRegex(Unsupported,'units=metadata'):
            convert(self.folder/'input.xmile',units='strict')

    def test_imported_flows_are_signed(self):
        model, metadata = self.convert_text(document().replace('<eqn>s/tau','<eqn>-s/tau'))
        self.assertEqual(metadata['flow_policy'],'signed')
        self.assertEqual([float(row['value']) for row in self.run_ir(model)], [10, 15, 22.5])
        for component in model['components']:
            if component['kind'] == 'flow':
                del component['non_negative']
        self.run_ir(model,success=False)

    def test_cli_metadata_and_no_overwrite(self):
        source = self.folder/'source.xmile'
        source.write_text(document())
        output = self.folder/'result.json'
        command = [sys.executable,str(ROOT/'tools/xmile2ir.py'),str(source),'--out',str(output),'--units','metadata']
        result = subprocess.run(command,text=True,capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr)
        sidecar = output.with_suffix('.json.metadata.json')
        self.assertEqual(json.loads(sidecar.read_text())['source_sha256'],hashlib.sha256(source.read_bytes()).hexdigest())
        saved = output.read_bytes(), sidecar.read_bytes()
        self.assertNotEqual(subprocess.run(command,capture_output=True).returncode,0)
        self.assertEqual(saved,(output.read_bytes(),sidecar.read_bytes()))
        source.write_text(document().replace('<eqn>5','<eqn>unsupported(5)'))
        other = self.folder/'rejected.json'
        command[command.index('--out')+1] = str(other)
        self.assertNotEqual(subprocess.run(command,capture_output=True).returncode,0)
        self.assertFalse(other.exists())
        self.assertFalse(other.with_suffix('.json.metadata.json').exists())


if __name__ == '__main__':
    unittest.main()
