# Declarative agent neighborhoods

Typed `mode: "abm"` populations can now use grid, continuous-space, network and whole-population neighborhoods in synchronous phases and asynchronous guards/actions. [The runnable fixture](../models/typed_abm_neighbors.ir.json) combines grid diffusion, periodic movement, string-group comparisons and directed-network observations:

```sh
./build/fathom run models/typed_abm_neighbors.ir.json
```

## Bindings and queries

A population may declare one `space` and one `network`. A grid binds two distinct integer fields; its coordinates and radius are dimensionless:

```json
"space": {"kind":"grid", "x":"x", "y":"y", "width":4, "height":3, "wrap":true},
"network": {"directed":true, "edges":[[0,1],[0,2],[1,3]]},
"queries": [
  {"id":"near_mean", "source":"space", "op":"mean", "field":"value", "radius":1, "unit":"1", "moore":false},
  {"id":"linked_sum", "source":"network", "op":"sum", "field":"value"},
  {"id":"same_group", "source":"space", "op":"count_same", "field":"group", "radius":1, "unit":"1"}
]
```

Continuous bindings instead use `fields`, `lower`, `upper`, `bin_width`, `unit` and optional `wrap`. For example, `{"kind":"continuous","fields":["x","y"],"lower":[0,0],"upper":[10,10],"bin_width":0.5,"unit":"meter","wrap":true}` requires both coordinate fields to have meter units. One to three distinct real/integer coordinate fields are supported. Bounds are half-open; bin width is positive. Spatial queries use that same unit. Continuous queries do not accept `moore`.

Query aliases can appear in arithmetic, such as `"expr":"(value+near_mean)/2"`, or dimensionless guards. An output can select `{"id":"first_near","agent":0,"query":"near_mean"}`. Aliases cannot collide with fields, parameters or other aliases. Query fields refer to base fields, never other queries.

| Option | Meaning |
|---|---|
| `source: "space"` | Grid Moore distance by default; `moore:false` selects von Neumann distance. Continuous space uses Euclidean distance. |
| `source: "network"` | Outgoing neighbors in a directed graph; adjacent vertices in an undirected graph. |
| `source: "population"` | Every live agent. |
| `include_self` | Defaults to false; exclusion removes only the requesting ID, retaining co-located peers. |
| `op: "count"` | Number of selected agents; no `field` option. |
| `op: "count_same"` | Number whose selected field equals the requesting agent's field; strings are supported. |
| `op: "sum"` / `"mean"` | Numeric/boolean field aggregate; an empty neighborhood returns zero. |

Neighbors are accumulated in increasing stable-ID order. Counts are dimensionless; sum/mean preserve the field unit. Real overflow and sums outside the exact integer range fail. The current reference runtime evaluates declared query values when building a phase/guard/action expression environment and evaluates selected output queries when sampling.

## Movement and rollback

Coordinate records are authoritative. Periodic normalization changes the temporary spatial index, while stored coordinates remain unwrapped. A phase assigning `x+1` therefore needs no modulo expression. Bounded spaces reject out-of-range coordinates.

Every initial population, mutation batch, synchronous phase and asynchronous event effect validates the complete resulting spatial state. Grid swaps are legal; collisions fail even when no query or output reads position. An invalid intermediate phase fails the entire synchronous step. An invalid async effect restores the whole timestamp, including the clock and timer identities. Continuous agents may share positions.

The native `SpatialSnapshot<Tag>` owns its index and namespace independently of the source store. `TypedPopulation` and `AsyncPopulation` accept an optional pure validator. Synchronous `apply(updates, retirements, births)` validates the complete batch once; the DEVS wrapper uses this for external effects. No mutable spatial cache lives outside the transaction. This correctness-first implementation rebuilds indexes; caching and acceleration are later work.

## Evidence and limits

The predeclared [interaction plan](../tests/oracles/abm/interaction-plan.json) covers **13 trajectories and 4,212 observations**. The pinned independent adapter uses Mesa SingleGrid, ContinuousSpace and NetworkGrid, with NetworkX directed/undirected graphs. It computes three explicit Jacobi phases: neighborhood averaging, coordinate motion, and network-weighted updates. It does not interpret the native expression implementation. Reference trajectories were frozen before native comparison, and pinned regeneration reproduces them.

Cases cover bounded/periodic grids, zero/large radii, Moore/von Neumann neighborhoods, self inclusion, isolated directed vertices, co-located continuous agents, periodic motion with changing neighbors, population aggregates, and exact string-group comparisons. Every agent's coordinates, evolving value and query outputs are compared at nine observation times. Normal/sanitizer comparisons use absolute/relative tolerance `2e-12`; observed maximum absolute error is about `1.42e-14`.

Native tests cover nonzero IDs, foreign namespaces, liveness snapshots, atomic swaps, failed spawn IDs, intermediate-phase rollback, async retry timer IDs, DEVS input-batch swaps, coordinate type errors and continuous boundary behavior. CLI hand tests add simultaneous asynchronous command ordering, guards, unit propagation, declaration permutations, observation-density invariance, unqueried movement failures and aggregate overflow. Schema contracts include closed objects, binding types, graph endpoints and aliases. See [current status](STATUS.md) for regression totals.

```sh
python3 tests/oracles/abm/interaction_plan.py --verify
python3 tests/oracles/abm/interaction_oracle.py --contract --native build/fathom
.venv-abm-oracle/bin/python tests/oracles/abm/interaction_oracle.py --verify
```

These deterministic trajectories extend the existing 7,504 independent neighborhood checks. They do not satisfy the full Schelling/Boids/wealth/Sugarscape/SIR distribution gates or sync/async SIR convergence. [Declarative topic delivery](ABM_TOPICS.md) is now supported. [Scheduled births/retirements](ABM_LIFECYCLE.md) now update live spatial and population membership; retired vertices and incident edges are removed, and newborns are initially isolated. [Phase/transition lifecycle](ABM_BEHAVIOR_LIFECYCLE.md) now uses the same final-batch spatial validation. [Scheduled graph edits](ABM_MUTABLE_NETWORKS.md) now connect current live vertices transactionally. [Declarative generators](ABM_GRAPH_GENERATORS.md) now have separate evidence. Behavior-generated edits, topic-handler lifecycle and cross-population links remain extensions. The declarative grid is bounded to one million cells; the native grid contract is unchanged. [M4 acceptance](M4_ACCEPTANCE.md) records the separately completed canonical-model and sync/async convergence gates.
