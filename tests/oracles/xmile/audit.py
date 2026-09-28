"""Reproducible corpus inventory; eligibility is independent of importer success."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT/'tools'))
from xmile2ir import convert, Unsupported

# Reviewed source-language policy, deliberately independent of convert().
# Structural limitations must never shrink the supported-equation denominator.
POLICY = {
    'version': 2,
    'method': 'reviewed lexical inventory of the pinned corpus; not a general XMILE validator',
    'functions': ['DELAY', 'DELAY1', 'DELAY3', 'DELAYN', 'PULSE', 'RAMP', 'SMTH1', 'SMTH3', 'SMTHN', 'STEP'],
    'operators': ['+', '-', '*', '/'],
    'lookup_tables': True,
    'structural_gaps_remain_eligible': True,
    'malformed_xml': 'unassessable, retained in total inventory',
    'target_percent': 80,
}
NAMESPACES = {'http://docs.oasis-open.org/xmile/ns/XMILE/v1.0',
              'http://www.systemdynamics.org/XMILE'}
REFERENCE_CONFLICTS = {
    'tests/zeroled_decimals/test_zeroled_decimals.xmile':
        'XMILE declares RK4; historical output.tab follows Euler for the time-dependent stock. '
        'RK4 is checked against a closed form; the full historical trajectory is not a source-faithful pass.'
}


def local(tag):
    return tag.rsplit('}', 1)[-1]


def assess(raw):
    try:
        tree = ET.fromstring(raw)
    except ET.ParseError as error:
        return dict(eligibility='unassessable', xml_error=str(error))
    functions, unsupported, features = set(), set(), set()
    tables = {e.get('name', '').casefold() for e in tree.iter() if local(e.tag)=='gf'}
    for node in tree.iter():
        tag = local(node.tag)
        if tag == 'eqn':
            # Quoted variable names must not be interpreted as syntax/functions.
            text = re.sub(r'"[^"\n]*"', 'symbol', node.text or '')
            for name in re.findall(r'\b([A-Za-z_][A-Za-z_0-9]*)\s*\(', text):
                if name.casefold() in tables:
                    functions.add('LOOKUP_TABLE')
                else:
                    functions.add(name.upper())
                    if name.upper() not in POLICY['functions']:
                        unsupported.add('function:'+name.upper())
            for word in re.findall(r'\b(?:IF|THEN|ELSE|AND|OR|NOT|MOD)\b', text, re.I):
                unsupported.add('syntax:'+word.upper())
            for symbol, name in [('^','power'), ('[','subscript'), ('<','comparison'),
                                 ('>','comparison'), ('=','comparison'), ('{','placeholder')]:
                if symbol in text:
                    unsupported.add('syntax:'+name)
        elif tag == 'gf':
            functions.add('LOOKUP_TABLE')
        elif tag == 'non_negative':
            features.add('nonnegative_clipping')
        elif tag == 'dimensions' and (len(node) or node.attrib):
            features.add('arrays')
        elif tag == 'sim_specs':
            method = node.get('method', 'Euler').upper()
            if method != 'EULER':
                features.add('integrator:'+method)
        elif tag == 'start':
            if float(node.text or 'nan') != 0:
                features.add('nonzero_start')
        if node.tag.startswith('{http://iseesystems.com/XMILE}') or any(
                key.startswith('{http://iseesystems.com/XMILE}') for key in node.attrib):
            features.add('vendor_extensions')
    if not any(local(e.tag)=='stock' for e in tree.iter()):
        features.add('no_stocks')
    if sum(local(e.tag)=='model' for e in tree.iter()) != 1:
        features.add('multiple_models')
    namespace = tree.tag[1:].split('}')[0] if tree.tag.startswith('{') else ''
    if namespace not in NAMESPACES or tree.get('version')!='1.0':
        features.add('source_dialect')
    return dict(eligibility='outside_equation_subset' if unsupported else 'eligible',
                functions=sorted(functions), unsupported_equations=sorted(unsupported),
                structural_features=sorted(features))


def build_report(checkout):
    manifest = json.loads((HERE/'manifest.json').read_text())
    expected = {name: digest for name, digest in manifest['files'].items() if name.endswith('.xmile')}
    paths = sorted(checkout.rglob('*.xmile'))
    if {str(path.relative_to(checkout)) for path in paths} != set(expected):
        raise ValueError('corpus file inventory differs from manifest')
    cases = []
    for path in paths:
        relative = str(path.relative_to(checkout))
        raw = path.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if digest != expected[relative]:
            raise ValueError('corpus source hash mismatch: '+relative)
        case = dict(path=relative, sha256=digest, group=str(path.relative_to(checkout).parent), **assess(raw))
        if relative in REFERENCE_CONFLICTS:
            case['reference_conflict'] = REFERENCE_CONFLICTS[relative]
        try:
            convert(path, units='metadata')
            case['status'] = 'importable'
        except Unsupported as exc:
            case.update(status='rejected', reason=str(exc))
        cases.append(case)
    eligible = [c for c in cases if c['eligibility']=='eligible']
    imported = [c for c in eligible if c['status']=='importable']
    groups = {c['group'] for c in eligible}
    # A directory group is covered only if all its eligible variants import.
    covered_groups = {g for g in groups if all(c['status']=='importable' for c in eligible if c['group']==g)}
    numerator, denominator = len(imported), len(eligible)
    historical_compatible = sum('reference_conflict' not in c for c in imported)
    return dict(commit=manifest['commit'],
                importer_sha256=hashlib.sha256((ROOT/'tools/xmile2ir.py').read_bytes()).hexdigest(),
                audit_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                eligibility_policy=POLICY, total_xmile_files=len(cases),
                counts=dict(Counter(c['status'] for c in cases)),
                eligibility_counts=dict(Counter(c['eligibility'] for c in cases)),
                eligible_coverage=dict(imported=numerator, total=denominator,
                    percent=100*numerator/denominator if denominator else None,
                    directory_groups_imported=len(covered_groups), directory_groups_total=len(groups),
                    historical_reference_compatible=historical_compatible,
                    historical_reference_percent=100*historical_compatible/denominator if denominator else None,
                    structural_target_met=bool(denominator and 100*numerator>=POLICY['target_percent']*denominator)),
                scope='Import coverage only; trajectory tests independently validate accepted cases. Malformed XML remains unassessable. No M2 completion claim.',
                cases=cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkout', type=Path, nargs='?', default=HERE/'corpus')
    parser.add_argument('--out', type=Path, default=HERE/'coverage.json')
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    # An upstream checkout is optional; vendored bytes suffice for offline CI.
    if (args.checkout/'.git').exists():
        commit = subprocess.check_output(['git','-C',str(args.checkout),'rev-parse','HEAD'],text=True).strip()
        manifest = json.loads((HERE/'manifest.json').read_text())
        if commit != manifest['commit'] or subprocess.check_output(
                ['git','-C',str(args.checkout),'status','--porcelain'],text=True).strip():
            parser.error('checkout must be clean and match the pinned commit')
    report = build_report(args.checkout)
    if args.verify:
        if json.loads(args.out.read_text()) != report:
            raise ValueError('stored corpus audit is stale')
    else:
        args.out.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({key:report[key] for key in ('counts','eligibility_counts','eligible_coverage')}))


if __name__ == '__main__':
    main()
