"""Independent Euler/continuous closed forms for the frozen agent-stock gate.

Only stdlib is needed to score; optional plots use the local plotting environment.
No native simulator, SD solver, or native trajectory is used to form expectations.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path

PLAN = Path(__file__).with_name("agent-stock-plan.json")


def require(ok, message):
    if not ok:
        raise ValueError(message)


def expected(case, time, dt=None):
    a, r0 = case["supply_rate"], case["reservoir"]
    n = len(case["initial"])
    # Integrals are continuous or discrete left-endpoint sums, respectively.
    def decay(k):
        return math.exp(-k*time) if dt is None else (1-k*dt)**round(time/dt)

    def integral(k):
        return (1-decay(k))/k if k else time

    agents, exposure = [], 0.
    for x0, k in zip(case["initial"], case["rates"]):
        x = x0*decay(k)
        area = x0*integral(k)
        if a:
            # The frozen feedback case has k != a; a=0 covers pure decay.
            coefficient = a*r0/n/(k-a)
            x += coefficient*(decay(a)-decay(k))
            area += coefficient*(integral(a)-integral(k))
        agents.append(x)
        exposure += area
    reservoir = r0*decay(a)
    completed = r0 + sum(case["initial"]) - reservoir - sum(agents)
    return agents, [reservoir, completed, exposure]


def score(rows, plan):
    keys = {(c["id"], dt, t) for c in plan["cases"] for dt in plan["steps"] for t in plan["times"]}
    require(len(rows) == len(keys), "incorrect observation count")
    index = {}
    for row in rows:
        require(set(row) == {"case", "dt", "time", "agents", "sum", "globals"}, "row schema mismatch")
        key = (row["case"], row["dt"], row["time"])
        require(key in keys and key not in index, "unexpected or duplicate observation")
        index[key] = row
    max_recurrence_gap, max_conservation_gap = 0., 0.
    curves, numeric = [], []
    for case in plan["cases"]:
        errors = []
        total = sum(case["initial"]) + case["reservoir"]
        for dt in plan["steps"]:
            error = 0.
            for t in plan["times"]:
                row = index[(case["id"], dt, t)]
                agents, globals_ = expected(case, t, dt)
                require(len(row["agents"]) == len(agents) and len(row["globals"]) == 3, "state width mismatch")
                actual = row["agents"] + row["globals"] + [row["sum"]]
                reference = agents + globals_ + [sum(agents)]
                require(all(type(x) in (int, float) and math.isfinite(x) and x >= 0 for x in actual), "invalid stock value")
                gap = max(abs(x-y) for x, y in zip(actual, reference))
                require(gap <= plan["tolerance"]*max(1., *map(abs, reference)), "Euler recurrence mismatch")
                require(abs(sum(row["agents"])-row["sum"]) <= plan["tolerance"]*max(1., row["sum"]), "aggregate mismatch")
                conservation = abs(sum(row["agents"])+sum(row["globals"][:2])-total)
                require(conservation <= plan["tolerance"]*max(1., total), "material not conserved")
                max_recurrence_gap = max(max_recurrence_gap, gap)
                max_conservation_gap = max(max_conservation_gap, conservation)
                continuous_agents, continuous_globals = expected(case, t)
                continuous = continuous_agents + continuous_globals + [sum(continuous_agents)]
                error = max(error, max(abs(x-y) for x, y in zip(actual, continuous)))
                numeric.append({"case": case["id"], "dt": dt, "time": t, "native_sum": row["sum"],
                                "euler_sum": sum(agents), "continuous_sum": sum(continuous_agents),
                                "recurrence_max_gap": gap, "conservation_gap": conservation})
            errors.append(error)
        orders = [math.log(a/b, 2) for a, b in zip(errors, errors[1:])]
        require(all(plan["order_bounds"][0] <= p <= plan["order_bounds"][1] for p in orders), "Euler refinement order failed")
        curves.append({"case": case["id"], "steps": plan["steps"], "max_continuous_errors": errors, "orders": orders})
    return {"observations": len(rows), "agent_values": sum(len(row["agents"]) for row in rows),
            "max_recurrence_gap": max_recurrence_gap, "max_conservation_gap": max_conservation_gap,
            "curves": curves, "numeric": numeric}


def synthetic_rows(plan):
    rows = []
    for c in plan["cases"]:
        for dt in plan["steps"]:
            for t in plan["times"]:
                agents, globals_ = expected(c, t, dt)
                rows.append({"case": c["id"], "dt": dt, "time": t, "agents": agents,
                             "globals": globals_, "sum": sum(agents)})
    return rows


def contract(plan):
    rows = synthetic_rows(plan)
    score(rows, plan)
    bad = []
    bad.append(rows[:-1])
    bad.append(rows[:-1]+[rows[0]])
    corrupted = copy.deepcopy(rows); corrupted[1]["agents"][0] += .1; bad.append(corrupted)
    corrupted = copy.deepcopy(rows); corrupted[1]["globals"][2] += .1; bad.append(corrupted)
    corrupted = copy.deepcopy(rows); corrupted[1]["sum"] = float("nan"); bad.append(corrupted)
    corrupted = copy.deepcopy(rows); corrupted[1]["globals"][1] += .1; bad.append(corrupted)
    # Conserved material with a wrong kinetic law must still fail.
    corrupted = copy.deepcopy(rows)
    for row in corrupted:
        case = next(c for c in plan["cases"] if c["id"] == row["case"])
        row["agents"] = case["initial"][:]
        row["sum"] = sum(case["initial"])
        row["globals"] = [case["reservoir"], 0., row["time"]*row["sum"]]
    bad.append(corrupted)
    # Integrating the newly published sum retroactively changes the exposure.
    corrupted = copy.deepcopy(rows)
    for row in corrupted:
        initial = next(sum(c["initial"]) for c in plan["cases"] if c["id"] == row["case"])
        row["globals"][2] += row["dt"]*(row["sum"]-initial)
    bad.append(corrupted)
    for sample in bad:
        try:
            score(sample, plan)
        except ValueError:
            continue
        raise ValueError("negative control accepted")
    print(f"Agent stock contract: {len(bad)} negative controls rejected")


def plot(report_path, destination):
    import csv
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    report = json.loads(report_path.read_text())
    destination.mkdir(parents=True, exist_ok=True)
    fig, ax = plt.subplots(figsize=(7, 4.5), layout="constrained")
    for curve in report["curves"]:
        ax.loglog(curve["steps"], curve["max_continuous_errors"], "o-", label=curve["case"].replace("_", " "))
    ax.set(xlabel="Euler time step", ylabel="Maximum absolute continuous-solution gap",
           title="Agent continuous stocks: first-order refinement")
    ax.grid(True, which="both", alpha=.2); ax.legend()
    for extension in ("png", "svg"):
        fig.savefig(destination/f"agent-stocks-convergence.{extension}", dpi=160)
    plt.close(fig)
    with (destination/"agent-stocks-numeric.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(report["numeric"][0]))
        writer.writeheader(); writer.writerows(report["numeric"])
    (destination/"agent-stocks-provenance.json").write_text(json.dumps({
        "report_sha256": hashlib.sha256(report_path.read_bytes()).hexdigest(),
        "plan_sha256": report["plan_sha256"], "native_sha256": report["native_sha256"],
        "method": "Independent discrete and continuous closed forms; no stochastic sampling",
    }, indent=2)+"\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--contract", action="store_true")
    parser.add_argument("--plots", type=Path)
    args = parser.parse_args()
    plan = json.loads(PLAN.read_text())
    if args.contract:
        contract(plan)
    if args.native:
        require(args.report is not None, "--report required with --native")
        report = score([json.loads(line) for line in args.native.read_text().splitlines()], plan)
        report.update(plan_sha256=hashlib.sha256(PLAN.read_bytes()).hexdigest(),
                      native_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest())
        args.report.write_text(json.dumps(report, indent=2, allow_nan=False)+"\n")
        print(f"Agent stocks: {report['observations']} observations, {report['agent_values']} individual values, 3 refinement curves passed")
    if args.plots:
        require(args.report is not None, "--report required with --plots")
        plot(args.report, args.plots)


if __name__ == "__main__":
    main()
