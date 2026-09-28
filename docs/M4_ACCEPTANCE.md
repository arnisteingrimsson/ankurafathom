# M4 CPU ABM acceptance checkpoint

The declared CPU ABM core is accepted locally against implementation-plan §7.4:
exact invariants, canonical model distributions and sync/async SIR convergence pass.
Full regression passes **170/170 normal tests and 170/170 ASan/UBSan tests**. Schema
conformance passes in both builds: 51 valid fixtures plus 13 interaction and six
generator cases, 377 structural invalid cases and 220 semantic invalid cases.
The supported scope and remaining extensions are listed below. Remote CI has not
run; see [platform status](STATUS.md).

| Canonical model | Current evidence | Remaining gate |
|---|---|---|
| Boltzmann wealth | [Declared sequential variant](ABM_WEALTH.md): 32 frozen distribution gates against 2,048 Mesa trajectories, exact integer conservation, 864 addressed Python observations plus 16 high-address golden observations | Passed for the four declared cases and finite horizons |
| Schelling segregation | [Declared sequential variant](ABM_SCHELLING.md): 32 frozen distribution gates against 2,048 Mesa trajectories, exact membership/group/occupancy invariants, 864 addressed Python agent states plus 16 high-address golden states | Passed for the four declared cases and finite horizons |
| Boids/flocking | [Declared synchronous variant](ABM_BOIDS.md): 32 frozen distribution gates against 2,048 Mesa runs, 2,880 paired state values (max error 2.665e-15), 960 exact initializer values plus eight high-address values; periodic/reflection, population and speed invariants | Passed for the four declared cases and finite horizons |
| Sugarscape-lite | [Declared sequential variant](ABM_SUGARSCAPE.md): 32 frozen distribution gates against 2,048 Mesa runs, exact resource/population/occupancy budgets, 537 addressed agent states, 1,296 land values and 36 ledgers plus high-address histories | Passed for the four declared cases and finite horizons |
| Agent SIR | [Declared synchronous network variant](ABM_SIR.md): 32 frozen distribution gates against 2,048 Mesa runs, exact population/state/immunity invariants, 864 addressed states plus 16 high-address golden states | Passed for the four declared cases and finite horizons |

All five named canonical model families have passed their declared comparison
suites. [Continuous-time SIR and sync/async convergence](ABM_SIR_ASYNC.md) also pass:
32 asynchronous Mesa gates, 864 exact states and 356 event observations, six exact
full-joint-state refinement curves, 36 native joint-law gates and six finest-sync/async
comparisons. The convergence ensemble contains 18,432 native trajectories. The final
error-halving ratios range from .492 to .508. This is finite-graph convergence
evidence; the separate [M5 well-mixed ABM/SD limit](HYBRID_SIR_MEAN_FIELD.md) now has
its own passing four-case evidence and stored convergence plots.

[Graph generation](ABM_GRAPH_GENERATORS.md) passes 51 separate distribution/analytic
gates against pinned NetworkX; it does not substitute for the model gates above.
Statechart timeout, message, guard and rate semantics have deterministic tests and
288 exact native rate firings plus 297 CLI observations. Hierarchical states and
changing hazards remain unsupported.

## Declared CPU acceptance scope

- Typed columnar synchronous phases read common snapshots, with stable IDs, bounded
  lifecycle allocation and complete-step rollback. Asynchronous timers provide exact
  event timestamps, deterministic ordering and transactional timestamp processing.
- Grid/continuous spatial indexes, CSR contacts, addressed graph generators and
  bounded message topics have independent conformance and trajectory evidence.
- Flat statecharts cover timeout, message, guard and constant-rate transitions,
  including independent exact calendar/rate tests. The declared DEVS/IR subset
  includes population publication, scheduled/behavior lifecycle and scheduled graph edits.
- Five native canonical models pass their frozen Mesa comparison suites and exact
  invariants. Both SIR modes use the actual typed population engines; the asynchronous
  model reschedules its own global race after each transition.
- Full-state finite-graph probability laws show sync→async convergence as dt shrinks;
  native trajectories agree with each respective law under predeclared sampling bounds.

Acceptance is for this CPU scope. Hierarchical/parallel statecharts, entry/exit actions,
compaction and general changing hazards,
topic-handler lifecycle, behavior-generated rewiring and general typed hybrid graphs
remain explicit extensions. SIR-specific hazard recomputation does not implement a
general changing-rate statechart kernel. Native canonical models do not add new
IR component kinds. Thread-count determinism, acceleration and Tenstorrent remain
later work; local results are not remote CI or cross-platform certification.

Next in the milestone sequence: [M5 hybrid validation and remaining bridges](M5_ACCEPTANCE.md),
with SIR/Bass population and DES fluid-limit evidence available; per-agent stocks and remaining bridge contracts are next. M2 source-dialect work and M5–M8 are tracked separately in
[platform status](STATUS.md).
