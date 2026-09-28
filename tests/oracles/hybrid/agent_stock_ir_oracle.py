"""Independent exact-rational interpreter checks for agent_stock_sd CLI models."""
import argparse
import copy
import csv
from fractions import Fraction as F
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT/"models/agent_stock_sd.ir.json"
PLAN = Path(__file__).with_name("agent-stock-ir-plan.json")
EXPERIMENT = ROOT/"models/agent_stock_sd.experiment.json"


def make_plan():
    return {"version": 1, "agent_steps": [.125, .25, .375, .5], "sd_steps": [.125, .25, .5],
            "orders": ["forward", "reverse"], "horizon": 2, "tolerance": 2e-12,
            "experiment_productivity": [0, 1, 1.5], "experiment_replications": 2}


def model(case, order):
    result = json.loads(FIXTURE.read_text())
    steps = make_plan()
    result["time"].update(dt=steps["sd_steps"][case % 3], horizon=steps["horizon"])
    population = result["components"][0]
    population["dt"] = steps["agent_steps"][case // 3]
    result["parameters"][0]["value"] = .5 if case % 2 else 1.5
    for i, record in enumerate(population["agents"]):
        record["enabled"] = (i+case) % 3 == 0
    if case == 11:
        population["agents"] = []
    result["parameters"].append({"id": "wear", "value": .125, "unit": "1/day"})
    result["components"].extend([
        {"id": "lost", "kind": "stock", "init": 0, "unit": "hour"},
        {"id": "discard", "kind": "flow", "source": "delivered", "destination": "lost",
         "expr": "wear*delivered", "unit": "hour/day"},
    ])
    result["outputs"].append({"id": "lost_total", "expr": "lost", "unit": "hour"})
    next(o for o in result["outputs"] if o["id"] == "balance")["expr"] = "pending+delivered+lost"
    if order == "reverse":
        result["components"].reverse()
    return result


def expected(document):
    population = next(c for c in document["components"] if c["kind"] == "population")
    records = population["agents"]
    initial = [F(a["todo"]) for a in records]
    todo, done = initial[:], [F(a["done"]) for a in records]
    parameters = {p["id"]: F(p["value"]) for p in document["parameters"]}
    rates = [parameters["productivity"]*F(a["speed"]) for a in records]
    dt, sd_dt, horizon = F(population["dt"]), F(document["time"]["dt"]), F(document["time"]["horizon"])
    agent_times = {i*dt for i in range(1, int(horizon/dt)+1)}
    output_times = {i*sd_dt for i in range(int(horizon/sd_dt)+1)}
    signals = sum(k*x for k, x in zip(rates, todo))
    delivered, lost, time = F(0), F(0), F(0)
    rows = {}
    for event in sorted(agent_times | output_times):
        discard = parameters.get("wear", F(0))*delivered
        delivered += (event-time)*(signals-discard)
        lost += (event-time)*discard
        time = event
        if event in agent_times:
            transfers = [dt*k*x for k, x in zip(rates, todo)]
            todo = [x-y for x, y in zip(todo, transfers)]
            done = [x+y for x, y in zip(done, transfers)]
            signals = sum(k*x for k, x in zip(rates, todo))
        if event in output_times:
            selected = [x for x, record in zip(todo, records) if record["enabled"]]
            values = dict(pending_total=sum(todo), completed_total=sum(done), delivered_total=delivered,
                          balance=sum(todo)+delivered+lost, selected_average=sum(selected)/len(selected) if selected else F(0),
                          people_count=F(len(records)), minimum_work=min(todo, default=F(0)), maximum_work=max(todo, default=F(0)),
                          completion_rate=signals, weighted_total=sum(F(r["level"])*x for r, x in zip(records, todo)), lost_total=lost)
            for name, value in values.items():
                if any(o["id"] == name for o in document["outputs"]):
                    rows[(float(event), name)] = float(value)
    return rows


def require(ok, message):
    if not ok:
        raise ValueError(message)


def score(rows, reference, tolerance):
    require(len(rows) == len(reference), "incorrect CSV row count")
    seen, gap = set(), 0.
    for row in rows:
        require(set(row) == {"time", "output_id", "value"}, "incorrect CSV columns")
        key = (float(row["time"]), row["output_id"])
        require(key in reference and key not in seen, "unexpected or repeated output")
        seen.add(key)
        actual, target = float(row["value"]), reference[key]
        require(abs(actual-target) <= tolerance*max(1., abs(target)), "declarative trajectory differs from rational reference")
        gap = max(gap, abs(actual-target))
    return gap


def contract(plan):
    reference = expected(model(0, "forward"))
    rows = [dict(time=t, output_id=name, value=value) for (t, name), value in reference.items()]
    score(rows, reference, plan["tolerance"])
    bad = [rows[:-1], rows[:-1]+[rows[0]]]
    for name in [o["id"] for o in model(0, "forward")["outputs"]]:
        changed = copy.deepcopy(rows)
        next(r for r in changed if r["output_id"] == name and r["time"] > 0)["value"] += .1
        bad.append(changed)
    changed = copy.deepcopy(rows);changed[0]["value"] = float("nan");bad.append(changed)
    changed = copy.deepcopy(rows);changed[0]["time"] = .01;bad.append(changed)
    for sample in bad:
        try:
            score(sample, reference, plan["tolerance"])
        except ValueError:
            continue
        raise ValueError("invalid declarative evidence accepted")
    print(f"Agent-stock IR contract: {len(bad)} corruptions rejected")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--write-plan", action="store_true")
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--contract", action="store_true")
    parser.add_argument("--fathom", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    canonical = json.dumps(make_plan(), indent=2)+"\n"
    if args.write_plan:
        PLAN.write_text(canonical)
    require(PLAN.read_text() == canonical, "IR oracle plan changed")
    plan = json.loads(canonical)
    if args.verify:
        print("Agent-stock IR plan: 24 models verified")
    if args.contract:
        contract(plan)
    if args.fathom:
        require(args.report is not None, "--report required")
        executable = args.fathom.resolve()
        comparisons, max_gap, digests = 0, 0., []
        with tempfile.TemporaryDirectory() as directory:
            for case in range(12):
                prior = None
                for order in plan["orders"]:
                    document = model(case, order)
                    path = Path(directory)/f"case-{case}-{order}.json"
                    path.write_text(json.dumps(document))
                    result = subprocess.run([str(executable), "run", str(path)], text=True, capture_output=True, check=False)
                    require(result.returncode == 0, result.stderr)
                    rows = list(csv.DictReader(io.StringIO(result.stdout)))
                    max_gap = max(max_gap, score(rows, expected(document), plan["tolerance"]))
                    comparisons += len(rows)
                    if prior is not None:
                        require(rows == prior, "declaration order changed decoded outputs")
                    prior = rows
                    digests.append(hashlib.sha256(result.stdout.encode()).hexdigest())
        experiment = json.loads(EXPERIMENT.read_text())
        require([s["parameters"]["productivity"] for s in experiment["scenarios"]] == plan["experiment_productivity"]
                and experiment["replications"] == plan["experiment_replications"], "experiment fixture changed")
        result = subprocess.run([str(executable), "run", str(FIXTURE), "--experiment", str(EXPERIMENT)],
                                text=True, capture_output=True, check=False)
        require(result.returncode == 0, result.stderr)
        groups = {}
        for row in csv.DictReader(io.StringIO(result.stdout)):
            require(set(row) == {"scenario", "replication", "time", "output_id", "value"}, "experiment CSV columns differ")
            key = (int(row.pop("scenario")), int(row.pop("replication")))
            groups.setdefault(key, []).append(row)
        require(set(groups) == {(s["id"], r) for s in experiment["scenarios"] for r in range(experiment["replications"])},
                "experiment trajectory coverage differs")
        for scenario in experiment["scenarios"]:
            document = json.loads(FIXTURE.read_text())
            document["parameters"][0]["value"] = scenario["parameters"]["productivity"]
            reference = expected(document)
            for replication in range(experiment["replications"]):
                max_gap = max(max_gap, score(groups[(scenario["id"], replication)], reference, plan["tolerance"]))
        report = dict(models=24, comparisons=comparisons, experiment_trajectories=len(groups),
                      experiment_comparisons=sum(map(len, groups.values())),
                      experiment_csv_sha256=hashlib.sha256(result.stdout.encode()).hexdigest(), max_absolute_gap=max_gap,
                      plan_sha256=hashlib.sha256(PLAN.read_bytes()).hexdigest(),
                      fixture_sha256=hashlib.sha256(FIXTURE.read_bytes()).hexdigest(), native_csv_sha256=digests)
        args.report.write_text(json.dumps(report, indent=2)+"\n")
        print(f"Agent-stock IR: 24 models, {comparisons} comparisons plus 6 experiment trajectories, max gap {max_gap}")


if __name__ == "__main__":
    main()
