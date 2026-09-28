"""Score declarative process observations against pinned independent entity histories."""
import csv
import io
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT / "tests/oracles/des"))
from discipline_oracles import compare


def model_for(spec, resource, horizon):
    components = [
        {"id":"work","kind":"entity_type","fields":[
            {"id":"duration","type":"real","unit":"day"}, {"id":"rank","type":"integer","unit":"1"}]},
        {"id":"arrivals","kind":"source","entity_type":"work","priority":"rank","schedule":[
            {"arrival":job["arrival"],"values":{"duration":job["service"],"rank":job["priority"]}} for job in spec["jobs"]]},
        {"id":"delivery","kind":"delay_block","entity_type":"work","duration":"duration"},
        {"id":"done","kind":"sink","entity_type":"work"}]
    outputs = [("emitted","arrivals","emitted"),("completed","done","completed"),("cycle_total","done","cycle_total"),
               ("active","delivery","active"),("busy_mean","delivery","in_process_mean")]
    if resource:
        components += [{"id":"staff","kind":"resource_pool","capacity":spec["capacity"],"max_request_units":1,"discipline":spec["discipline"]},
                       {"id":"claim","kind":"seize","entity_type":"work","pool":"staff","units":"1"},
                       {"id":"give_back","kind":"release","entity_type":"work","pool":"staff"}]
        links = [{"from":"arrivals","to":"claim"},{"from":"claim","to":"delivery"},
                 {"from":"delivery","to":"give_back"},{"from":"give_back","to":"done"}]
        outputs += [("waiting","staff","waiting"),("queue_mean","staff","queue_mean"),("utilization","staff","utilization"),
                    ("released","claim","granted")]
    else:
        components[2]["capacity"] = spec["capacity"]
        queue = {"id":"waiting_room","kind":"queue","entity_type":"work","discipline":spec["discipline"]}
        if spec["queue_capacity"] is not None:
            queue["capacity"] = spec["queue_capacity"]
        components.append(queue)
        links = [{"from":"arrivals","to":"waiting_room"},{"from":"waiting_room","to":"delivery"},{"from":"delivery","to":"done"}]
        if spec["queue_capacity"] is not None:
            components.append({"id":"lost","kind":"sink","entity_type":"work"})
            links.append({"from":"waiting_room","port":"rejected","to":"lost"})
            outputs.append(("lost_count","lost","completed"))
        outputs += [("waiting","waiting_room","waiting"),("queue_mean","waiting_room","queue_mean"),
                    ("utilization","delivery","utilization"),("released","waiting_room","released"),
                    ("wait_total","waiting_room","wait_total")]
    if spec["discipline"] == "priority":
        components[1].pop("priority")
        next(node for node in components if node["id"] == ("claim" if resource else "waiting_room"))["priority"] = "rank"
    return {"ir_version":"0.1","name":"typed_"+spec["name"],"mode":"des","time":{"unit":"day","dt":.125,"horizon":horizon},
            "components":components,"links":links,"outputs":[{"id":key,"component":node,"metric":metric} for key,node,metric in outputs]}


def main():
    executable = str(Path(sys.argv[1]).resolve())
    reference = json.loads((ROOT / "tests/oracles/des/discipline-reference.json").read_text())
    compare(reference,{"version":1,"cases":reference["cases"]})
    checked = 0
    largest = 0
    reports = []
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "model.json"
        for resource in (False, True):
            for case in reference["cases"]:
                spec = case["spec"]
                if resource and spec["queue_capacity"] is not None:
                    continue
                records, rejected = case["records"], case["rejected"]
                horizon = math.ceil(max(record[4] for record in records)) + 1
                model = model_for(spec, resource, horizon)
                path.write_text(json.dumps(model))
                result = subprocess.run([executable,"run",str(path)],capture_output=True,text=True)
                assert result.returncode == 0, (spec["name"],resource,result.stderr)
                actual = {(float(row["time"]),row["output_id"]):float(row["value"])
                          for row in csv.DictReader(io.StringIO(result.stdout))}
                expected = {}
                for tick in range(horizon*8+1):
                    time = tick/8
                    due = [r for r in records if r[4] <= time]
                    started = [r for r in records if r[3] <= time]
                    busy_area = sum(max(0,min(time,r[4])-r[3]) for r in records)
                    queue_area = sum(max(0,min(time,r[3])-r[2]) for r in records)
                    values = {
                        "emitted":sum(job["arrival"]<=time for job in spec["jobs"]),
                        "completed":len(due), "cycle_total":sum(r[4]-r[2] for r in due),
                        "active":sum(r[3]<=time<r[4] for r in records),
                        "busy_mean":busy_area/time if time else 0,
                        "waiting":sum(r[2]<=time<r[3] for r in records),
                        "queue_mean":queue_area/time if time else 0,
                        "utilization":busy_area/(time*spec["capacity"]) if time else 0,
                        "released":len(started), "wait_total":sum(r[3]-r[2] for r in started),
                        "lost_count":sum(arrival<=time for _,arrival in rejected)}
                    for output in model["outputs"]:
                        expected[time,output["id"]] = values[output["id"]]
                assert actual.keys() == expected.keys(), "declarative oracle observation coverage differs"
                worst = 0
                for key,value in expected.items():
                    error = abs(actual[key]-value)
                    assert math.isfinite(actual[key]) and error <= 2e-9,(spec["name"],resource,key,actual[key],value)
                    worst = max(worst,error)
                checked += len(expected)
                largest = max(largest,worst)
                reports.append({"case":spec["name"],"resource":resource,"observations":len(expected),"maximum_error":worst})
    assert len(reports)==120
    report = {"version":1,"cases":reports,"observations":checked,"maximum_error":largest}
    if len(sys.argv)>2:
        Path(sys.argv[2]).write_text(json.dumps(report,indent=2)+"\n")
    print(f"Typed declarative queue/resource oracle: 120 cases, {checked} observations, maximum error {largest:.3g}")


if __name__ == "__main__":
    main()
