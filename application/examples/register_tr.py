"""Create a reviewed-subset API descriptor for an existing generated T&R model.

Does not modify or regenerate the other agent's model. Registries reference that
model; service startup captures its exact bytes and referenced data files.
"""
import argparse
import json
from pathlib import Path


def create(model_path, destination):
    model_path = Path(model_path).resolve()
    model = json.loads(model_path.read_text())
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    source = 'Synthetic T&R scenario controls; defaults resolved from the captured parameter table. Sponsor approval outstanding.'
    controls = [
        ('ai', 'AI task savings', 'Savings at full adoption for exposed task hours, before rework; applied through the adoption calendar.', 'number', 0, 1),
        ('erosion', 'New-contract price erosion', 'Scenario price pass-through control; existing contract values remain governed by the model.', 'number', 0, 1),
        ('bd', 'Business-development allocation', 'Scenario control enabling surplus eligible senior time for business development.', 'number', 0, 1),
        ('macro', 'Market-shock exposure', 'Scenario weight on the bound market-shock calendar.', 'number', 0, 1),
        ('fixed_shift', 'Eligible fixed-fee conversion', 'Conversion control for eligible new work, subject to model constraints.', 'number', 0, 1),
        ('demand_scale', 'Demand multiplier', 'Scale applied to modeled new prospects; 0..3 is the API exploration domain.', 'number', 0, 3),
        ('success_gate', 'Success-fee realization control', 'Multiplier on modeled expected contingent success.', 'number', 0, 1),
        ('policy_hold', 'Replace exits immediately', '1 selects the hold policy; use 0 with responsive=0 for freeze.', 'boolean', 0, 1),
        ('policy_responsive', 'Responsive hiring', '1 selects responsive hiring; cannot coexist with hold.', 'boolean', 0, 1)]
    parameters = {p['id'] for p in model['parameters']}
    if not all(name in parameters for name, *_ in controls):
        raise ValueError('This adapter requires the current T&R scenario-control contract')
    metrics = []
    outputs = {o['id'] for o in model['outputs']}
    for name, label, definition, unit in [
        ('revenue', 'Recognized revenue', 'Earned fees less modeled disallowance.', 'USD'),
        ('ebitda', 'Modeled practice EBITDA', 'Revenue less modeled compensation, overhead and other modeled costs.', 'USD'),
        ('cost', 'Operating costs', 'All costs included by the captured T&R model.', 'USD'),
        ('collections', 'Collections', 'Modeled cash collections during the selected interval.', 'USD'),
        ('cash_flow', 'Operating cash contribution', 'Collections less modeled costs; not a complete enterprise free-cash-flow measure.', 'USD'),
        ('delivery_hours', 'Delivery hours', 'Total delivered task hours across billable levels.', 'hours')]:
        output = 'cum_'+name
        if output not in outputs:
            raise ValueError('Required T&R output missing: '+output)
        metrics.append(dict(id=name, label=label, definition=definition, unit=unit,
            format='currency' if unit == 'USD' else 'number', dimensions={'practice': 'TR'},
            aggregation='delta', interpolation='hold', output=output))
    # Derive billable capacity from billable levels only: cum_paid also includes
    # support and would change the utilization denominator. Names come from
    # the companion saved config when available; do not invent level identities.
    companion = model_path.parent.parent/'config.json'
    config = json.loads(companion.read_text()) if companion.is_file() else None
    if config:
        billable, workforce = [], []
        literals = {p['id']: p['value'] for p in model['parameters']}
        for index, level in enumerate(config['levels']):
            if literals.get(f'l{index}_hours_per_job') != level['hours_per_job']:
                raise ValueError('Companion config task classification differs from generated model')
            headcount, paid = f'l{index}_fte', f'm_paid_l{index}'
            if headcount not in outputs or paid not in outputs:
                raise ValueError('T&R level outputs do not match companion config')
            dims = {'practice': 'TR', 'level': level['id']}
            key = 'headcount_'+level['id']
            workforce.append(key)
            metrics.append(dict(id=key, label=level['id'].replace('_', ' ').title()+' FTE',
                definition='Expected full-time equivalents at the selected endpoint.', unit='people', format='number',
                dimensions=dims, aggregation='last', interpolation='hold', output=headcount))
            if level['hours_per_job'] > 0:
                key = 'paid_'+level['id']
                billable.append(key)
                metrics.append(dict(id=key, label=level['id'].replace('_', ' ').title()+' paid hours',
                    definition='Monthly paid hours summed over (from,to].', unit='hours', format='number',
                    dimensions=dims, aggregation='sum', interpolation='hold', output=paid))
        metrics += [dict(id=key, label=label, definition=definition, unit=unit, format='number', dimensions={'practice': 'TR'},
                         aggregation='sum_metrics', members=members)
                    for key, label, definition, unit, members in [
                        ('headcount', 'Total expected FTE', 'Sum of endpoint billable and support expected FTE.', 'people', workforce),
                        ('billable_paid', 'Billable paid hours', 'Sum of paid hours for configured billable levels; excludes support.', 'hours', billable)]]
        metrics.append(dict(id='utilization', label='Delivery utilization', definition='Delivery hours divided by billable-level paid hours, including paid non-delivery time in the denominator.',
            unit='1', format='percent', dimensions={'practice': 'TR'}, aggregation='ratio', numerator='delivery_hours', denominator='billable_paid'))
    metrics.append(dict(id='margin', label='Modeled EBITDA margin', definition='Selected modeled EBITDA divided by selected recognized revenue.',
        unit='1', format='percent', dimensions={'practice': 'TR'}, aggregation='ratio', numerator='ebitda', denominator='revenue'))
    descriptor = dict(schema_version='1.0', id='ankura_tr', label='Ankura T&R synthetic pilot',
        description='Current bounded monthly T&R model. Synthetic scenario assumptions; expected FTE and fluid delivery. Review the pilot specification before interpreting forecasts.',
        parameters=[dict(id=name, label=label, description=description, kind=kind, minimum=low, maximum=high, status='synthetic', source=source)
                    for name, label, description, kind, low, high in controls], metrics=metrics,
        constraints=[dict(kind='sum_lte', parameters=['policy_hold', 'policy_responsive'], value=1,
                          message='Choose hold, freeze, or responsive hiring; hold and responsive cannot both be enabled.')])
    (destination/'descriptor.json').write_text(json.dumps(descriptor, indent=2)+'\n')
    (destination/'registry.json').write_text(json.dumps(dict(models=[dict(model=str(model_path), descriptor='descriptor.json')]), indent=2)+'\n')
    return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    create(args.model, args.out)
