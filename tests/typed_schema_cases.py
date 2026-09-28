"""Typed process structure and semantic mutations for the independent schema gate."""
import copy
import json


def cases(models):
    resource = json.loads((models / "typed_resource_process.ir.json").read_text())
    queue = json.loads((models / "typed_queue_process.ir.json").read_text())
    routing = json.loads((models / "typed_routing_process.ir.json").read_text())
    generated = json.loads((models / "typed_stochastic_process.ir.json").read_text())
    capacity = json.loads((models / "typed_capacity_process.ir.json").read_text())
    structural, semantic = [], []

    def add(target, name, base, edit):
        instance = copy.deepcopy(base)
        edit(instance)
        target.append(("typed_"+name, instance))

    for value in (True, -1, 1000001, .5, None):
        add(structural,"capacity_"+str(value),resource,lambda m,v=value:m["components"][2].update(capacity=v))
    for value in (0, True, -1, .5, None):
        add(structural,"max_units_"+str(value),resource,lambda m,v=value:m["components"][2].update(max_request_units=v))
    for value in (True, 0, None):
        add(structural,"preempt_"+str(value),resource,lambda m,v=value:m["components"][3].update(preempt=v))
    for value in ("vector",None,True):
        add(structural,"field_kind_"+str(value),resource,lambda m,v=value:m["components"][0]["fields"][0].update(type=v))
    add(structural,"missing_field_unit",resource,lambda m:m["components"][0]["fields"][0].pop("unit"))
    add(structural,"boolean_unit",resource,lambda m:m["components"][0]["fields"][2].update(unit="1"))
    add(structural,"missing_type",resource,lambda m:m["components"][1].pop("entity_type"))
    add(structural,"source_unknown",resource,lambda m:m["components"][1].update(exponential={}))
    add(structural,"source_bad_values",resource,lambda m:m["components"][1]["schedule"][0].update(values=[]))
    add(structural,"source_bad_value",resource,lambda m:m["components"][1]["schedule"][0]["values"].update(rank=[]))
    add(structural,"source_negative_arrival",resource,lambda m:m["components"][1]["schedule"][0].update(arrival=-1))
    add(structural,"missing_duration",resource,lambda m:m["components"][4].pop("duration"))
    add(structural,"numeric_duration",resource,lambda m:m["components"][4].update(duration=1))
    add(structural,"zero_delay_capacity",queue,lambda m:m["components"][3].update(capacity=0))
    add(structural,"queue_bad_discipline",queue,lambda m:m["components"][2].update(discipline="random"))
    for value in (-1,65536,True,.5,None):
        add(structural,"stream_"+str(value),routing,lambda m,v=value:m["components"][2].update(stream=v))
    for value in (-.1,1.1,True,None):
        add(structural,"probability_"+str(value),routing,lambda m,v=value:m["components"][2]["branches"][0].update(probability=v))
    add(structural,"mixed_selection",routing,lambda m:m["components"][2].update(otherwise="other"))
    add(structural,"missing_branches",routing,lambda m:m["components"][2].pop("branches"))
    add(structural,"empty_branches",routing,lambda m:m["components"][2].update(branches=[]))
    add(structural,"missing_metric_port",routing,lambda m:m["outputs"][0].pop("port"))
    add(structural,"extraneous_metric_port",resource,lambda m:m["outputs"][0].update(port="out"))
    add(structural,"legacy_mix",resource,lambda m:m["components"].append({"id":"old_server","kind":"server","capacity":1}))
    add(structural,"queue_priority_fifo",queue,lambda m:m["components"][2].update(priority="rank"))
    add(structural,"seize_priority_type",resource,lambda m:m["components"][3].update(priority=1))
    for value in (-1,1000001,True,.5,None):
        add(structural,"generator_count_"+str(value),generated,lambda m,v=value:m["components"][1]["generator"].update(count=v))
    for value in (-1,True,None):
        add(structural,"generator_start_"+str(value),generated,lambda m,v=value:m["components"][1]["generator"].update(start=v))
    add(structural,"source_two_inputs",generated,lambda m:m["components"][1].update(schedule=[]))
    add(structural,"source_no_inputs",resource,lambda m:m["components"][1].pop("schedule"))
    add(structural,"interarrival_kind",generated,lambda m:m["components"][1]["generator"]["interarrival"].update(kind="normal"))
    add(structural,"interarrival_missing_stream",generated,lambda m:m["components"][1]["generator"]["interarrival"].pop("stream"))
    add(structural,"interarrival_missing_rate",generated,lambda m:m["components"][1]["generator"]["interarrival"].pop("rate"))
    add(structural,"interarrival_bad_stream",generated,lambda m:m["components"][1]["generator"]["interarrival"].update(stream=65536))
    add(structural,"duration_policy_kind",generated,lambda m:m["components"][3]["duration"].update(kind="normal"))
    add(structural,"duration_policy_missing_stream",generated,lambda m:m["components"][3]["duration"].pop("stream"))
    add(structural,"duration_policy_extra",generated,lambda m:m["components"][3]["duration"].update(extra=1))
    add(structural,"capacity_schedule_shape",capacity,lambda m:m["components"][2].update(schedule={}))
    add(structural,"capacity_schedule_missing",capacity,lambda m:m["components"][2]["schedule"][0].pop("capacity"))
    add(structural,"capacity_schedule_negative",capacity,lambda m:m["components"][2]["schedule"][0].update(time=-1))

    add(semantic,"field_type_value",resource,lambda m:m["components"][1]["schedule"][0]["values"].update(rank=1.5))
    add(semantic,"field_bool_value",resource,lambda m:m["components"][1]["schedule"][0]["values"].update(urgent=1))
    add(semantic,"record_missing",resource,lambda m:m["components"][1]["schedule"][0]["values"].pop("label"))
    add(semantic,"record_extra",resource,lambda m:m["components"][1]["schedule"][0]["values"].update(extra=0))
    add(semantic,"field_duplicate",resource,lambda m:m["components"][0]["fields"][1].update(id="duration"))
    add(semantic,"field_shadows_parameter",resource,lambda m:m["components"][0]["fields"][0].update(id="speed"))
    add(semantic,"field_unit",resource,lambda m:m["components"][0]["fields"][0].update(unit="currency"))
    add(semantic,"unknown_type",resource,lambda m:m["components"][1].update(entity_type="missing"))
    add(semantic,"unknown_pool",resource,lambda m:m["components"][3].update(pool="missing"))
    add(semantic,"retained_lease",resource,lambda m:m["links"][2].update(to="done"))
    add(semantic,"cycle",resource,lambda m:m["links"][-1].update(to="claim"))
    add(semantic,"fanout",resource,lambda m:m["links"].append({"from":"delivery","to":"done"}))
    add(semantic,"missing_exit",resource,lambda m:m["links"].pop())
    add(semantic,"time_expression",resource,lambda m:m["components"][4].update(duration="t"))
    add(semantic,"bool_expression",resource,lambda m:m["components"][4].update(duration="urgent"))
    add(semantic,"priority_unit",resource,lambda m:m["components"][1].update(priority="duration"))
    add(semantic,"duration_unit",resource,lambda m:m["components"][4].update(duration="1"))
    add(semantic,"unbounded_utilization",resource,lambda m:m["outputs"][8].update(metric="utilization"))
    add(semantic,"wrong_metric",resource,lambda m:m["outputs"][0].update(metric="completed"))
    add(semantic,"probability_sum",routing,lambda m:m["components"][2]["branches"][0].update(probability=.5))
    add(semantic,"duplicate_port",routing,lambda m:m["components"][2]["branches"][1].update(port="fast"))
    add(semantic,"unknown_port",routing,lambda m:m["links"][1].update(port="other"))
    add(semantic,"unknown_metric_port",routing,lambda m:m["outputs"][0].update(port="other"))
    add(semantic,"queue_missing_rejection",queue,lambda m:m["links"].pop(2))
    add(semantic,"unbounded_queue_target",queue,lambda m:m["components"][3].pop("capacity"))
    add(semantic,"queue_credit_bypass",queue,lambda m:m["links"][0].update(to="delivery"))
    add(semantic,"seize_priority_unit",resource,lambda m:m["components"][3].update(priority="duration"))
    add(semantic,"capacity_expression_unit",capacity,lambda m:m["components"][2].update(capacity="gap"))
    add(semantic,"capacity_schedule_unit",capacity,lambda m:m["components"][2]["schedule"][0].update(capacity="gap"))
    add(semantic,"capacity_schedule_duplicate",capacity,lambda m:m["components"][2]["schedule"][1].update(time=.5))
    add(semantic,"capacity_schedule_horizon",capacity,lambda m:m["components"][2]["schedule"][1].update(time=7))
    add(semantic,"source_rate_unit",generated,lambda m:m["parameters"][0].update(unit="day"))
    add(semantic,"service_rate_unit",generated,lambda m:m["parameters"][1].update(unit="day"))
    add(semantic,"process_stream_overlap",generated,lambda m:m["components"][3]["duration"].update(stream=100))
    add(semantic,"interarrival_entity_symbol",generated,lambda m:m["components"][1]["generator"]["interarrival"].update(rate="rank"))
    add(semantic,"generator_record_type",generated,lambda m:m["components"][1]["generator"]["values"].update(rank=1.5))
    return structural, semantic
