"""Deterministic post-processing of verified results, never chart-decimated data."""
import math
from .contracts import fields, integer, number, require


class Results:
    def __init__(self, description, rows):
        self.description = description
        self.metrics = {m['id']: m for m in description['metrics']}
        self.series = {}
        for row in rows:
            key = (int(row['scenario']), int(row['replication']), row['output_id'])
            time, value = float(row['time']), float(row['value'])
            require(number(time) and number(value), 'Nonfinite result', 'RESULT_CORRUPT', 409)
            self.series.setdefault(key, []).append((time, value))
        for samples in self.series.values():
            samples.sort()
            require(all(a[0] < b[0] for a, b in zip(samples, samples[1:])), 'Duplicate observation time', 'RESULT_CORRUPT', 409)

    def raw(self, metric, scenario, replication):
        require(metric in self.metrics, 'Unknown metric: '+metric)
        definition = self.metrics[metric]
        require('output' in definition, 'Derived metrics are available through summary/compare, not raw playback')
        key = (scenario, replication, definition['output'])
        require(key in self.series, 'Metric/scenario/replication has no observations', 'NO_OBSERVATIONS', 404)
        return self.series[key]

    def window(self, metric, scenario, replication, start, end):
        values = self.raw(metric, scenario, replication)
        require(number(start) and number(end) and start <= end, 'Invalid time window')
        def locate(t):
            matches = [i for i, (time, _) in enumerate(values) if math.isclose(time, t, rel_tol=1e-12, abs_tol=1e-12)]
            require(len(matches) == 1, 'Summary endpoints must identify unique recorded sample times')
            return matches[0]
        left, right = locate(start), locate(end)
        require(left <= right, 'Reversed time window')
        return values[left:right+1]

    def value(self, metric, scenario, replication, start, end):
        require(metric in self.metrics, 'Unknown metric: '+metric)
        spec = self.metrics[metric]
        kind = spec['aggregation']
        reason = None
        if kind in ('ratio', 'sum_metrics'):
            refs = [spec['numerator'], spec['denominator']] if kind == 'ratio' else spec['members']
            parts = [self.value(key, scenario, replication, start, end) for key in refs]
            numbers = [p['value'] for p in parts]
            if any(v is None for v in numbers):
                value, reason = None, 'undefined_dependency'
            elif kind == 'ratio':
                value = numbers[0]/numbers[1] if numbers[1] != 0 else None
                if value is None:
                    reason = 'zero_denominator'
            else:
                value = math.fsum(numbers)
        else:
            samples = self.window(metric, scenario, replication, start, end)
            if kind == 'last':
                value = samples[-1][1]
            elif kind == 'sum':
                # Period amounts are timestamped at their period's END. Exclude
                # the left boundary (including the initial-state placeholder).
                value = math.fsum(v for _, v in samples[1:])
            elif kind == 'delta':
                value = samples[-1][1]-samples[0][1]
            else:
                amount = math.fsum((b[0]-a[0])*(a[1] if spec['interpolation'] == 'hold' else (a[1]+b[1])/2)
                                   for a, b in zip(samples, samples[1:]))
                if kind == 'integral':
                    value = amount
                else:
                    duration = samples[-1][0]-samples[0][0]
                    value = amount/duration if duration else None
                    if value is None:
                        reason = 'zero_duration'
        require(value is None or number(value), 'Derived metric overflow', 'METRIC_ARITHMETIC')
        unit = spec['unit']
        if kind == 'integral':
            unit = '('+unit+')*'+self.description['time']['unit']
        return dict(metric=metric, value=value, unit=unit, aggregation=kind, reason=reason,
                    dimensions=spec['dimensions'], interval=dict(start=start, end=end))

    def summary(self, request):
        fields(request, ('metrics', 'scenario', 'replication', 'from', 'to', 'thresholds'), ('metrics', 'from', 'to'))
        scenario, replication = selection(request)
        require(isinstance(request['metrics'], list) and request['metrics'] and all(isinstance(m, str) for m in request['metrics'])
                and len(set(request['metrics'])) == len(request['metrics']), 'metrics must be unique IDs')
        values = [self.value(m, scenario, replication, request['from'], request['to']) for m in request['metrics']]
        thresholds = request.get('thresholds', [])
        require(isinstance(thresholds, list), 'thresholds must be an array')
        checks = []
        for rule in thresholds:
            fields(rule, ('metric', 'operator', 'value'), ('metric', 'operator', 'value'))
            require(rule['operator'] in ('gte', 'lte') and number(rule['value']), 'Invalid threshold')
            value = self.value(rule['metric'], scenario, replication, request['from'], request['to'])['value']
            passed = None if value is None else value >= rule['value'] if rule['operator'] == 'gte' else value <= rule['value']
            checks.append(dict(rule, observed=value, passed=passed))
        return dict(scenario=scenario, replication=replication, values=values, thresholds=checks,
                    calculation_version='1.0', data_resolution='full')


