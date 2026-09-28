"""Check Draft 2020-12 IR structure against the independent C++ loader."""

import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from jsonschema import Draft202012Validator
from typed_schema_cases import cases as typed_cases
from abm_schema_cases import cases as abm_cases
from agent_stock_schema_cases import cases as agent_stock_cases
from abm_generator_ir_contract import model as generator_model
from oracles.abm.interaction_plan import make_plan as interaction_plan


ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / "ir/schema/ankurafathom-ir.schema.json"
MODELS = ROOT / "models"


def lint(executable: Path, path: Path) -> bool:
    result = subprocess.run(
        [str(executable), "lint", str(path)], capture_output=True, text=True, check=False
    )
    payload = json.loads(result.stdout if result.returncode == 0 else result.stderr)
    return result.returncode == 0 and payload["verdict"] == "pass"


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: schema_contract.py /path/to/fathom")
    executable = Path(sys.argv[1]).resolve()
    schema = json.loads(SCHEMA.read_text())
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)

    valid_paths = sorted(
        path for path in MODELS.glob("*.ir.json") if not path.name.startswith("invalid_")
    )
    if not valid_paths:
        raise AssertionError("no valid IR fixtures found")
    for path in valid_paths:
        instance = json.loads(path.read_text())
        validator.validate(instance)
        if not lint(executable, path):
            raise AssertionError(f"loader rejected schema-valid fixture: {path}")

    interaction_cases=interaction_plan()['cases']
    generator_cases=[generator_model(kind, asynchronous=asynchronous)
                     for kind in ['erdos_renyi','watts_strogatz','barabasi_albert']
                     for asynchronous in [False,True]]
    with tempfile.TemporaryDirectory() as directory:
        for case in interaction_cases:
            validator.validate(case['model'])
            path=Path(directory)/(case['id']+'.json')
            path.write_text(json.dumps(case['model']))
            if not lint(executable,path):
                raise AssertionError('loader rejected interaction fixture: '+case['id'])
        for index,case in enumerate(generator_cases):
            validator.validate(case)
            path=Path(directory)/f'generator-{index}.json'
            path.write_text(json.dumps(case))
            if not lint(executable,path):
                raise AssertionError(f'loader rejected generator fixture: {index}')

    decay = json.loads((MODELS / "decay.ir.json").read_text())
    process = json.loads((MODELS / "process.ir.json").read_text())
    tandem = json.loads((MODELS / "tandem_process.ir.json").read_text())
    stochastic = json.loads((MODELS / "stochastic_process.ir.json").read_text())
    hybrid = json.loads((MODELS / "hybrid_completion.ir.json").read_text())
    feedback = json.loads((MODELS / "hybrid_feedback.ir.json").read_text())
    rate = json.loads((MODELS / "hybrid_rate.ir.json").read_text())
    adoption = json.loads((MODELS / "abm_adoption.ir.json").read_text())
    agent_pool = json.loads((MODELS / "agent_pool.ir.json").read_text())
    agent_phases = json.loads((MODELS / "agent_pool_phases.ir.json").read_text())
    delivery = json.loads((MODELS / "agent_pool_delivery.ir.json").read_text())
    utilization = json.loads((MODELS / "agent_pool_utilization.ir.json").read_text())
    staffed_sd = json.loads((MODELS / "agent_pool_sd.ir.json").read_text())
    variable_delay = json.loads((MODELS / "variable_delay.ir.json").read_text())
    mutations, typed_semantic = typed_cases(MODELS)
    abm_structural, abm_semantic = abm_cases(MODELS)
    mutations.extend(abm_structural)
    typed_semantic.extend(abm_semantic)
    agent_stock_structural, agent_stock_semantic = agent_stock_cases(MODELS)
    mutations.extend(agent_stock_structural)
    typed_semantic.extend(agent_stock_semantic)
    probability = json.loads((MODELS / "probability_process.ir.json").read_text())
    for key, values in (("match", (-.01, 1.01, None, True, "0.5")),
                        ("stream", (-1, 65536, .5, None, True))):
        for value in values:
            case = copy.deepcopy(probability)
            case["components"][2]["probability"][key] = value
            mutations.append((f"probability_{key}_{value}", case))
    for key in ("match", "stream"):
        case = copy.deepcopy(probability)
        del case["components"][2]["probability"][key]
        mutations.append((f"probability_missing_{key}", case))
    for value in (None, True, [], .5, {"match":.5, "stream":301, "unknown":1}):
        case = copy.deepcopy(probability)
        case["components"][2]["probability"] = value
        mutations.append((f"probability_shape_{value}", case))
    case = copy.deepcopy(probability)
    case["components"][2]["priority_at_most"] = 0
    mutations.append(("router_two_rules", case))
    independent = json.loads((MODELS / "independent_service_process.ir.json").read_text())
    for key, values in (("kind", ("erlang", None, True)), ("rate", (0, -1, None, True, "1")),
                        ("stream", (-1, 65536, .5, None, True))):
        for value in values:
            case = copy.deepcopy(independent)
            case["components"][1]["service"][key] = value
            mutations.append((f"service_{key}_{value}", case))
    for key in ("kind", "rate", "stream"):
        case = copy.deepcopy(independent)
        del case["components"][1]["service"][key]
        mutations.append((f"service_missing_{key}", case))
    for value in (None, True, [], "exponential", {"kind":"exponential", "rate":1, "stream":201, "unknown":1}):
        case = copy.deepcopy(independent)
        case["components"][1]["service"] = value
        mutations.append((f"service_shape_{value}", case))
    graph = json.loads((MODELS / "routed_process.ir.json").read_text())
    for value in (True, None, "high", 0.5, -2147483649, 2147483648):
        case = copy.deepcopy(graph)
        case["components"][1]["priority_at_most"] = value
        mutations.append((f"router_priority_{value}", case))
    case = copy.deepcopy(graph)
    del case["components"][1]["priority_at_most"]
    mutations.append(("router_missing_threshold", case))
    case = copy.deepcopy(graph)
    case["components"][1]["condition"] = "priority < 0"
    mutations.append(("router_unknown_condition", case))
    for value in ("unknown", True, None):
        case = copy.deepcopy(graph)
        case["links"][0]["port"] = value
        mutations.append((f"route_port_{value}", case))
    for key, values in (("queue_capacity", (-1, 1.5, True, None, 1000001)),
                        ("discipline", ("random", "preemptive", True, None))):
        for value in values:
            case = copy.deepcopy(process)
            case["components"][1][key] = value
            mutations.append((f"server_{key}_{value}", case))
    for value in (-2147483649, 2147483648, 18446744073709551615, 0.5, True, None, "high"):
        case = copy.deepcopy(process)
        case["components"][0]["schedule"][0]["priority"] = value
        mutations.append((f"entity_priority_{value}", case))
    history_delay = json.loads((MODELS / 'history_delay.ir.json').read_text())
    for order in (1,3,5,255):
        case = copy.deepcopy(history_delay)
        case['components'][2]['order'] = order
        mutations.append(('history_order_'+str(order),case))
    absolute_clock = json.loads((MODELS / "nonzero_start.ir.json").read_text())
    for invalid in (True, "10", None):
        case = copy.deepcopy(absolute_clock)
        case['time']['start'] = invalid
        mutations.append(('invalid_start_'+str(invalid), case))
    for label, original in [('des', process), ('hybrid', hybrid), ('abm_sd', adoption),
                            ('agent_pool', agent_pool), ('agent_pool_sd', staffed_sd)]:
        case = copy.deepcopy(original)
        case['time']['start'] = 0
        mutations.append(('start_wrong_mode_'+label, case))

    clipping = json.loads((MODELS / "clipping.ir.json").read_text())
    for field, value in [('clip_outflows', 'true'), ('outflow_order', 'high'),
                         ('outflow_order', ['high', 'high']), ('non_negative', False),
                         ('clip_outflows', False)]:
        case = copy.deepcopy(clipping)
        case['components'][0][field] = value
        mutations.append(('clipping_stock_'+str(len(mutations)), case))
    case = copy.deepcopy(clipping)
    del case['components'][0]['outflow_order']
    mutations.append(('clipping_missing_priority', case))
    for field, value in [('clip_negative', 1), ('non_negative', False)]:
        case = copy.deepcopy(clipping)
        case['components'][3][field] = value
        mutations.append(('clipping_flow_'+field, case))
    case = copy.deepcopy(clipping)
    case['integrator'] = 'rk4'
    mutations.append(('rk4_clipping', case))
    for field in ('clip_outflows', 'clip_negative', 'outflow_order'):
        case = copy.deepcopy(hybrid)
        case['sd']['components'][0][field] = [] if field == 'outflow_order' else False
        mutations.append(('hybrid_clipping_'+field, case))

    for method in ('RK4', 'midpoint', 'rk45', None, 1):
        case = copy.deepcopy(decay)
        case['integrator'] = method
        mutations.append(('invalid_integrator_'+str(method), case))
    for label, original in [('des', process), ('hybrid', hybrid), ('abm_sd', adoption),
                            ('agent_pool', agent_pool), ('agent_pool_sd', staffed_sd)]:
        case = copy.deepcopy(original)
        case['integrator'] = 'rk4'
        mutations.append(('integrator_wrong_mode_'+label, case))
    case = copy.deepcopy(variable_delay)
    case['integrator'] = 'rk4'
    mutations.append(('rk4_delay_component', case))

    expression_outputs = json.loads((MODELS / "expression_outputs.ir.json").read_text())
    for output in (
        {"id":"x", "expr":"1"}, {"id":"x", "expr":1, "unit":"1"},
        {"id":"x", "expr":"", "unit":"1"}, {"id":"x", "expr":"1", "unit":""},
        {"id":"x", "expr":"1", "unit":"1", "stock":"s"},
        {"id":"x", "stock":"s", "unit":"1"},
    ):
        case = copy.deepcopy(expression_outputs)
        case["outputs"] = [output]
        mutations.append(("invalid_expression_output_"+str(len(mutations)), case))

    for order in (0, -1, 256, 2.5, True, "2"):
        case = copy.deepcopy(variable_delay)
        case['components'][2]['order'] = order
        mutations.append((f'invalid_delay_order_{order}', case))
    case = copy.deepcopy(variable_delay)
    case['components'][2].update(type='fixed', order=2)
    mutations.append(('fixed_delay_cascade_order', case))

    for invalid_duration in (True, None, {}, [], "", 0, -1):
        case = copy.deepcopy(variable_delay)
        case["components"][2]["duration"] = invalid_duration
        mutations.append((f"invalid_delay_duration_{invalid_duration}", case))

    for invalid_flag in ("false", 0, None):
        case = copy.deepcopy(decay)
        case["components"][1]["non_negative"] = invalid_flag
        mutations.append((f"invalid_flow_sign_{invalid_flag}", case))

    case = copy.deepcopy(decay)
    del case["components"][0]["unit"]
    mutations.append(("missing_stock_unit", case))
    case = copy.deepcopy(process)
    case["components"][0]["schedule"][0]["service"] = 0
    mutations.append(("zero_service", case))
    case = copy.deepcopy(tandem)
    case["components"][1]["service_scale"] = 0
    mutations.append(("zero_service_scale", case))
    case = copy.deepcopy(stochastic)
    case["components"][0]["schedule"] = []
    mutations.append(("two_source_modes", case))
    case = copy.deepcopy(process)
    case["outputs"][0]["metric"] = "unknown_metric"
    mutations.append(("unknown_metric", case))
    case = copy.deepcopy(process)
    case["links"][0]["extra"] = 1
    mutations.append(("unknown_link_field", case))
    case = copy.deepcopy(hybrid)
    del case["bridges"][0]["amount"]
    mutations.append(("missing_bridge_amount", case))
    case = copy.deepcopy(hybrid)
    case["outputs"][0]["metric"] = "completed"
    mutations.append(("stock_output_with_metric", case))
    case = copy.deepcopy(hybrid)
    case["sd"]["components"][0]["kind"] = "table"
    mutations.append(("unsupported_hybrid_sd_kind", case))
    case = copy.deepcopy(feedback)
    case["bridges"][1]["service"] = 0
    mutations.append(("zero_feedback_service", case))
    case = copy.deepcopy(feedback)
    del case["bridges"][1]["max_count"]
    mutations.append(("missing_feedback_limit", case))
    case = copy.deepcopy(rate)
    case["bridges"][1]["service_rate"] = 0
    mutations.append(("zero_rate_service", case))
    case = copy.deepcopy(rate)
    del case["bridges"][1]["stream"]
    mutations.append(("missing_rate_stream", case))
    case = copy.deepcopy(adoption)
    case["abm"]["population"]["count"] = 0
    mutations.append(("zero_abm_population", case))
    case = copy.deepcopy(adoption)
    case["outputs"][3]["metric"] = "unknown"
    mutations.append(("unknown_abm_metric", case))
    case = copy.deepcopy(agent_pool)
    del case["agent_pool"]["pool"]["max_request_units"]
    mutations.append(("missing_agent_pool_request_limit", case))
    case = copy.deepcopy(agent_pool)
    case["agent_pool"]["schedule"][0]["requests"][0]["units"] = 0
    mutations.append(("zero_agent_pool_request", case))
    case = copy.deepcopy(agent_pool)
    case["outputs"][0]["metric"] = "unknown"
    mutations.append(("unknown_agent_pool_metric", case))
    case = copy.deepcopy(agent_pool)
    case["agent_pool"]["schedule"][0]["requests"][0]["request_id"] = 2**64
    mutations.append(("agent_pool_request_id_overflow", case))
    case = copy.deepcopy(agent_phases)
    del case["agent_pool"]["phases"][0]["unit"]
    mutations.append(("agent_phase_missing_unit", case))
    case = copy.deepcopy(agent_phases)
    case["agent_pool"]["schedule"][1]["phases"][0] = 7
    mutations.append(("agent_phase_nonstring_reference", case))
    case = copy.deepcopy(agent_phases)
    case["agent_pool"]["phases"][0]["unknown"] = True
    mutations.append(("agent_phase_unknown_field", case))
    case = copy.deepcopy(delivery)
    del case["agent_pool"]["schedule"][0]["engagements"][0]["duration"]
    mutations.append(("delivery_missing_duration", case))
    case = copy.deepcopy(delivery)
    case["agent_pool"]["schedule"][0]["engagements"][0]["duration"] = 0
    mutations.append(("delivery_zero_duration", case))
    for field in ("requests", "releases"):
        case = copy.deepcopy(delivery)
        case["agent_pool"]["schedule"][1][field] = []
        mutations.append((f"delivery_manual_{field}", case))
    case = copy.deepcopy(delivery)
    del case["agent_pool"]["delivery"]
    mutations.append(("engagements_without_delivery", case))
    case = copy.deepcopy(utilization)
    case["outputs"][6]["metric"] = "mean_utilization"
    mutations.append(("unsupported_utilization_metric", case))
    case = copy.deepcopy(utilization)
    del case["outputs"][6]["process"]
    case["outputs"][6]["pool"] = "staffing"
    mutations.append(("utilization_requires_process_selector", case))

    for field in ("sd", "bridges"):
        case = copy.deepcopy(staffed_sd)
        del case[field]
        mutations.append((f"staffed_sd_missing_{field}", case))
    case = copy.deepcopy(staffed_sd)
    del case["agent_pool"]["delivery"]
    mutations.append(("staffed_sd_missing_delivery", case))
    case = copy.deepcopy(staffed_sd)
    case["bridges"].append(copy.deepcopy(case["bridges"][0]))
    mutations.append(("staffed_sd_multiple_bridges", case))
    case = copy.deepcopy(staffed_sd)
    case["bridges"][0]["amount"] = 0
    mutations.append(("staffed_sd_zero_amount", case))
    case = copy.deepcopy(staffed_sd)
    case["outputs"][0]["pool"] = "staffing"
    mutations.append(("staffed_sd_mixed_output_selectors", case))
    case = copy.deepcopy(staffed_sd)
    case["mode"] = "agent_pool"
    mutations.append(("plain_agent_pool_rejects_sd", case))
    case = copy.deepcopy(staffed_sd)
    case["des"] = copy.deepcopy(hybrid["des"])
    mutations.append(("staffed_sd_rejects_des_section", case))

    semantic_mutations = typed_semantic
    case = copy.deepcopy(adoption)
    case["sd"]["components"][0]["init"] = 1
    semantic_mutations.append(("nonzero_aggregate_stock", case))
    case = copy.deepcopy(adoption)
    case["sd"]["components"][2].update({
        "source": "adopted_stock",
        "destination": None,
        "expr": "adopted_stock * imitation",
        "unit": "person/day",
    })
    semantic_mutations.append(("aggregate_stock_flow_endpoint", case))
    case = copy.deepcopy(agent_pool)
    case["agent_pool"]["bridge"]["unit"] = "person"
    semantic_mutations.append(("agent_pool_bridge_unit_mismatch", case))
    case = copy.deepcopy(agent_pool)
    case["agent_pool"]["schedule"][2]["time"] = 0.5
    semantic_mutations.append(("agent_pool_unordered_schedule", case))

    semantic_diagnostics = []
    for chance in (0, .35, 1):
        case = copy.deepcopy(probability)
        case["components"][2]["probability"]["match"] = chance
        case["components"][0]["schedule"][0]["id"] = 1 << 48
        semantic_diagnostics.append((f"routing_id_width_{chance}", case, "IR_DES", "/components/0/schedule/0/id"))
        case = copy.deepcopy(probability)
        case["components"][2]["probability"]["match"] = chance
        del case["links"][2]
        semantic_diagnostics.append((f"routing_required_exit_{chance}", case, "IR_LINK", "/links"))
    for stream in (12, 13):
        case = copy.deepcopy(probability)
        case["components"][0] = dict(id="arrivals", kind="source", exponential=dict(
            count=32, arrival_rate=.5, service_rate=1, start=0, first_id=0, stream=12))
        case["components"][2]["probability"]["stream"] = stream
        semantic_diagnostics.append((f"routing_source_overlap_{stream}", case, "IR_DES", "/components/2/probability/stream"))
    for index, pointer in ((1, "/components/2/probability/stream"), (3, "/components/3/service/stream")):
        case = copy.deepcopy(probability)
        case["components"][index]["service"] = dict(kind="exponential", rate=1, stream=301)
        semantic_diagnostics.append((f"routing_service_overlap_{index}", case, "IR_DES", pointer))
    case = copy.deepcopy(independent)
    case["components"][2]["service"]["stream"] = 201
    semantic_diagnostics.append(("service_stream_reuse", case, "IR_DES", "/components/2/service/stream"))
    case = copy.deepcopy(independent)
    case["components"][0]["schedule"][0]["id"] = 1 << 48
    semantic_diagnostics.append(("service_id_width", case, "IR_DES", "/components/0/schedule/0/id"))
    for stream in (12, 13):
        case = copy.deepcopy(stochastic)
        case["components"][1]["service"] = {"kind":"exponential", "rate":1, "stream":stream}
        semantic_diagnostics.append((f"service_source_overlap_{stream}", case, "IR_DES", "/components/1/service/stream"))
    for stream in (20, 21):
        case = copy.deepcopy(rate)
        case["des"]["components"][1]["service"] = {"kind":"exponential", "rate":1, "stream":stream}
        semantic_diagnostics.append((f"service_rate_overlap_{stream}", case, "IR_HYBRID", "/bridges/1/stream"))
    for name, index, destination in (("graph_cycle", 9, "triage"),
                                      ("graph_rejection_completion", 8, "done")):
        case = copy.deepcopy(graph)
        case["links"][index]["to"] = destination
        if index == 8:
            case["components"] = [c for c in case["components"] if c["id"] != "lost"]
            case["outputs"] = [o for o in case["outputs"] if o["component"] != "lost"]
        semantic_diagnostics.append((name, case, "IR_LINK", "/links"))
    case = copy.deepcopy(graph)
    case["links"].pop()
    semantic_diagnostics.append(("graph_missing_exit", case, "IR_LINK", "/links"))
    case = copy.deepcopy(graph)
    case["links"].append(copy.deepcopy(case["links"][1]))
    semantic_diagnostics.append(("graph_duplicate_exit", case, "IR_LINK", "/links/11"))
    case = copy.deepcopy(graph)
    case["links"][0]["port"] = "rejected"
    semantic_diagnostics.append(("graph_source_rejection", case, "IR_LINK", "/links/0"))
    offgrid_inputs = json.loads((MODELS / 'offgrid_inputs.ir.json').read_text())
    case=copy.deepcopy(offgrid_inputs)
    case['integrator']='rk4'
    semantic_diagnostics.append(('next_tick_rk4',case,'IR_INTEGRATOR','/components/1/expr'))
    case=copy.deepcopy(offgrid_inputs)
    case['components'][1]['expr']='XMILE_NEXT_PULSE(quantity,first,period,quantity)'
    semantic_diagnostics.append(('next_tick_origin_unit',case,'IR_UNIT','/components/1/expr'))
    for name in ('XMILE_NEXT_STEP','XMILE_NEXT_RAMP','XMILE_NEXT_PULSE'):
        case=copy.deepcopy(offgrid_inputs)
        case['parameters'][0]['id']=name
        semantic_diagnostics.append(('reserved_'+name,case,'IR_ID','/parameters/0/id'))
    xmile_inputs = json.loads((MODELS / 'xmile_inputs.ir.json').read_text())
    case = copy.deepcopy(xmile_inputs)
    case['integrator'] = 'rk4'
    semantic_diagnostics.append(('xmile_rk4_input', case, 'IR_INTEGRATOR', '/components/1/expr'))
    case = copy.deepcopy(xmile_inputs)
    case['components'][1]['expr'] = 'XMILE_PULSE(quantity,quantity,period)'
    semantic_diagnostics.append(('xmile_pulse_time_units', case, 'IR_UNIT', '/components/1/expr'))
    case = copy.deepcopy(xmile_inputs)
    case['outputs'][1]['unit'] = 'kg'
    semantic_diagnostics.append(('xmile_pulse_result_units', case, 'IR_UNIT', '/outputs/1/unit'))
    for name in ('XMILE_STEP', 'XMILE_RAMP', 'XMILE_PULSE'):
        case = copy.deepcopy(xmile_inputs)
        case['parameters'][0]['id'] = name
        semantic_diagnostics.append(('reserved_'+name, case, 'IR_ID', '/parameters/0/id'))
    for start, dt, horizon, method in [(1e20, 1, 2, 'euler'), (1e308, 1e308, 1e308, 'euler'),
                                       (2**53, 2, 4, 'rk4')]:
        case = copy.deepcopy(absolute_clock)
        case['time'].update(start=start, dt=dt, horizon=horizon)
        case['integrator'] = method
        if method == 'rk4':
            case['components'].pop()
            case['outputs'].pop()
        semantic_diagnostics.append(('collapsed_clock_'+str(start), case, 'IR_TIME', '/time/start'))
    case = copy.deepcopy(absolute_clock)
    case['time']['horizon'] = 1e-12
    semantic_diagnostics.append(('sub_tick_horizon', case, 'IR_TIME', '/time'))
    case = copy.deepcopy(absolute_clock)
    case['outputs'][1]['expr'] = 'NONNEGATIVE(slope)'
    semantic_diagnostics.append(('filter_output_units', case, 'IR_UNIT', '/outputs/1/unit'))
    case = copy.deepcopy(absolute_clock)
    case['components'][2]['duration'] = 'dt-t'
    semantic_diagnostics.append(('absolute_initial_duration', case, 'IR_DELAY', '/components/2/duration'))
    for order in (['missing'], ['high'], ['high', 'inflow']):
        case = copy.deepcopy(clipping)
        case['components'][0]['outflow_order'] = order
        semantic_diagnostics.append(('clipping_priority_'+str(len(semantic_diagnostics)),
                                     case, 'IR_CLIPPING', '/components'))
    case = copy.deepcopy(clipping)
    case['components'][3].pop('clip_negative')
    case['components'][3]['non_negative'] = False
    semantic_diagnostics.append(('clipping_signed_incident', case, 'IR_CLIPPING', '/components'))
    case = copy.deepcopy(clipping)
    case['components'][4]['destination'] = 'inventory'
    semantic_diagnostics.append(('clipping_cycle', case, 'IR_CLIPPING', '/components'))
    case = copy.deepcopy(decay)
    case['integrator'] = 'rk4'
    case['components'][1]['expr'] = 'STEP(decay_rate*material,1)'
    semantic_diagnostics.append(('rk4_tick_function', case, 'IR_INTEGRATOR', '/components/1/expr'))
    for expression, unit, code, pointer in [
        ("missing", "1", "IR_SYMBOL", "/outputs/0/expr"),
        ("rate*t", "day", "IR_UNIT", "/outputs/0/unit"),
        ("1+", "1", "IR_EXPR", "/outputs/0/expr"),
    ]:
        case = copy.deepcopy(expression_outputs)
        case["outputs"] = [{"id":"x", "expr":expression, "unit":unit}]
        semantic_diagnostics.append(("expression_output_"+expression, case, code, pointer))
    for expression, code in [("missing", "IR_SYMBOL"), ("intake", "IR_UNIT"),
                             ("material", "IR_DELAY_DURATION"), ("tau-tau", "IR_DELAY"),
                             ("tau+", "IR_EXPR")]:
        case = copy.deepcopy(variable_delay)
        case["components"][2]["duration"] = expression
        semantic_diagnostics.append((f"delay_duration_{expression}", case, code, "/components/2/duration"))

    for name, expression, code in [
        ("unknown_phase_symbol", "capacity + unknown", "IR_SYMBOL"),
        ("unknown_phase_function", "unknown(capacity)", "IR_FUNCTION"),
        ("bad_phase_expression", "capacity +", "IR_EXPR"),
        ("bad_phase_expression_unit", "active", "IR_UNIT"),
        ("mixed_phase_expression_units", "capacity + t", "IR_UNIT"),
    ]:
        case = copy.deepcopy(agent_phases)
        case["agent_pool"]["phases"][0]["capacity_expr"] = expression
        semantic_diagnostics.append((name, case, code, "/agent_pool/phases/0/capacity_expr"))
    case = copy.deepcopy(agent_phases)
    case["agent_pool"]["phases"][0]["unit"] = "person"
    semantic_diagnostics.append(("bad_phase_unit", case, "IR_UNIT", "/agent_pool/phases/0/unit"))
    case = copy.deepcopy(agent_phases)
    case["agent_pool"]["phases"][1]["id"] = "balance_free"
    semantic_diagnostics.append(("duplicate_phase_id", case, "IR_ID", "/agent_pool/phases/1/id"))
    case = copy.deepcopy(agent_phases)
    case["agent_pool"]["schedule"][1]["phases"][0] = "missing"
    semantic_diagnostics.append(("unknown_phase_id", case, "IR_REF", "/agent_pool/schedule/1/phases/0"))
    case = copy.deepcopy(delivery)
    case["agent_pool"]["delivery"]["time_unit"] = "hour"
    semantic_diagnostics.append(("delivery_time_unit", case, "IR_UNIT", "/agent_pool/delivery/time_unit"))
    case = copy.deepcopy(delivery)
    case["agent_pool"]["schedule"][0]["engagements"][1]["request_id"] = 10
    semantic_diagnostics.append(("delivery_duplicate_id", case, "IR_DELIVERY", "/agent_pool/schedule/0/engagements/1"))
    case = copy.deepcopy(delivery)
    case["agent_pool"]["schedule"][1]["time"] = 0
    semantic_diagnostics.append(("delivery_duplicate_time", case, "IR_DELIVERY", "/agent_pool/schedule/1/time"))
    case = copy.deepcopy(delivery)
    case["agent_pool"]["schedule"][0]["engagements"][0]["units"] = 3
    semantic_diagnostics.append(("delivery_excess_units", case, "IR_TYPE", "/agent_pool/schedule/0/engagements/0/units"))
    case = copy.deepcopy(delivery)
    case["outputs"][2]["process"] = "missing"
    semantic_diagnostics.append(("delivery_missing_process", case, "IR_OUTPUT", "/outputs/2"))

    for field, invalid, code in [
        ("from", "staffing", "IR_REF"),
        ("to", "missing", "IR_REF"),
        ("unit", "person", "IR_UNIT"),
    ]:
        case = copy.deepcopy(staffed_sd)
        case["bridges"][0][field] = invalid
        semantic_diagnostics.append((f"staffed_sd_bridge_{field}", case, code, f"/bridges/0/{field}"))
    case = copy.deepcopy(staffed_sd)
    case["sd"]["parameters"].append({"id": "cost_per_day_squared", "value": 20, "unit": "USD/day/day"})
    case["sd"]["components"][3]["expr"] = "cost_per_day_squared * dt"
    semantic_diagnostics.append(("staffed_sd_dt_expression", case, "IR_HYBRID", "/sd/components/3/expr"))
    case = copy.deepcopy(staffed_sd)
    case["sd"]["components"][1]["id"] = "consultants"
    case["sd"]["components"][3]["destination"] = "consultants"
    semantic_diagnostics.append(("staffed_sd_cross_section_id", case, "IR_ID", "/agent_pool/population/id"))
    case = copy.deepcopy(staffed_sd)
    case["outputs"][0]["stock"] = "missing"
    semantic_diagnostics.append(("staffed_sd_missing_output_stock", case, "IR_OUTPUT", "/outputs/0/stock"))
    case = copy.deepcopy(staffed_sd)
    case["outputs"][1]["id"] = "revenue"
    semantic_diagnostics.append(("staffed_sd_duplicate_output", case, "IR_ID", "/outputs/1/id"))

    with tempfile.TemporaryDirectory() as directory:
        for name, instance in mutations:
            path = Path(directory) / f"{name}.json"
            path.write_text(json.dumps(instance))
            if validator.is_valid(instance):
                raise AssertionError(f"schema accepted invalid {name}")
            if lint(executable, path):
                raise AssertionError(f"loader accepted invalid {name}")

        for name, instance in semantic_mutations:
            path = Path(directory) / f"{name}.json"
            path.write_text(json.dumps(instance))
            validator.validate(instance)
            if lint(executable, path):
                raise AssertionError(f"loader accepted semantically invalid {name}")

        for name, instance, code, pointer in semantic_diagnostics:
            validator.validate(instance)
            path = Path(directory) / f"{name}.json"
            path.write_text(json.dumps(instance))
            result = subprocess.run([str(executable), "lint", str(path)],
                                    capture_output=True, text=True, check=False)
            if result.returncode == 0:
                raise AssertionError(f"loader accepted invalid {name}")
            diagnostic = json.loads(result.stderr)["diagnostics"][0]
            if diagnostic["code"] != code or diagnostic["pointer"] != pointer:
                raise AssertionError(f"wrong diagnostic for {name}: {diagnostic}")

    print(
        f"schema contract: {len(valid_paths)} valid fixtures plus {len(interaction_cases)} interaction and {len(generator_cases)} generator cases, "
        f"{len(mutations)} structural and "
        f"{len(semantic_mutations) + len(semantic_diagnostics)} semantic invalid mutations"
    )


if __name__ == "__main__":
    main()
