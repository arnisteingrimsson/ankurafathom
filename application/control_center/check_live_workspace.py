"""Optional live smoke check; requires console and Equinix services running."""
import argparse
import copy
import json
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import ProxyHandler, Request, build_opener


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8086)
    args = parser.parse_args()
    client = build_opener(ProxyHandler({}))
    base = f'http://127.0.0.1:{args.port}/api/projects/equinix-ai/workspace'

    def call(path='', body=None):
        request = Request(base + path, data=None if body is None else json.dumps(body).encode(),
                          headers={'Content-Type': 'application/json'})
        with client.open(request, timeout=30) as response:
            return json.load(response)

    definition = call()['current']
    session = call('/start', {'revision': definition['revision']})
    run = '/runs/' + session['id']
    report = dict(session=session['id'], definition_revision=definition['revision'],
                  config_sha256=session['config_sha256'], scope='Live API smoke check with synthetic observations; not business calibration.')
    try:
        assert session['frame']['time'] == 0
        assert session['config_sha256'] == definition['config_sha256']
        frame = call(run + '/step', {'expected_revision': session['revision'], 'days': 1})['frame']
        assert frame['time'] == 1 and frame['checks_passed']
        fixture = dict(source='Synthetic API smoke fixture derived from computed state', kind='synthetic',
                       rows=[dict(time=1, metric='active', unit='customer', actual=frame['metrics']['active'], tolerance=0)])
        positive = call(run + '/evidence', fixture)
        assert positive['passed']
        negative = copy.deepcopy(fixture)
        negative['source'] = 'Synthetic negative control: deliberately incorrect active-customer count'
        negative['rows'][0]['actual'] += 1000
        rejected = call(run + '/evidence', negative)
        assert not rejected['passed']
        invalid = copy.deepcopy(fixture)
        invalid['rows'][0]['unit'] = 'USD'
        try:
            call(run + '/evidence', invalid)
            raise AssertionError('Invalid unit accepted')
        except HTTPError as error:
            assert error.code == 422
            error.close()
        report.update(passed=True, initial_time=0, computed_through=1,
                      native_checks=len(frame['checks']), positive_comparison=positive,
                      negative_control=rejected, invalid_unit_rejected=True)
    finally:
        call(run + '/stop', {})
    path = Path(__file__).resolve().parents[2] / 'artifacts/visual-workspace-acceptance/live-api.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k:v for k,v in report.items() if k not in ('positive_comparison','negative_control')}, indent=2))


if __name__ == '__main__':
    main()
