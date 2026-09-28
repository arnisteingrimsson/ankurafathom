"""Exact rational coupled clocks for agent stocks -> aggregate -> held-input SD."""
import argparse
import copy
from fractions import Fraction as F
import hashlib
import itertools
import json
from pathlib import Path

PLAN = Path(__file__).with_name("agent-stocks-atomic-plan.json")


def make_plan():
    cases = json.loads(PLAN.with_name("agent-stock-plan.json").read_text())["cases"]
    return {"version": 1, "cases": cases, "agent_steps": [.25, .125],
            "consumer_ratios": [.5, 1, 1.5, 2], "orders": list(map(list, itertools.permutations(range(3)))),
            "horizons": [k/8 for k in range(21)], "tolerance": 2e-12}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def reference(case, dt, ratio, horizon):
    dt, sd_dt, horizon = F(dt), F(dt)*F(ratio), F(horizon)
    agents = list(map(F, case["initial"]))
    rates = list(map(F, case["rates"]))
    globals_ = [F(case["reservoir"]), F(0), F(0)]
    supply, n = F(case["supply_rate"]), len(agents)
    source_events = {dt*i for i in range(1, int(horizon/dt)+1)}
    sd_events = {sd_dt*i for i in range(1, int(horizon/sd_dt)+1)}
    source_time, sd_time, revision, area = F(0), F(0), 0, F(0)
    signal = sum(agents)
    for event in sorted(source_events | sd_events):
        # The ending interval uses the preceding publication, even if agent
        # integration is also due at this event time.
        area += (event-sd_time)*signal
        sd_time = event
        if event in source_events:
            incoming = supply*globals_[0]
            completed = sum(k*x for k, x in zip(rates, agents))
            globals_ = [globals_[0]-dt*incoming, globals_[1]+dt*completed, globals_[2]+dt*sum(agents)]
            agents = [x+dt*(incoming/n-k*x) for k, x in zip(rates, agents)]
            signal = sum(agents)
            source_time = event
            revision += 1
    return {"source_time": float(source_time), "consumer_time": float(sd_time), "revision": revision,
            "agents": list(map(float, agents)), "globals": list(map(float, globals_)),
            "signal": float(signal), "area": float(area)}


def score(rows, plan):
    expected = {(c["id"], dt, r, tuple(order), h): reference(c, dt, r, h) for c in plan["cases"]
                for dt in plan["agent_steps"] for r in plan["consumer_ratios"] for order in plan["orders"]
                for h in plan["horizons"]}
    require(len(rows) == len(expected), "wrong observation count")
    seen, max_gap, comparisons = set(), 0., 0
    for row in rows:
        require(set(row) == {"case", "dt", "ratio", "order", "horizon", "source_time", "consumer_time", "revision",
                             "agents", "globals", "signal", "area"}, "invalid row schema")
        key = (row["case"], row["dt"], row["ratio"], tuple(row["order"]), row["horizon"])
        require(key in expected and key not in seen, "unknown or repeated observation")
        seen.add(key); ref = expected[key]
        for field in ("source_time", "consumer_time", "revision"):
            require(row[field] == ref[field], "clock/revision mismatch")
        require(len(row["agents"]) == len(ref["agents"]) and len(row["globals"]) == 3, "state width mismatch")
        actual = row["agents"]+row["globals"]+[row["signal"], row["area"]]
        target = ref["agents"]+ref["globals"]+[ref["signal"], ref["area"]]
        for x, y in zip(actual, target):
            require(type(x) in (int, float) and abs(x-y) <= plan["tolerance"]*max(1., abs(y)), "coupled trajectory mismatch")
            max_gap = max(max_gap, abs(x-y)); comparisons += 1
    return {"configurations": len(plan["cases"])*len(plan["agent_steps"])*len(plan["consumer_ratios"])*6,
            "observations": len(rows), "scalar_comparisons": comparisons, "max_absolute_gap": max_gap}


def contract(plan):
    rows = [dict(case=c["id"], dt=dt, ratio=r, order=o, horizon=h, **reference(c, dt, r, h))
            for c in plan["cases"] for dt in plan["agent_steps"] for r in plan["consumer_ratios"]
            for o in plan["orders"] for h in plan["horizons"]]
    score(rows, plan)
    invalid = [rows[:-1], rows[:-1]+[rows[0]]]
    for field in ("source_time", "consumer_time", "revision", "signal", "area"):
        bad = copy.deepcopy(rows); bad[4][field] += .125; invalid.append(bad)
    for field in ("agents", "globals"):
        bad = copy.deepcopy(rows); bad[4][field][0] += .125; invalid.append(bad)
    bad = copy.deepcopy(rows); bad[4]["area"] = float("nan"); invalid.append(bad)
    # Correct stock law with backward-applied publications is still invalid.
    bad = copy.deepcopy(rows)
    for row in bad:
        initial = sum(next(c for c in plan["cases"] if c["id"] == row["case"])["initial"])
        row["area"] += row["dt"]*(row["signal"]-initial)
    invalid.append(bad)
    for rows_ in invalid:
        try:
            score(rows_, plan)
        except ValueError:
            continue
        raise ValueError("corrupt coupled evidence accepted")
    print(f"Agent-stock atomic oracle: {len(invalid)} negative controls rejected")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--write-plan", action="store_true")
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--contract", action="store_true")
    parser.add_argument("--native", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    canonical = json.dumps(make_plan(), indent=2)+"\n"
    if args.write_plan:
        PLAN.write_text(canonical)
    require(PLAN.read_text() == canonical, "coupled plan changed")
    plan = json.loads(canonical)
    if args.verify:
        print("Agent-stock atomic plan: 144 configurations verified")
    if args.contract:
        contract(plan)
    if args.native:
        require(args.report is not None, "--report required")
        report = score([json.loads(line) for line in args.native.read_text().splitlines()], plan)
        report.update(plan_sha256=hashlib.sha256(PLAN.read_bytes()).hexdigest(),
                      native_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest())
        args.report.write_text(json.dumps(report, indent=2)+"\n")
        print(json.dumps(report))


if __name__ == "__main__":
    main()
