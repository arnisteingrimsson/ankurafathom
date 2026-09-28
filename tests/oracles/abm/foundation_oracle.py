"""Pinned Mesa/NetworkX reference; offline checking needs only the standard library.

Grid swaps are applied by removing all movers before placing them. Continuous
queries include all agents at the center, then filter only the querying ID.
NetworkGrid uses one agent per node and outgoing one-hop neighbors on DiGraph.
These adapters reconcile API conventions; neighborhood calculations stay in Mesa.
"""
import argparse
import copy
import hashlib
import importlib.metadata
import json
from pathlib import Path

from foundation_plan import HERE, encoded, make_plan


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata():
    return dict(version=1, plan_sha256=digest(HERE / "foundation-plan.json"),
                adapter_sha256=digest(Path(__file__)),
                requirements_sha256=digest(HERE / "requirements.txt"),
                engines={"Mesa": "3.5.1", "networkx": "3.7"})


def generate():
    for line in (HERE / "requirements.txt").read_text().splitlines():
        package, version = line.split("==")
        require(importlib.metadata.version(package) == version, f"unpinned package: {package}")
    import networkx as nx
    from mesa.space import SingleGrid, ContinuousSpace, NetworkGrid

    class Agent:
        def __init__(self, identity):
            self.unique_id, self.pos = identity, None

    result = metadata()
    result["cases"] = []
    for case in make_plan()["cases"]:
        kind = case["kind"]
        if kind == "network":
            graph = nx.DiGraph() if case["directed"] else nx.Graph()
            graph.add_nodes_from(case["vertices"])
            graph.add_edges_from(case["edges"])
            space = NetworkGrid(graph)
            agents = {i: Agent(i) for i in case["vertices"]}
            for identity, agent in agents.items():
                space.place_agent(agent, identity)
        else:
            if kind == "grid":
                space = SingleGrid(case["width"], case["height"], case["wrap"])
            else:
                space = ContinuousSpace(*case["upper"], case["wrap"], *case["lower"])
            agents = {identity: Agent(identity) for identity, _ in case["agents"]}
            for identity, point in case["agents"]:
                space.place_agent(agents[identity], tuple(point))
        phases = []
        for phase in case["phases"]:
            if kind == "network":
                graph.remove_edges_from(phase["remove"])
                graph.add_edges_from(phase["add"])
                rows = [sorted(a.unique_id for a in space.get_neighbors(i)) for i in case["queries"]]
            else:
                for identity, _ in phase:
                    space.remove_agent(agents[identity])
                for identity, point in phase:
                    space.place_agent(agents[identity], tuple(point))
                rows = []
                for query in case["queries"]:
                    identity, radius, *flags = query
                    if kind == "grid":
                        found = space.get_neighbors(agents[identity].pos, flags[0], flags[1], radius)
                    else:
                        found = space.get_neighbors(agents[identity].pos, radius, include_center=True)
                        if not flags[0]:
                            found = [a for a in found if a.unique_id != identity]
                    rows.append(sorted(a.unique_id for a in found))
            phases.append(rows)
        result["cases"].append(dict(id=case["id"], phases=phases))
    return result


def validate_reference(reference):
    require((HERE / "foundation-plan.json").read_text() == encoded(make_plan()), "input plan is stale")
    require(set(reference) == set(metadata()) | {"cases"}, "reference fields differ")
    for key, value in metadata().items():
        require(reference[key] == value, f"stale reference metadata: {key}")
    validate_shape(reference["cases"])


def validate_shape(cases):
    plan = make_plan()["cases"]
    require(type(cases) is list and len(cases) == len(plan), "case count differs")
    for actual, spec in zip(cases, plan):
        require(set(actual) == {"id", "phases"} and actual["id"] == spec["id"], "case identity differs")
        phases = actual["phases"]
        require(type(phases) is list and len(phases) == len(spec["phases"]), "phase count differs")
        identities = set(spec["vertices"] if spec["kind"] == "network" else [a[0] for a in spec["agents"]])
        for rows in phases:
            require(type(rows) is list and len(rows) == len(spec["queries"]), "query count differs")
            for row in rows:
                require(type(row) is list and all(type(i) is int and i in identities for i in row), "invalid neighbor ID")
                require(row == sorted(set(row)), "unsorted or duplicate neighbors")


def compare(reference, native):
    validate_reference(reference)
    require(set(native) == {"version", "cases"} and type(native["version"]) is int and native["version"] == 1,
            "invalid native report")
    validate_shape(native["cases"])
    count = 0
    for expected, actual in zip(reference["cases"], native["cases"]):
        for phase, (left, right) in enumerate(zip(expected["phases"], actual["phases"])):
            for query, (a, b) in enumerate(zip(left, right)):
                require(a == b, f"neighbors differ: {expected['id']} phase={phase} query={query}: {a} != {b}")
                count += 1
    return count


def contract(reference):
    native = dict(version=1, cases=copy.deepcopy(reference["cases"]))
    compare(reference, native)
    mutations = [lambda x: x["cases"].pop(), lambda x: x["cases"].reverse(),
                 lambda x: x["cases"][0]["phases"].pop(),
                 lambda x: x["cases"][0]["phases"][0].pop(),
                 lambda x: x["cases"][0]["phases"][0][0].append(999999),
                 lambda x: x["cases"][0]["phases"][0].__setitem__(0, [100]),
                 lambda x: x.__setitem__("version", True)]
    for mutate in mutations:
        corrupt = copy.deepcopy(native)
        mutate(corrupt)
        try:
            compare(reference, corrupt)
        except ValueError:
            continue
        raise AssertionError("corrupted native report accepted")
    for key in metadata():
        corrupt = copy.deepcopy(reference)
        corrupt[key] = None
        try:
            compare(corrupt, native)
        except ValueError:
            continue
        raise AssertionError(f"corrupted reference {key} accepted")
    print("ABM foundation comparison rejects truncated, reordered, stale and altered reports")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--generate", action="store_true")
    action.add_argument("--verify", action="store_true")
    action.add_argument("--native", type=Path)
    action.add_argument("--contract", action="store_true")
    args = parser.parse_args()
    path = HERE / "foundation-reference.json"
    if args.generate:
        reference = generate()
        validate_reference(reference)
        path.write_text(encoded(reference))
        print(f"Generated {len(reference['cases'])} Mesa/NetworkX foundation cases")
    else:
        reference = json.loads(path.read_text())
        validate_reference(reference)
        if args.verify:
            require(reference == generate(), "Mesa/NetworkX regeneration differs")
            print("Mesa/NetworkX regeneration matches frozen reference")
        elif args.contract:
            contract(reference)
        else:
            count = compare(reference, json.loads(args.native.read_text()))
            print(f"Mesa/NetworkX: {len(reference['cases'])} cases, {count} exact neighborhood comparisons")