def selection(request):
    scenario, replication = request.get('scenario', 0), request.get('replication', 0)
    require(integer(scenario, 0, 65535) and integer(replication, 0, 65535), 'Invalid scenario/replication')
    return scenario, replication


def compare(baseline, candidate, request):
    fields(request, ('baseline_run', 'candidate_run', 'baseline_scenario', 'candidate_scenario', 'replication',
                     'metrics', 'from', 'to', 'valuation'),
           ('baseline_run', 'candidate_run', 'metrics', 'from', 'to'))
    require(isinstance(request['metrics'], list) and request['metrics'] and all(isinstance(m, str) for m in request['metrics']), 'metrics must be IDs')
    b, rep = selection(dict(scenario=request.get('baseline_scenario', 0), replication=request.get('replication', 0)))
    c, _ = selection(dict(scenario=request.get('candidate_scenario', 0), replication=rep))
    start, end = request['from'], request['to']
    changes = []
    for metric in request['metrics']:
        left = baseline.value(metric, b, rep, start, end)
        right = candidate.value(metric, c, rep, start, end)
        x, y = left['value'], right['value']
        delta = None if x is None or y is None else y-x
        relative = None if delta is None or x == 0 else 100*delta/abs(x)
        require(delta is None or number(delta), 'Comparison overflow')
        require(relative is None or number(relative), 'Percentage overflow')
        changes.append(dict(metric=metric, baseline=x, candidate=y, delta=delta, percent_change=relative,
                            percent_convention='100*(candidate-baseline)/abs(baseline)',
                            reason='undefined_metric' if delta is None else 'zero_baseline' if x == 0 else None,
                            unit=left['unit'], dimensions=left['dimensions']))
    result = dict(values=changes, calculation_version='1.0', interval=dict(start=start, end=end), replication=rep,
                  uncertainty='Single selected replication; no confidence interval is implied')
    if 'valuation' in request:
        v = request['valuation']
        fields(v, ('metric', 'annual_discount_rate', 'time_units_per_year', 'initial_incremental_cash_flow'),
               ('metric', 'annual_discount_rate', 'time_units_per_year', 'initial_incremental_cash_flow'))
        metric = v['metric']
        require(metric in baseline.metrics, 'Unknown cash-flow metric')
        kind = baseline.metrics[metric]['aggregation']
        require(kind in ('sum', 'delta'), 'Valuation requires period amounts or a cumulative cash-flow metric')
        rate, scale, initial = v['annual_discount_rate'], v['time_units_per_year'], v['initial_incremental_cash_flow']
        require(number(rate) and rate > -1 and number(scale) and scale > 0 and number(initial), 'Invalid valuation conventions')
        xs, ys = baseline.window(metric, b, rep, start, end), candidate.window(metric, c, rep, start, end)
        require([t for t, _ in xs] == [t for t, _ in ys], 'Valuation requires aligned observation grids')
        flows = []
        for index in range(1, len(xs)):
            flow = ys[index][1]-xs[index][1]
            if kind == 'delta':
                flow -= ys[index-1][1]-xs[index-1][1]
            flows.append((xs[index][0], flow))
        discounted, balance = [initial], initial
        had_deficit, payback = balance < 0, None
        try:
            for time, amount in flows:
                discounted.append(amount/(1+rate)**((time-start)/scale))
                balance += amount
                if had_deficit and balance >= 0 and payback is None:
                    payback = time
                had_deficit = had_deficit or balance < 0
            npv = math.fsum(discounted)
        except (OverflowError, ZeroDivisionError) as error:
            raise ValueError('Valuation arithmetic overflow') from error
        require(number(npv) and number(balance), 'Valuation arithmetic overflow')
        result['valuation'] = dict(v, incremental_npv=npv, undiscounted_payback_time=payback,
            payback_status='recovered' if payback is not None else 'not_recovered' if had_deficit else 'no_deficit',
            convention='Period-end discounting from window start; first sampled recovery after a deficit; no interpolation or permanence guarantee',
            unit=baseline.metrics[metric]['unit'])
    return result
