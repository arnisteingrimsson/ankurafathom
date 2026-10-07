"""Strict project descriptors, immutable source capture and request validation."""
import csv
import hashlib
import json
import math
import re
from pathlib import Path


class APIError(Exception):
    def __init__(self, code, message, status=422):
        super().__init__(message)
        self.code, self.status = code, status


def require(condition, message, code='INVALID_REQUEST', status=422):
    if not condition:
        raise APIError(code, message, status)


def fields(value, allowed, required=()):
    require(isinstance(value, dict), 'Expected an object')
    require(not value.keys()-set(allowed), 'Unknown fields: '+', '.join(sorted(value.keys()-set(allowed))))
    require(set(required) <= value.keys(), 'Missing fields: '+', '.join(sorted(set(required)-value.keys())))


def number(value):
    return type(value) in (int, float) and math.isfinite(value)


def integer(value, low, high):
    return type(value) is int and low <= value <= high


def identifier(value):
    return isinstance(value, str) and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_-]{0,127}', value) is not None


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def loads(data):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'Duplicate JSON key: '+key)
            result[key] = value
        return result
    try:
        return json.loads(data, object_pairs_hook=pairs,
                          parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    except (ValueError, UnicodeError) as error:
        raise APIError('INVALID_JSON', 'Expected finite, valid JSON') from error


def read_table(path):
    if path.suffix == '.csv':
        with path.open(newline='') as stream:
            return list(csv.DictReader(stream))
    try:
        import pyarrow as pa
        if path.suffix == '.parquet':
            import pyarrow.parquet as pq
            return pq.read_table(path).to_pylist()
        with pa.memory_map(str(path), 'r') as stream:
            return pa.ipc.open_file(stream).read_all().to_pylist()
    except ImportError as error:
        raise APIError('ARROW_REQUIRED', 'Install runtime/requirements.txt to describe bound Arrow/Parquet parameters') from error


def capture(model_path, descriptor_path):
    """Capture exact bytes once; requests never read mutable project source files."""
    model_path, descriptor_path = Path(model_path).resolve(), Path(descriptor_path).resolve()
    files = {'model.json': model_path.read_bytes()}
    model = loads(files['model.json'])
    descriptor = loads(descriptor_path.read_bytes())
    require(isinstance(model, dict), 'Model must be an object')
    for binding in model.get('data', []):
        source = binding['source']
        require(isinstance(source, str), 'Data source must be a relative file path')
        path = Path(source)
        require(not path.is_absolute() and '..' not in path.parts and str(path) == source and source not in ('model.json', 'descriptor.json'),
                'Data sources must stay inside the model directory')
        resolved = (model_path.parent/path).resolve()
        require(resolved.is_relative_to(model_path.parent), 'Data symlink escapes model directory')
        files[source] = resolved.read_bytes()
    files['descriptor.json'] = canonical(descriptor)
    return model, descriptor, files


def describe(model, spec, directory):
    fields(spec, ('schema_version', 'id', 'label', 'description', 'parameters', 'metrics', 'constraints'),
           ('schema_version', 'id', 'label', 'description', 'parameters', 'metrics'))
    require(spec['schema_version'] == '1.0' and identifier(spec['id']), 'Unsupported descriptor version or model ID')
    for name in ('label', 'description'):
        require(isinstance(spec[name], str) and bool(spec[name].strip()), 'Missing '+name)
    require(isinstance(spec['parameters'], list) and isinstance(spec['metrics'], list) and spec['metrics'], 'Expected parameter/metric arrays')
    native = {p['id']: dict(p) for p in model.get('parameters', [])}
    # Resolve effective bound defaults from the captured tables, never placeholder
    # IR literals. Native lint remains authoritative for table/schema validity.
    for binding in model.get('data', []):
        use = binding['use']
        if use['kind'] != 'parameter_table':
            continue
        rows = read_table(directory/binding['source'])
        key = binding['schema']['key_column']
        key_type = next(column['type'] for column in binding['schema']['columns'] if column['name'] == key)
        def typed_key(value):
            if key_type in ('f64', 'i64', 'u64', 'integer', 'real'):
                return float(value)
            if key_type in ('bool', 'boolean') and isinstance(value, str):
                require(value in ('true', 'false', '0', '1'), 'Invalid boolean table key')
                return value in ('true', '1')
            return value
        matches = [row for row in rows if typed_key(row[key]) == use['key']]
        require(len(matches) == 1, 'Parameter binding key must select exactly one row')
        for mapping in use['parameters']:
            native[mapping['parameter']]['value'] = float(matches[0][mapping['column']])
    exposed = []
    seen = set()
    for p in spec['parameters']:
        fields(p, ('id', 'label', 'description', 'kind', 'minimum', 'maximum', 'choices', 'status', 'source'),
               ('id', 'label', 'description', 'kind', 'status', 'source'))
        require(p['id'] in native and p['id'] not in seen, 'Unknown/duplicate parameter')
        require(p['kind'] in ('number', 'integer', 'boolean'), 'Unsupported parameter kind')
        require(p['status'] in ('synthetic', 'calibrated', 'agreed', 'unspecified'), 'Unsupported assumption status')
        for text in ('label', 'description', 'source'):
            require(isinstance(p[text], str) and bool(p[text].strip()), 'Parameter '+text+' is required')
        require(('choices' in p) != ('minimum' in p or 'maximum' in p), 'Declare choices or inclusive minimum/maximum')
        if 'choices' in p:
            require(isinstance(p['choices'], list) and p['choices'] and all(number(v) for v in p['choices']), 'Invalid choices')
            require(len(set(p['choices'])) == len(p['choices']), 'Duplicate choices')
            for choice in p['choices']:
                validate_parameter(p, choice)
        else:
            require(number(p.get('minimum')) and number(p.get('maximum')) and p['minimum'] <= p['maximum'], 'Invalid bounds')
            require(p['kind'] != 'boolean' or (p['minimum'] in (0, 1) and p['maximum'] in (0, 1)), 'Boolean bounds must use 0/1')
        default = native[p['id']]['value']
        validate_parameter(p, default)
        exposed.append(dict(p, default=default, unit=native[p['id']]['unit']))
        seen.add(p['id'])
    outputs = {o['id']: o for o in model['outputs']}
    components = {c['id']: c for c in model['components']}
    metrics = {}
    for metric in spec['metrics']:
        fields(metric, ('id', 'label', 'definition', 'unit', 'format', 'dimensions', 'aggregation', 'output',
                        'numerator', 'denominator', 'members', 'interpolation'),
               ('id', 'label', 'definition', 'unit', 'format', 'dimensions', 'aggregation'))
        require(identifier(metric['id']) and metric['id'] not in metrics, 'Invalid/duplicate metric ID')
        require(all(isinstance(metric[k], str) and metric[k] for k in ('label', 'definition', 'unit')), 'Metric text fields required')
        require(metric['format'] in ('number', 'integer', 'currency', 'percent'), 'Unsupported display format')
        require(isinstance(metric['dimensions'], dict) and all(identifier(k) and isinstance(v, str) for k, v in metric['dimensions'].items()), 'Invalid metric dimensions')
        kind = metric['aggregation']
        require(kind in ('last', 'sum', 'delta', 'integral', 'mean', 'ratio', 'sum_metrics'), 'Unsupported aggregation')
        if kind in ('ratio', 'sum_metrics'):
            require('output' not in metric and 'interpolation' not in metric, 'Derived metrics cannot declare a native output')
            if kind == 'ratio':
                require(isinstance(metric.get('numerator'), str) and isinstance(metric.get('denominator'), str) and 'members' not in metric, 'Ratio needs numerator and denominator')
            else:
                require(isinstance(metric.get('members'), list) and metric['members'] and all(isinstance(v, str) for v in metric['members']) and len(set(metric['members'])) == len(metric['members']), 'Sum needs unique metric members')
                require('numerator' not in metric and 'denominator' not in metric, 'Unexpected ratio fields')
        else:
            require(metric.get('output') in outputs, 'Unknown native output')
            require(not {'numerator', 'denominator', 'members'} & metric.keys(), 'Unexpected derived fields')
            require(metric.get('interpolation') in ('hold', 'linear'), 'Declare hold or linear interpolation')
            output = outputs[metric['output']]
            unit = output.get('unit', components.get(output.get('stock'), {}).get('unit'))
            if unit:
                require(unit == metric['unit'], 'Metric unit must match the native output unit')
        metrics[metric['id']] = dict(metric)
    visited, active = set(), set()
    def visit(key):
        require(key in metrics, 'Unknown metric dependency: '+str(key))
        require(key not in active, 'Metric dependency cycle')
        if key in visited:
            return
        active.add(key)
        metric = metrics[key]
        deps = [metric['numerator'], metric['denominator']] if metric['aggregation'] == 'ratio' else metric.get('members', [])
        for dep in deps:
            visit(dep)
            if metric['aggregation'] == 'sum_metrics':
                require(metrics[dep]['unit'] == metric['unit'], 'Summed metric units differ')
        active.remove(key)
        visited.add(key)
    for key in metrics:
        visit(key)
    constraints = spec.get('constraints', [])
    require(isinstance(constraints, list), 'Constraints must be an array')
    for constraint in constraints:
        fields(constraint, ('kind', 'parameters', 'value', 'message'), ('kind', 'parameters', 'value', 'message'))
        require(constraint['kind'] in ('sum_lte', 'sum_equal') and isinstance(constraint['parameters'], list)
                and constraint['parameters'] and all(p in seen for p in constraint['parameters'])
                and len(set(constraint['parameters'])) == len(constraint['parameters'])
                and number(constraint['value']) and isinstance(constraint['message'], str), 'Invalid cross-parameter constraint')
    result = dict(spec, parameters=exposed, metrics=list(metrics.values()), time=model['time'],
                  capabilities=dict(playback='observations', animation_trace=False,
                                    explain=model.get('mode', 'sd') == 'sd' and not any(c.get('clip', False) for c in model['components']),
                                    progress='execution_stages', cancellation='isolated_process',
                                    validation=bool(model.get('checks')), parameter_constraints='service_contract'))
    validate_overrides(result, {})
    return result


def validate_parameter(spec, value):
    require(number(value), 'Parameter values must be finite numbers (boolean switches use 0/1)')
    require(spec['kind'] != 'integer' or float(value).is_integer(), 'Integer parameter required')
    require(spec['kind'] != 'boolean' or value in (0, 1), 'Boolean parameter requires numeric 0 or 1')
    require(value in spec['choices'] if 'choices' in spec else spec['minimum'] <= value <= spec['maximum'], 'Parameter outside declared domain: '+spec['id'])


def validate_overrides(description, overrides):
    require(isinstance(overrides, dict), 'overrides must be an object')
    parameters = {p['id']: p for p in description['parameters']}
    require(not overrides.keys()-parameters.keys(), 'Unknown or non-editable parameter override')
    values = {key: p['default'] for key, p in parameters.items()}
    for key, value in overrides.items():
        validate_parameter(parameters[key], value)
        values[key] = value
    for rule in description.get('constraints', []):
        value = math.fsum(values[p] for p in rule['parameters'])
        require(value <= rule['value'] if rule['kind'] == 'sum_lte' else math.isclose(value, rule['value'], rel_tol=0, abs_tol=1e-12), rule['message'])
    return values
