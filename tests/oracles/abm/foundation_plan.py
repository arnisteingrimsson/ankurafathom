"""Predeclared, dependency-free inputs for the M4 spatial/network comparisons."""
import argparse
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent


def make_plan():
    cases = []
    for width, height in [(1, 1), (1, 5), (2, 3), (5, 4), (7, 7)]:
        for wrap in [False, True]:
            points = [(x, y) for x in range(width) for y in range(height)
                      if (x + 2 * y) % 3 != 1]
            agents = [[100 + 7 * i, list(p)] for i, p in enumerate(points)]
            queries = [[a[0], r, moore, own] for a in agents
                       for r in [0, 1, 2, max(width, height) + 1]
                       for moore in [False, True] for own in [False, True]]
            # Rotate occupied locations in one transaction, including fully occupied grids.
            moves = [[a[0], agents[(i + 1) % len(agents)][1]] for i, a in enumerate(agents)]
            cases.append(dict(id=f"grid-{width}-{height}-{int(wrap)}", kind="grid",
                              width=width, height=height, wrap=wrap, agents=agents,
                              phases=[[], moves], queries=queries))
    for wrap in [False, True]:
        for bin_width in [0.25, 0.7, 2.0, 20.0]:
            # Binary-exact coordinates exercise edges, co-location and exact radius ties.
            agents = [[501 + 11 * i, [-2 + ((i * 7) % 39) / 8, 1 + ((i * 13) % 55) / 8]]
                      for i in range(24)]
            agents[1][1] = agents[0][1][:]
            agents[2][1], agents[3][1] = [-1.5, 1.0], [-1.0, 1.0]
            moves = [[a[0], [-2 + ((i * 17 + 3) % 39) / 8,
                              1 + ((i * 19 + 2) % 55) / 8]] for i, a in enumerate(agents)]
            if wrap:
                # Mesa and native APIs both normalize finite out-of-domain moves.
                moves[0][1] = [-7.125, 15.875]
            queries = [[a[0], r, own] for a in agents
                       for r in [0.0, 0.5, 1.0, 2.75, 20.0] for own in [False, True]]
            cases.append(dict(id=f"continuous-{int(wrap)}-{bin_width}", kind="continuous",
                              lower=[-2.0, 1.0], upper=[3.0, 8.0], bin_width=bin_width,
                              wrap=wrap, agents=agents, phases=[[], moves], queries=queries))
    for directed in [False, True]:
        for n in [1, 2, 9, 24]:
            vertices = [1001 + 13 * i for i in range(n)]
            edges = [[a, b] for i, a in enumerate(vertices) for j, b in enumerate(vertices)
                     if i != j and (directed or i < j) and (3 * i + j) % 5 == 0]
            removals = edges[::3]
            additions = [[a, b] for i, a in enumerate(vertices) for j, b in enumerate(vertices)
                         if i != j and (directed or i < j) and (3 * i + j) % 5 == 1]
            cases.append(dict(id=f"network-{int(directed)}-{n}", kind="network",
                              directed=directed, vertices=vertices[::-1], edges=edges[::-1],
                              phases=[dict(add=[], remove=[]), dict(add=additions, remove=removals)],
                              queries=vertices))
    return dict(version=1, cases=cases)


def encoded(plan):
    return json.dumps(plan, indent=2, allow_nan=False) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    path = HERE / "foundation-plan.json"
    data = encoded(make_plan())
    if args.verify:
        if path.read_text() != data:
            raise SystemExit("ABM foundation input plan is stale")
    else:
        path.write_text(data)
    print(f"ABM foundation plan: {len(make_plan()['cases'])} cases")
