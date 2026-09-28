"""Exact-rational oracle for typed snapshot reducers and piecewise-constant SD inputs."""
import argparse
import copy
from fractions import Fraction
import hashlib
import json
from pathlib import Path

PLAN = Path(__file__).with_name("typed-aggregate-plan.json")


def make_plan():
    cases = []
    for c in range(24):
        snapshots = []
        for s, time in enumerate([0, .375, .5, 1.125, 1.5, 2.25]):
            records = [{"value": (c+3*j+2*s) % 13-6, "selected": (j+c+s) % 3 != 0,
                        "alive": s != 5 and not (s >= 2 and j == 0)} for j in range(3+s)]
            snapshots.append({"time": time, "revision": 100+7*s, "records": records})
        cases.append({"id": f"case_{c:02d}", "dt": .25 if c % 2 else .5,
                      "filtered": bool(c % 3), "snapshots": snapshots})
    return {"version": 1, "cases": cases, "orders": [0, 1],
            "horizons": [k/8 for k in range(21)], "tolerance": 2e-12,
            "reducers": ["sum", "mean", "count", "min", "max"],
            "empty": [0, 0, 0, -1, 1]}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def reduce_snapshot(snapshot, filtered):
    values = sorted(Fraction(r["value"]) for r in snapshot["records"]
                    if r["alive"] and (not filtered or r["selected"]))
    return ([sum(values), sum(values)/len(values), Fraction(len(values)), values[0], values[-1]]
            if values else list(map(Fraction, [0, 0, 0, -1, 1])))


def reference(case, horizon):
    # Event times are rational; this integrates rectangles, not a numerical solver.
    horizon, dt = Fraction(horizon), Fraction(case["dt"])
    by_time = {Fraction(s["time"]): s for s in case["snapshots"] if s["time"] <= horizon}
    events = set(by_time) | {k*dt for k in range(1, int(horizon/dt)+1)}
    time, values, state = Fraction(0), [Fraction(0)]*5, [Fraction(0)]*5
    revision = None
    for event in sorted(events):
        state = [x+(event-time)*v for x, v in zip(state, values)]
        time = event
        if event in by_time:
            values = reduce_snapshot(by_time[event], case["filtered"])
            revision = by_time[event]["revision"]
    return {"time": float(time), "revision": revision, "signals": list(map(float, values)),
            "stocks": list(map(float, state))}


def score(rows, plan):
    expected = {(c["id"], order, h): reference(c, h) for c in plan["cases"]
                for order in plan["orders"] for h in plan["horizons"]}
    require(len(rows) == len(expected), "incorrect row count")
    seen, max_gap = set(), 0.
    for row in rows:
        require(set(row) == {"case", "order", "horizon", "time", "revision", "signals", "stocks"}, "invalid row schema")
        key = (row["case"], row["order"], row["horizon"])
        require(key in expected and key not in seen, "unknown/duplicate row")
        seen.add(key)
        ref = expected[key]
        require(row["time"] == ref["time"] and row["revision"] == ref["revision"], "clock/revision mismatch")
        for name in ("signals", "stocks"):
            require(len(row[name]) == 5, "wrong vector width")
            for actual, target in zip(row[name], ref[name]):
                require(type(actual) in (int, float) and abs(actual-target) <= plan["tolerance"]*max(1., abs(target)),
                        "reducer or integral mismatch")
                max_gap = max(max_gap, abs(actual-target))
    return {"cases": len(plan["cases"]), "declaration_orders": 2, "observations": len(rows),
            "scalar_comparisons": len(rows)*10, "max_absolute_gap": max_gap}


def contract(plan):
    rows = [dict(case=c["id"], order=o, horizon=h, **reference(c, h)) for c in plan["cases"]
            for o in plan["orders"] for h in plan["horizons"]]
    score(rows, plan)
    bad = [rows[:-1], rows[:-1]+[rows[0]]]
    for name in ("signals", "stocks"):
        for i in range(5):
            corrupted = copy.deepcopy(rows); corrupted[4][name][i] += .1; bad.append(corrupted)
    corrupted = copy.deepcopy(rows); corrupted[4]["revision"] += 1; bad.append(corrupted)
    corrupted = copy.deepcopy(rows); corrupted[4]["time"] += .125; bad.append(corrupted)
    corrupted = copy.deepcopy(rows); corrupted[4]["stocks"][0] = float("nan"); bad.append(corrupted)
    # Replacing held values with new ones over the preceding interval must fail.
    corrupted = copy.deepcopy(rows)
    for row in corrupted:
        if row["time"] > 0:
            row["stocks"] = [row["time"]*x for x in row["signals"]]
    bad.append(corrupted)
    for sample in bad:
        try:
            score(sample, plan)
        except ValueError:
            continue
        raise ValueError("bad aggregate evidence accepted")
    print(f"Typed aggregate oracle: {len(bad)} negative controls rejected")


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
    require(PLAN.read_text() == canonical, "frozen plan differs from canonical generation")
    plan = json.loads(canonical)
    if args.verify:
        print("24-case typed aggregate plan verified")
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
