# Typed populations, async timers and flat statecharts

This M4 increment adds three native C++ building blocks. The subsequent [DEVS and typed IR increment](ABM_TYPED_IR.md) exposes them through kernel execution and `mode: "abm"`. Existing adoption and agent-pool modes keep their current behavior.

## Typed synchronous population

`TypedPopulation<Tag>` owns a `PopulationStore<Tag>`, composing `des::RuntimeEntityStore<Tag>` with an optional value-owned network and reusing its column schemas, exact types, namespace-checked references, monotonic 48-bit IDs and retirement tombstones. Real, integer, boolean and string fields remain separate columns. The read-only `store()` provides records, columns and schema metadata. Use `spawn`, `update`, `retire` and `add_phase` for mutations.

A phase has signature `Record(Reference, const Store&)`. Every live agent reads the same store; all returned records commit together. Optional [lifecycle callbacks](ABM_BEHAVIOR_LIFECYCLE.md) collect births and self-retirement from that snapshot; retiring agents skip their record rule and publisher. Later phases see the preceding phase's results. Failure in any phase rolls back the whole step. Mutating the population from a rule or recursively stepping it raises an error. Callbacks must be pure and use the supplied snapshot; external side effects are not rolled back.

## Async population

`AsyncPopulation<Tag>` owns a typed store, an exact absolute clock and a binary timer heap. Its rule receives `const Timer&` and `const Store&`, and returns `Effects`. A timer contains time, agent reference, unique timer ID, kind and an opaque generation token. Equal-time timers order by agent ID, then timer ID. The rule sees the latest staged store, so these are sequential event semantics rather than synchronous phase semantics.

```cpp
struct Person {};
using Pop = ankurafathom::abm::AsyncPopulation<Person>;
using Store = Pop::Store;
Store agents(7, {{"completed", ankurafathom::des::FieldKind::integer}});
Pop population(std::move(agents), [](const Pop::Timer& timer, const Store& snapshot) {
    auto value = snapshot.record(timer.agent);
    value[0] = std::get<std::int64_t>(value[0]) + 1;
    Pop::Effects result;
    result.updates.push_back({timer.agent, std::move(value)});
    return result;
});
auto person = population.spawn({std::int64_t(0)});
population.schedule({2.5, person, "complete", 0});
population.run_until(2.5); // Includes events exactly at 2.5.
```

`Effects` contains explicit cancellations, record updates, retirements, births (each with optional initial timers), and timer schedules. `apply` commits one such batch and returns allocated agent and timer IDs. Retiring an agent cancels its remaining timers; scheduling for inactive agents, duplicate or missing cancellation IDs, backwards time and conflicting updates/retirements fail.

`step()` processes one complete timestamp and returns its timer trace, including newly scheduled same-time events. It stages all record changes, births, timer IDs and clock changes until the timestamp completes. A rule failure restores the whole timestamp. `run_until` retains earlier successful timestamps if a later one fails. Its positive per-timestamp event budget detects zero-time cycles without committing partial work. An idle horizon advances the clock but cannot pull a timer from even one representable instant after that horizon into the earlier sample.

## Statecharts

`Statechart<Tag>` is an immutable definition. It binds three distinct fields: integer state, real entry time and integer entry generation. New agents use generation `-1`; `start(agent, store, initial_state, time, draws)` returns initialization effects, applied through the population. State entry increments generation, including self-transitions.

Transitions support:

- **Messages:** explicit event strings, with guards and selection by lowest priority then transition name.
- **Timeouts:** fixed nonnegative duration after state entry. Positive durations must advance the floating-point clock.
- **Rates:** fixed nonnegative hazard, one addressed exponential draw on entry. Zero disables the transition. Each rate transition owns a distinct Philox stream.

`message` and `on_timer` return ordinary population effects. Guards inspect the pre-transition snapshot. A selected action returns a full record; the engine writes its reserved state/time/generation fields and validates all values before returning effects. The population then commits the fields and new timers together. Constant-rate draws use the agent ID and entry generation, so rollback/retry repeats the exact draw. Generations are explicitly limited to 0–65535 by the current address layout.

Timed transitions with false guards consume their timer without resampling until re-entry. Exiting a state makes its remaining timers stale; stale timers are ignored when dispatched. They still occupy calendar entries until then, so timer count is not transition count. A chart's timer prefix must be reserved for that chart on that agent. Callers deliver messages at the owning scheduler's current event time; no broker or external DEVS input is delivered implicitly.

[Phase/transition lifecycle actions](ABM_BEHAVIOR_LIFECYCLE.md) now add births and self-retirement, including chart initialization for newborns. Hierarchical or parallel states, entry/exit actions, changing hazards and storage compaction remain open. The DEVS wrapper, [spatial/network queries](ABM_INTERACTIONS.md), and [population topic delivery](ABM_TOPICS.md) are documented separately. [Publications emitted by phases/state transitions](ABM_GENERATED_PUBLICATIONS.md) now use transactional population outboxes. [Mutable population networks](ABM_MUTABLE_NETWORKS.md) now support scheduled edge edits and transactional membership. The declared canonical-model distribution and sync/async convergence gates now pass; see [M4 acceptance](M4_ACCEPTANCE.md) for scope and evidence.

## Correctness evidence

`abm_typed_population` checks typed Jacobi rotation/composition, schema failures, stable IDs, independent copies, cancellation, retirements, newborn timers, exact time boundaries, reentrancy and whole-timestamp rollback/retry. A 300-schedule workload with cancellations compares complete dispatch order and per-agent counts to an independent sorted-vector calendar.

The existing `SyncPopulation` also has a failed-spawn regression. Previously a throwing agent move advanced the next ID before insertion and broke the ID-to-row mapping on retry. Identity allocation now follows successful insertion; the test first reproduced the failure and now checks both retry identity and retirement by that ID.

`abm_statechart` checks hand-derived message/timeout traces, priority and declaration-order invariance, false guards, stale timers, typed action failures, retry, generation limits, zero-time cycles and invalid chart definitions. Twelve fixed seeds with 24 self-transitions each match **288 exact firing times** computed from the documented Philox address and inverse-CDF formula. This verifies draw consumption and scheduling; it does not replace Mesa distributional model comparisons.

```sh
ctest --test-dir build --output-on-failure -R '^(abm_|rng|des_runtime_store)'
ctest --test-dir build-sanitize --output-on-failure -R '^(abm_|rng|des_runtime_store)'
```

At this native execution checkpoint, the full normal regression passed **116/116**, with **52/52 affected ASan/UBSan checks** after the failed-spawn fix. Current totals and subsequent integration work are recorded in [status](STATUS.md). Remote CI has not run.
