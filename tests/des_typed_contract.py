"""Declarative typed process graph: hand oracle, ordering and rejection contracts."""
import copy
import csv
import io
import json
import math
from pathlib import Path
import random
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def philox_word(seed, scenario, replication, identity, stream):
    """Integer reference implementation, independent of the native draw call."""
    mask = (1 << 32) - 1
    counter = [identity & mask, (identity >> 32) | (stream << 16), scenario | (replication << 16), 0]
    key = [seed & mask, seed >> 32]
    for _ in range(10):
        a, b = 0xD2511F53 * counter[0], 0xCD9E8D57 * counter[2]
        counter = [(b >> 32) ^ counter[1] ^ key[0], b & mask,
                   (a >> 32) ^ counter[3] ^ key[1], a & mask]
        key = [(key[0] + 0x9E3779B9) & mask, (key[1] + 0xBB67AE85) & mask]
    return counter[0]


def main():
    executable = str(Path(sys.argv[1]).resolve())
    fixture = json.loads((ROOT / "models/typed_resource_process.ir.json").read_text())
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "model.json"

        def call(model, command="run", success=True):
            path.write_text(json.dumps(model))
            result = subprocess.run([executable, command, str(path)], capture_output=True, text=True)
            if success:
                assert result.returncode == 0, result.stderr
            else:
                assert result.returncode != 0, "invalid typed model succeeded"
                diagnostic = json.loads(result.stderr)
                assert diagnostic["diagnostics"][0]["code"].startswith("IR_"), diagnostic
                return diagnostic
            if command == "lint":
                return json.loads(result.stdout)
            return {(float(row["time"]), row["output_id"]): float(row["value"])
                    for row in csv.DictReader(io.StringIO(result.stdout))}

        baseline = call(fixture)
        for index in range(13):
            time = index / 2
            completed = int(time >= 1) + int(time >= 3) + int(time >= 6)
            cycle = (1 if time >= 1 else 0) + (2.5 if time >= 3 else 0) + (6 if time >= 6 else 0)
            queue_area = min(time, 3) + max(0, min(time, 1) - .5)
            assert baseline[time, "completed"] == completed
            assert baseline[time, "cycle_total"] == cycle
            assert baseline[time, "emitted"] == (2 if time < .5 else 3)
            assert baseline[time, "allocated"] == int(time < 6)
            assert baseline[time, "in_process"] == int(time < 6)
            assert baseline[time, "waiting"] == (1 if time < .5 else 2 if time < 1 else 1 if time < 3 else 0)
            assert math.isclose(baseline[time, "queue_mean"], queue_area / time if time else 0)
            assert baseline[time, "utilization"] == (1 if time else 0)
        generator = random.Random(20260925)
        for _ in range(24):
            model = copy.deepcopy(fixture)
            generator.shuffle(model["components"])
            generator.shuffle(model["links"])
            generator.shuffle(model["outputs"])
            assert call(model) == baseline, "declaration order changed typed process"
        dense = copy.deepcopy(fixture)
        dense["time"]["dt"] = .25
        dense_rows = call(dense)
        assert all(dense_rows[key] == value for key, value in baseline.items())
        admission_priority = copy.deepcopy(fixture)
        admission_priority["components"][1].pop("priority")
        admission_priority["components"][3]["priority"] = "rank"
        assert call(admission_priority) == baseline
        for expression in ("rank / 2", "2147483648"):
            invalid = copy.deepcopy(admission_priority)
            invalid["components"][3]["priority"] = expression
            call(invalid,"run",False)

        def mutate(edit, command="lint"):
            model = copy.deepcopy(fixture)
            edit(model)
            return call(model, command, False)

        mutate(lambda m: m["components"][1]["schedule"][0]["values"].update(rank=1.5))
        mutate(lambda m: m["components"][1]["schedule"][0]["values"].update(urgent=1))
        mutate(lambda m: m["components"][1]["schedule"][0]["values"].update(extra=1))
        mutate(lambda m: m["components"][1]["schedule"][0]["values"].pop("label"))
        mutate(lambda m: m["components"][0]["fields"][0].update(unit="currency"))
        mutate(lambda m: m["components"][3].update(preempt=True))
        mutate(lambda m: m["components"][3].update(pool="missing"))
        mutate(lambda m: m["links"][-1].update(to="claim"))
        mutate(lambda m: m["links"][2].update(to="done"))
        mutate(lambda m: m["links"].append({"from":"delivery", "to":"done"}))
        mutate(lambda m: m["components"][4].update(duration="urgent"))
        mutate(lambda m: m["components"][4].update(duration="1"))
        mutate(lambda m: m["components"][1].update(priority="duration"))
        mutate(lambda m: m["components"][1]["schedule"][0]["values"].update(rank=9007199254740993), "run")
        mutate(lambda m: m["parameters"][0].update(value=0), "run")
        mutate(lambda m: m["components"][3].update(units="2"), "run")
        mutate(lambda m: m["outputs"][0].update(metric="missing"))
        queue = json.loads((ROOT / "models/typed_queue_process.ir.json").read_text())
        actual = call(queue)
        assert actual[4, "completed"] == 3 and actual[4, "lost_count"] == 2
        assert actual[4, "cycle_total"] == 5 and actual[4, "wait_total"] == 2
        assert actual[4, "queue_mean"] == .5 and actual[4, "utilization"] == .75
        # Direct bounded delay has explicit loss routing, without an input queue.
        direct = copy.deepcopy(queue)
        direct["components"].pop(2)
        direct["links"] = [{"from":"arrivals", "to":"delivery"},
                           {"from":"delivery", "to":"done"},
                           {"from":"delivery", "port":"rejected", "to":"lost"}]
        direct["outputs"] = [o for o in direct["outputs"] if o["component"] != "waiting_room"]
        actual = call(direct)
        assert actual[4, "completed"] == 2 and actual[4, "lost_count"] == 3
        assert actual[4, "cycle_total"] == 2
        # A second entity namespace shares the pool, including colliding local entity IDs.
        shared = copy.deepcopy(fixture)
        shared["time"]["horizon"] = 7
        names = {"work":"work_b", "arrivals":"arrivals_b", "claim":"claim_b",
                 "delivery":"delivery_b", "give_back":"give_back_b", "done":"done_b"}
        for node in fixture["components"]:
            if node["id"] not in names:
                continue
            node = copy.deepcopy(node)
            node["id"] = names[node["id"]]
            if "entity_type" in node:
                node["entity_type"] = "work_b"
            if node["kind"] == "source":
                node["schedule"] = [{"arrival":0,"values":{"duration":.5,"rank":-2,"urgent":True,"label":"other"}}]
            shared["components"].append(node)
        shared["links"] += [{"from":names[link["from"]],"to":names[link["to"]]} for link in fixture["links"]]
        shared["outputs"] += [{"id":"completed_b","component":"done_b","metric":"completed"},
                              {"id":"cycle_b","component":"done_b","metric":"cycle_total"}]
        actual = call(shared)
        assert actual[.5,"completed_b"] == 1 and actual[7,"cycle_b"] == .5
        assert actual[2.5,"completed"] == 1 and actual[3.5,"completed"] == 2 and actual[6.5,"completed"] == 3
        assert actual[7,"cycle_total"] == 12 and math.isclose(actual[7,"queue_mean"],6/7)
        assert math.isclose(actual[7,"utilization"],6.5/7)
        for _ in range(8):
            generator.shuffle(shared["components"])
            generator.shuffle(shared["links"])
            assert call(shared) == actual

        routing = json.loads((ROOT / "models/typed_routing_process.ir.json").read_text())
        actual = call(routing)
        fast = 0
        for identity in range(32):
            fast += philox_word(0,0,0,identity,500) < (1 << 30)
            time = identity * .125
            assert actual[time,"fast_count"] == fast
            assert actual[time,"unused_count"] == 0
            assert actual[time,"regular_count"] == identity + 1 - fast
        assert actual[6,"completed"] == 32
        conditional = copy.deepcopy(routing)
        choice = conditional["components"][2]
        choice.pop("stream")
        choice["branches"] = [
            {"port":"fast", "condition":{"left":"rank","op":"lt","right":"0"}},
            {"port":"unused", "condition":{"left":"rank","op":"eq","right":"0"}}]
        choice["otherwise"] = "regular"
        actual = call(conditional)
        counts = [0,0,0]
        for identity in range(32):
            rank = identity % 5 - 2
            counts[0 if rank < 0 else 1 if rank == 0 else 2] += 1
            for port, count in zip(("fast","unused","regular"),counts):
                assert actual[identity*.125,port+"_count"] == count
        for invalid in ("lt_bad",):
            bad = copy.deepcopy(conditional)
            bad["components"][2]["branches"][0]["condition"]["op"] = invalid
            call(bad,"lint",False)
        for base in (routing, conditional, queue):
            baseline_rows = call(base)
            for _ in range(8):
                changed = copy.deepcopy(base)
                generator.shuffle(changed["components"])
                generator.shuffle(changed["links"])
                assert call(changed) == baseline_rows
        # Experiment addresses and overrides use the same general run path.
        experiment = Path(directory) / "experiment.json"
        settings = {"seed":987654321,"replications":3,"scenarios":[{"id":17,"parameters":{}},{"id":18,"parameters":{}}]}
        experiment.write_text(json.dumps(settings))
        path.write_text(json.dumps(routing))
        args = [executable,"run",str(path),"--experiment",str(experiment)]
        first = subprocess.run(args,capture_output=True,text=True)
        replay = subprocess.run(args,capture_output=True,text=True)
        assert first.returncode == replay.returncode == 0 and first.stdout == replay.stdout
        for row in csv.DictReader(io.StringIO(first.stdout)):
            if row["output_id"] == "fast_count" and float(row["time"]) == 6:
                expected = sum(philox_word(settings["seed"],int(row["scenario"]),int(row["replication"]),i,500) < (1 << 30)
                               for i in range(32))
                assert float(row["value"]) == expected
        settings = {"seed":1,"replications":1,"scenarios":[{"id":0,"parameters":{"speed":2}}]}
        experiment.write_text(json.dumps(settings))
        path.write_text(json.dumps(fixture))
        result = subprocess.run(args,capture_output=True,text=True)
        assert result.returncode == 0,result.stderr
        final = {r["output_id"]:float(r["value"]) for r in csv.DictReader(io.StringIO(result.stdout)) if float(r["time"])==6}
        assert final["completed"] == 3 and final["cycle_total"] == 4.5 and final["utilization"] == .5
        generated = copy.deepcopy(fixture)
        generated["parameters"].append({"id":"gap","unit":"day","value":.5})
        source = generated["components"][1]
        source.pop("schedule")
        source["generator"] = {"count":3,"start":.5,"values":{"duration":1,"rank":0,"urgent":False,"label":"generated"},
                               "interarrival":{"kind":"constant","interval":"gap"}}
        actual = call(generated)
        explicit = copy.deepcopy(generated)
        template = explicit["components"][1].pop("generator")["values"]
        explicit["components"][1]["schedule"] = [{"arrival":1+i*.5,"values":template} for i in range(3)]
        assert actual == call(explicit)
        assert actual[6,"cycle_total"] == 4.5 and actual[6,"completed"] == 3
        generated["components"][1]["generator"]["count"] = 0
        assert call(generated)[6,"emitted"] == 0
        generated["parameters"][-1]["value"] = 0
        call(generated,"run",False)

        capacity = copy.deepcopy(fixture)
        capacity["time"]["horizon"] = 8
        capacity["parameters"].append({"id":"initial_staff","unit":"1","value":0})
        capacity["components"][2]["capacity"] = "initial_staff"
        capacity["components"][2]["schedule"] = [{"time":1,"capacity":"1"}]
        actual = call(capacity)
        assert actual[0,"allocated"] == 0 and actual[.5,"waiting"] == 3
        assert actual[8,"cycle_total"] == 13.5 and actual[8,"completed"] == 3
        assert math.isclose(actual[8,"utilization"],6/7) and actual[8,"queue_mean"] == 7.5/8
        for expression in ("-1", "0.5", "1000001"):
            bad = copy.deepcopy(capacity)
            bad["components"][2]["capacity"] = expression
            call(bad,"run",False)
        bad = copy.deepcopy(fixture)
        bad["components"][2]["schedule"] = [{"time":1,"capacity":0}]
        call(bad,"run",False)  # Release has not reached the pool in the first microstep at t=1.

        # Compare generated arrivals and independent service against the legacy native path.
        stochastic = copy.deepcopy(queue)
        stochastic["time"] = {"unit":"day","dt":.5,"horizon":80}
        stochastic["parameters"] = [{"id":"arrival_rate","unit":"1/day","value":.7},
                                    {"id":"service_rate","unit":"1/day","value":1.2}]
        stochastic["components"][1].pop("schedule")
        stochastic["components"][1]["generator"] = {"count":64,"values":{"duration":1,"rank":0},
            "interarrival":{"kind":"exponential","rate":"arrival_rate","stream":100}}
        stochastic["components"][2].pop("capacity")
        stochastic["components"][3]["duration"] = {"kind":"exponential","rate":"service_rate","stream":101}
        stochastic["components"].pop()  # Unbounded queue has no lost sink.
        stochastic["links"].pop(2)
        metrics = [("emitted","arrivals","emitted"),("completed","done","completed"),
                   ("cycle_total","done","cycle_total"),("waiting","delivery","waiting"),
                   ("queue_mean","delivery","queue_mean"),("utilization","delivery","utilization")]
        legacy = {"ir_version":"0.1","name":"legacy_generated","mode":"des","time":stochastic["time"],
                  "components":[{"id":"arrivals","kind":"source","exponential":{"count":64,"arrival_rate":.7,
                                  "service_rate":1.2,"start":0,"first_id":0,"stream":100}},
                                {"id":"delivery","kind":"server","capacity":1},{"id":"done","kind":"sink"}],
                  "links":[{"from":"arrivals","to":"delivery"},{"from":"delivery","to":"done"}],
                  "outputs":[{"id":key,"component":node,"metric":metric} for key,node,metric in metrics]}
        stochastic["outputs"] = copy.deepcopy(legacy["outputs"])
        for output in stochastic["outputs"]:
            if output["metric"] in ("waiting","queue_mean"):
                output["component"] = "waiting_room"
        for slots in (1,2):
            stochastic["components"][3]["capacity"] = slots
            legacy["components"][1]["capacity"] = slots
            actual, expected = call(stochastic),call(legacy)
            assert actual.keys() == expected.keys()
            assert all(math.isclose(actual[key],value,rel_tol=1e-12,abs_tol=1e-12) for key,value in expected.items())
        bad = copy.deepcopy(stochastic)
        bad["components"][3]["duration"]["stream"] = 100
        call(bad,"lint",False)
        for index in (0,1):
            bad = copy.deepcopy(stochastic)
            bad["parameters"][index]["value"] = 0
            call(bad,"run",False)
        empty = copy.deepcopy(stochastic)
        empty["components"][1]["generator"]["count"] = 0
        assert call(empty)[80,"emitted"] == 0
        empty["parameters"][0]["value"] = 0
        call(empty,"run",False)
        settings = {"seed":314159,"replications":2,"scenarios":[{"id":13,"parameters":{}}]}
        experiment.write_text(json.dumps(settings))
        path.write_text(json.dumps(stochastic))
        typed_result = subprocess.run(args,capture_output=True,text=True)
        path.write_text(json.dumps(legacy))
        legacy_result = subprocess.run(args,capture_output=True,text=True)
        assert typed_result.returncode == legacy_result.returncode == 0,(typed_result.stderr,legacy_result.stderr)
        typed_rows = list(csv.DictReader(io.StringIO(typed_result.stdout)))
        legacy_rows = list(csv.DictReader(io.StringIO(legacy_result.stdout)))
        assert len(typed_rows) == len(legacy_rows)
        for a,b in zip(typed_rows,legacy_rows):
            assert all(a[k]==b[k] for k in ("scenario","replication","time","output_id"))
            assert math.isclose(float(a["value"]),float(b["value"]),rel_tol=1e-12,abs_tol=1e-12)
        capacity_fixture = json.loads((ROOT / "models/typed_capacity_process.ir.json").read_text())
        final_capacity = call(capacity_fixture)
        assert final_capacity[6,"completed"] == 3 and final_capacity[6,"utilization"] == .75
        assert final_capacity[6,"available_staff"] == 0
    print("Typed declarative resource hand trajectory, 24 declaration permutations, sampling and invalid models passed")


if __name__ == "__main__":
    main()
