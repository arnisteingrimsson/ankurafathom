"""Versioned machine-readable HTTP contract for project UI builders."""


def obj(properties, required=()):
    return dict(type='object', additionalProperties=False, properties=properties, required=list(required))


def array(items): return dict(type='array', items=items)
def ref(name): return {'$ref': '#/components/schemas/'+name}


def document():
    string, numeric = dict(type='string'), dict(type='number')
    natural = dict(type='integer', minimum=0, maximum=65535)
    overrides = dict(type='object', additionalProperties=numeric)
    schemas = {}
    schemas['Scenario'] = obj(dict(id=natural, overrides=overrides), ('id', 'overrides'))
    schemas['RunRequest'] = obj(dict(model_id=string, model_version=string, overrides=overrides,
        scenarios=dict(type='array', minItems=1, maxItems=1024, items=ref('Scenario')),
        seed=dict(type='integer', minimum=0, maximum=2**64-1), replications=dict(type='integer', minimum=1, maximum=1024),
        threads=dict(type='integer', minimum=1, maximum=32), require_check=dict(type='boolean')), ('model_id', 'model_version'))
    schemas['RunRequest']['not'] = {'required': ['overrides', 'scenarios']}
    schemas['Threshold'] = obj(dict(metric=string, operator=dict(type='string', enum=['gte', 'lte']), value=numeric), ('metric', 'operator', 'value'))
    schemas['SummaryRequest'] = obj({'metrics': dict(type='array', minItems=1, uniqueItems=True, items=string), 'scenario': natural,
        'replication': natural, 'from': numeric, 'to': numeric, 'thresholds': array(ref('Threshold'))}, ('metrics', 'from', 'to'))
    schemas['Valuation'] = obj(dict(metric=string, annual_discount_rate=dict(type='number', exclusiveMinimum=-1),
        time_units_per_year=dict(type='number', exclusiveMinimum=0), initial_incremental_cash_flow=numeric),
        ('metric', 'annual_discount_rate', 'time_units_per_year', 'initial_incremental_cash_flow'))
    schemas['CompareRequest'] = obj({'baseline_run': string, 'candidate_run': string, 'baseline_scenario': natural,
        'candidate_scenario': natural, 'replication': natural, 'metrics': dict(type='array', minItems=1, items=string),
        'from': numeric, 'to': numeric, 'valuation': ref('Valuation')}, ('baseline_run', 'candidate_run', 'metrics', 'from', 'to'))
    schemas['ExplainRequest'] = obj({'metric': string, 'at': numeric, 'from': numeric, 'scenario': natural, 'replication': natural}, ('metric', 'at'))
    schemas['Error'] = obj(dict(error=obj(dict(code=string, message=string), ('code', 'message'))), ('error',))
    schemas['Progress'] = obj(dict(kind=dict(type='string', const='execution_stages'),
        completed_trajectories=dict(type=['integer', 'null'], minimum=0), total_trajectories=dict(type='integer', minimum=1)),
        ('kind', 'completed_trajectories', 'total_trajectories'))
    schemas['Run'] = dict(type='object', required=['id', 'status', 'stage', 'model_id', 'model_version', 'progress', 'artifacts'],
        properties=dict(id=string, status=dict(type='string', enum=['queued', 'running', 'cancelling', 'completed', 'cancelled', 'failed', 'interrupted']),
                        stage=string, model_id=string, model_version=string, progress=ref('Progress'),
                        artifacts=dict(type='object', additionalProperties=string), cache_hit=dict(type='boolean')))
    schemas['Parameter'] = obj(dict(id=string, label=string, description=string,
        kind=dict(type='string', enum=['number', 'integer', 'boolean']), minimum=numeric, maximum=numeric,
        choices=dict(type='array', minItems=1, uniqueItems=True, items=numeric),
        status=dict(type='string', enum=['synthetic', 'calibrated', 'agreed', 'unspecified']), source=string),
        ('id', 'label', 'description', 'kind', 'status', 'source'))
    schemas['Parameter']['oneOf'] = [dict(required=['choices'], **{'not': {'anyOf': [{'required': ['minimum']}, {'required': ['maximum']}]}}),
                                    dict(required=['minimum', 'maximum'], **{'not': {'required': ['choices']}})]
    schemas['Metric'] = obj(dict(id=string, label=string, definition=string, unit=string,
        format=dict(type='string', enum=['number', 'integer', 'currency', 'percent']), dimensions=dict(type='object', additionalProperties=string),
        aggregation=dict(type='string', enum=['last', 'sum', 'delta', 'integral', 'mean', 'ratio', 'sum_metrics']),
        output=string, numerator=string, denominator=string, members=array(string),
        interpolation=dict(type='string', enum=['hold', 'linear'])), ('id', 'label', 'definition', 'unit', 'format', 'dimensions', 'aggregation'))
    schemas['Descriptor'] = obj(dict(schema_version=dict(type='string', const='1.0'), id=string, label=string, description=string,
        parameters=array(ref('Parameter')), metrics=dict(type='array', minItems=1, items=ref('Metric')),
        constraints=array(obj(dict(kind=dict(type='string', enum=['sum_lte', 'sum_equal']), parameters=array(string), value=numeric, message=string),
                              ('kind', 'parameters', 'value', 'message')))), ('schema_version', 'id', 'label', 'description', 'parameters', 'metrics'))
    described = {**schemas['Parameter'], 'properties': {**schemas['Parameter']['properties'], 'default': numeric, 'unit': string}}
    schemas['Description'] = dict(type='object', required=['id', 'model_version', 'parameters', 'metrics', 'capabilities', 'time'],
        properties=dict(id=string, model_version=string, label=string, description=string, parameters=array(described),
                        metrics=array(ref('Metric')), capabilities=dict(type='object'), time=dict(type='object'),
                        input_sha256=dict(type='object', additionalProperties=string)))
    schemas['Observation'] = obj(dict(metric=string, time=numeric, value=numeric, scenario=natural, replication=natural,
                                     dimensions=dict(type='object', additionalProperties=string)),
                                ('metric', 'time', 'value', 'scenario', 'replication', 'dimensions'))
    schemas['Results'] = dict(type='object', required=['rows', 'provenance', 'next_offset', 'downsampling', 'authoritative_for_aggregation'],
        properties=dict(rows=array(ref('Observation')), next_offset=dict(type=['integer', 'null']),
                        source_rows=dict(type='integer'), total_rows=dict(type='integer'), downsampling=string,
                        authoritative_for_aggregation=dict(type='boolean', const=False), provenance=dict(type='object')))
    nullable_number, nullable_string = dict(type=['number', 'null']), dict(type=['string', 'null'])
    dimensions = dict(type='object', additionalProperties=string)
    schemas['Interval'] = obj(dict(start=numeric, end=numeric), ('start', 'end'))
    schemas['Provenance'] = obj(dict(run_id=string, model_id=string, model_version=string,
        native_manifest_id=nullable_string, engine_identity=dict(type='object'), artifacts=dict(type='object', additionalProperties=string)),
        ('run_id', 'model_id', 'model_version', 'engine_identity', 'artifacts'))
    schemas['MetricValue'] = obj(dict(metric=string, value=nullable_number, unit=string, aggregation=string,
        reason=nullable_string, dimensions=dimensions, interval=ref('Interval')),
        ('metric', 'value', 'unit', 'aggregation', 'reason', 'dimensions', 'interval'))
    schemas['ThresholdResult'] = obj(dict(metric=string, operator=dict(type='string', enum=['gte', 'lte']), value=numeric,
        observed=nullable_number, passed=dict(type=['boolean', 'null'])), ('metric', 'operator', 'value', 'observed', 'passed'))
    schemas['Summary'] = obj(dict(scenario=natural, replication=natural, values=array(ref('MetricValue')),
        thresholds=array(ref('ThresholdResult')), provenance=ref('Provenance'), calculation_version=string,
        data_resolution=dict(type='string', const='full')),
        ('scenario', 'replication', 'values', 'thresholds', 'provenance', 'calculation_version', 'data_resolution'))
    schemas['ComparisonValue'] = obj(dict(metric=string, baseline=nullable_number, candidate=nullable_number,
        delta=nullable_number, percent_change=nullable_number, percent_convention=string, reason=nullable_string,
        unit=string, dimensions=dimensions),
        ('metric', 'baseline', 'candidate', 'delta', 'percent_change', 'percent_convention', 'reason', 'unit', 'dimensions'))
    schemas['ValuationResult'] = obj(dict(schemas['Valuation']['properties'], incremental_npv=numeric,
        undiscounted_payback_time=nullable_number, payback_status=dict(type='string', enum=['recovered', 'not_recovered', 'no_deficit']),
        convention=string, unit=string), (*schemas['Valuation']['required'], 'incremental_npv', 'undiscounted_payback_time', 'payback_status', 'convention', 'unit'))
    schemas['Comparison'] = obj(dict(values=array(ref('ComparisonValue')), baseline_provenance=ref('Provenance'),
        candidate_provenance=ref('Provenance'), calculation_version=string, interval=ref('Interval'), replication=natural,
        uncertainty=string, valuation=ref('ValuationResult')),
        ('values', 'baseline_provenance', 'candidate_provenance', 'calculation_version', 'interval', 'replication', 'uncertainty'))
    schemas['Results']['properties']['provenance'] = ref('Provenance')
    paths = {}
    def route(path, method, operation, response=None, body=None, description='', parameters=None):
        success = dict(description='Successful operation', content={'application/json': dict(schema=ref(response) if response else dict(type='object'))})
        responses = {'200': success, 'default': dict(description='Structured error', content={'application/json': dict(schema=ref('Error'))})}
        if operation == 'createRun':
            responses['202'] = dict(success, description='Accepted asynchronous execution')
        value = dict(operationId=operation, description=description, responses=responses)
        if '{id}' in path:
            value['parameters'] = [dict(name='id', **{'in': 'path'}, required=True, schema=string)]
        if parameters:
            value.setdefault('parameters', []).extend(parameters)
        if body:
            value['requestBody'] = dict(required=True, content={'application/json': dict(schema=ref(body))})
        paths.setdefault(path, {})[method] = value
    route('/health', 'get', 'health')
    route('/v1/models', 'get', 'listModels')
    route('/v1/models/{id}/describe', 'get', 'describeModel', 'Description')
    route('/v1/runs', 'post', 'createRun', 'Run', 'RunRequest', 'Runs are immutable. Completed exact matches return cache_hit=true and the original run ID.')
    route('/v1/runs/{id}', 'get', 'getRun', 'Run')
    schemas['Empty'] = obj({})
    route('/v1/runs/{id}/cancel', 'post', 'cancelRun', 'Run', 'Empty', 'Idempotent; retained run record. Cancelled runs publish no result API.')
    params = [dict(name=name, **{'in': 'query'}, required=False, schema=schema,
                   **({'style': 'form', 'explode': True} if name in ('metric', 'dimension') else {})) for name, schema in [
        ('metric', array(string)), ('dimension', array(dict(type='string', description='dimension-name:value'))),
        ('scenario', natural), ('replication', natural), ('from', numeric), ('to', numeric),
        ('max_points', dict(type='integer', minimum=2)), ('offset', dict(type='integer', minimum=0)),
        ('limit', dict(type='integer', minimum=1, maximum=10000))]]
    route('/v1/runs/{id}/results', 'get', 'getResults', 'Results', parameters=params,
          description='Raw declared output series; endpoints preserved under sample selection. Derived metrics use summary. Never compute KPIs from downsampled rows.')
    route('/v1/runs/{id}/summary', 'post', 'summarizeRun', 'Summary', 'SummaryRequest', 'Full-resolution aggregation on recorded interval endpoints; single selected scenario/replication.')
    route('/v1/compare', 'post', 'compareRuns', 'Comparison', 'CompareRequest', 'Same model and engine version only; no implicit averaging across replications.')
    route('/v1/runs/{id}/explain', 'post', 'explainRun', body='ExplainRequest', description='Existing native standalone SD verified re-execution; 120-second request limit.')
    route('/v1/runs/{id}/events', 'get', 'streamStatus', description='SSE status events. Reconnect receives current status, not historical event replay. Stream ends at terminal state.')
    paths['/v1/runs/{id}/events']['get']['responses']['200'] = dict(description='Stage updates', content={'text/event-stream': dict(schema=string)})
    return dict(openapi='3.1.0', info=dict(title='AnkuraFathom local project API', version='1.0.0'),
                servers=[dict(url='http://127.0.0.1:8765')], paths=paths, components=dict(schemas=schemas))
