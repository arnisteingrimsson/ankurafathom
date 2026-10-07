"""Create a small independently calculable model package for API/UI integration."""
import argparse
import json
from pathlib import Path


def create(destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    parameters = [dict(id=name, value=value, unit=unit) for name, value, unit in [
        ('demand_hours', 100, 'hours'), ('paid_hours', 160, 'hours'), ('productivity', 0, '1'),
        ('fixed_fee', 0, '1'), ('rate', 200, 'USD/hours'), ('payroll', 10000, 'USD'),
        ('variable_cost', 10, 'USD/hours'), ('license', 40, 'USD'), ('zero_usd', 0, 'USD'), ('one_month', 1, 'month')]]
    auxiliaries = [dict(id=name, kind='aux', expr=expr, unit=unit) for name, expr, unit in [
        ('work', 'MIN(demand_hours,paid_hours/(1-productivity))', 'hours'),
        ('hours', 'work*(1-productivity)', 'hours'),
        ('revenue', '(fixed_fee*work+(1-fixed_fee)*hours)*rate', 'USD'),
        ('cost', 'payroll+hours*variable_cost+IF_POSITIVE(productivity,license,zero_usd)', 'USD')]]
    components, outputs, metrics = list(auxiliaries), [], []
    for name, expr, unit in [('revenue', 'revenue', 'USD'), ('cost', 'cost', 'USD'),
                             ('profit', 'revenue-cost', 'USD'), ('hours', 'hours', 'hours'), ('capacity', 'paid_hours', 'hours')]:
        key = 'cum_'+name
        components += [dict(id=key, kind='stock', init=0, unit=unit, non_negative=name != 'profit'),
                       dict(id='flow_'+name, kind='flow', source=None, destination=key, expr='('+expr+')/one_month',
                            unit=unit+'/month', non_negative=name != 'profit')]
        outputs.append(dict(id=key, stock=key))
        metrics.append(dict(id=name, label=name.title(), definition='Cumulative '+name+' difference over the selected interval.',
            unit=unit, format='currency' if unit == 'USD' else 'number', dimensions={'practice': 'demo'},
            output=key, aggregation='delta', interpolation='hold'))
    metrics += [dict(id=name, label=label, definition=definition, unit='1', format='percent', dimensions={'practice': 'demo'},
                     aggregation='ratio', numerator=numerator, denominator=denominator)
                for name, label, definition, numerator, denominator in [
                    ('margin', 'Operating contribution margin', 'Selected operating contribution divided by selected revenue; excludes tax and financing.', 'profit', 'revenue'),
                    ('utilization', 'Utilization', 'Actual delivery hours divided by paid capacity hours over the selected interval.', 'hours', 'capacity')]]
    model = dict(ir_version='0.1', name='Application integration economics fixture', mode='sd',
        time=dict(unit='month', dt=1, horizon=12), parameters=parameters, components=components, outputs=outputs,
        checks=[dict(id='accounting', kind='assert', expr='cum_profit == cum_revenue - cum_cost', absolute_tolerance=1e-8, relative_tolerance=1e-12)])
    descriptor = dict(schema_version='1.0', id='economics_demo', label='Economics API demonstration',
        description='Synthetic one-person fixture for UI integration; not an Ankura forecast.',
        parameters=[dict(id=name, label=label, description=description, kind=kind, minimum=low, maximum=high,
                         status='synthetic', source='application/examples/make_demo.py: hand-calculable fixture')
                    for name, label, description, kind, low, high in [
                        ('demand_hours', 'Monthly demand', 'Incoming work in baseline hours.', 'number', 0, 300),
                        ('productivity', 'Delivery-hour reduction', 'Fraction of delivery hours saved on each unit of work.', 'number', 0, .5),
                        ('fixed_fee', 'Fixed-fee pricing', '0 bills actual hours; 1 bills baseline contracted work.', 'boolean', 0, 1)]], metrics=metrics)
    for name, value in [('model.json', model), ('descriptor.json', descriptor),
                        ('registry.json', dict(models=[dict(model='model.json', descriptor='descriptor.json')]))]:
        (destination/name).write_text(json.dumps(value, indent=2)+'\n')
    return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True)
    create(parser.parse_args().out)
