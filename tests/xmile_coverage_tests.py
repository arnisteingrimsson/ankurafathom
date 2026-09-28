"""Offline inventory integrity and non-circular eligibility checks."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
ORACLE=ROOT/'tests/oracles/xmile'
sys.path.insert(0,str(ORACLE))
import audit


def source(equation='4-5+6',method='Euler',start=0,extra=''):
    return f'<xmile xmlns="http://www.systemdynamics.org/XMILE" version="1.0"><sim_specs method="{method}"><start>{start}</start><stop>1</stop><dt>1</dt></sim_specs><model><variables><aux name="a"><eqn>{equation}</eqn>{extra}</aux></variables></model></xmile>'


class CoverageTests(unittest.TestCase):
    def test_reproducible_full_inventory(self):
        functions=set(audit.convert.__globals__['STATE_FUNCTIONS'])|set(audit.convert.__globals__['INPUT_FUNCTIONS'])
        self.assertEqual(set(audit.POLICY['functions']),{name.upper() for name in functions})
        stored=json.loads((ORACLE/'coverage.json').read_text())
        self.assertEqual(audit.build_report(ORACLE/'corpus'),stored)
        self.assertEqual(stored['eligibility_counts'],dict(eligible=8,outside_equation_subset=10,unassessable=49))
        self.assertEqual(stored['eligible_coverage'],dict(imported=8,total=8,percent=100.0,
            directory_groups_imported=6,directory_groups_total=6,structural_target_met=True,
            historical_reference_compatible=7,historical_reference_percent=87.5))
        gaps={c['path'] for c in stored['cases'] if c['eligibility']=='eligible' and c['status']=='rejected'}
        self.assertEqual(gaps,set())
        conflicts=[c for c in stored['cases'] if 'reference_conflict' in c]
        self.assertEqual([c['path'] for c in conflicts],['tests/zeroled_decimals/test_zeroled_decimals.xmile'])
        # Every accepted file has a trajectory test and vendored reference(s).
        references={
            'tests/delay_xmile/test_delay_xmile.xmile':['tests/delay_xmile/output.tab'],
            'samples/teacup/teacup_w_diagram.xmile':['samples/teacup/output.csv','samples/teacup/output_stella1006.csv'],
            'samples/teacup/teacup.xmile':['samples/teacup/output.csv','samples/teacup/output_stella1006.csv'],
            'samples/SIR/SIR.xmile':['samples/SIR/output.csv','samples/SIR/output_stella1006.csv'],
            'samples/SIR/SIR_reciprocal-dt.xmile':['samples/SIR/output.csv','samples/SIR/output_stella1006.csv'],
            'tests/lookups/test_lookups_no-indirect.xmile':['tests/lookups/output.tab','tests/lookups/output_stella1006.csv'],
            'tests/eval_order/eval_order.xmile':['tests/eval_order/output.csv'],
            'tests/zeroled_decimals/test_zeroled_decimals.xmile':['tests/zeroled_decimals/output.tab']}
        self.assertEqual(set(references),{c['path'] for c in stored['cases'] if c['status']=='importable'})
        manifest=json.loads((ORACLE/'manifest.json').read_text())
        for names in references.values():
            for name in names:
                self.assertEqual(hashlib.sha256((ORACLE/'corpus'/name).read_bytes()).hexdigest(),manifest['files'][name])

    def test_importer_regressions_do_not_shrink_denominator(self):
        with patch.object(audit,'convert',side_effect=audit.Unsupported('injected importer failure')):
            report=audit.build_report(ORACLE/'corpus')
        self.assertEqual(report['eligibility_counts'],dict(eligible=8,outside_equation_subset=10,unassessable=49))
        self.assertEqual(report['eligible_coverage']['total'],8)
        self.assertEqual(report['eligible_coverage']['imported'],0)
        self.assertEqual(report['eligible_coverage']['historical_reference_compatible'],0)
        self.assertFalse(report['eligible_coverage']['structural_target_met'])

    def test_structural_gaps_remain_in_denominator(self):
        for text in (source(method='RK4'),source(start=1),source(extra='<non_negative/>'),
                     source().replace('version="1.0"','version="0.9"'),
                     source().replace('<model>','<dimensions><dim name="x"/></dimensions><model>')):
            with self.subTest(text=text):
                self.assertEqual(audit.assess(text)['eligibility'],'eligible')
        self.assertEqual(audit.assess('<xmile><broken>')['eligibility'],'unassessable')
        for eq in ('SIN(1)','2^3','a[1]','TIME &lt; 2','IF TIME THEN 1 ELSE 2'):
            self.assertEqual(audit.assess(source(eq))['eligibility'],'outside_equation_subset')
        self.assertEqual(audit.assess(source('SMTHN(2,3,2)'))['eligibility'],'eligible')
        self.assertEqual(audit.assess(source('"IF"+"SIN(1)"'))['eligibility'],'eligible')

    def test_inventory_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(ValueError,'inventory differs'):
                audit.build_report(Path(folder))
        manifest=json.loads((ORACLE/'manifest.json').read_text())
        changed=copy.deepcopy(manifest)
        changed['files']['samples/SIR/SIR.xmile']='0'*64
        original=Path.read_text
        def altered(path,*args,**kwargs):
            return json.dumps(changed) if path==ORACLE/'manifest.json' else original(path,*args,**kwargs)
        with patch.object(Path,'read_text',altered),self.assertRaisesRegex(ValueError,'source hash mismatch'):
            audit.build_report(ORACLE/'corpus')


if __name__=='__main__':
    unittest.main()
