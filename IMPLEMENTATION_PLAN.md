# AnkuraFathom — Implementation Plan v0.3

Platform name: **AnkuraFathom**. The repository is `AnkuraFathom/`; the command-line executable is `fathom`. Namespaces, packages, schemas, and user-facing documentation use `ankurafathom` or `fathom` as appropriate.

This document is written for the implementation agent. It is a plan, a spec, and a set of rules. Where it says MUST, do not deviate without recording an ADR (see §8.5) and flagging it to Arni. Where it says SHOULD, use judgment. Where it says DECIDE, it is your call — record it.

---

## 0. Read this first

### 0.1 Mission

Build an open-source, C++, formally grounded standalone hybrid simulation platform — system dynamics (SD), agent-based (ABM), discrete-event (DES), and any mix of the three in one model — for data-backed business decisions. It runs headless through its own CLI and APIs. Integration with other analytics platforms is deferred; later phases may run batched components on Tenstorrent hardware.

The platform is operated by AI agents, not by people (§1.4). Its job is to be a **calculator** (accurate, with known error bars), to be **traceable** (every number explains where it came from), and to produce **visuals for human validation** (generated, static, from the IR and the results — never an editor). Everything an agent needs to do — bring in data, build the model, run, validate, iterate — happens through code and structured I/O.

### 0.2 Sequencing at a glance

1. **Phase A (completed reference fixture):** the small standalone, deterministic synthetic two-practice model establishes business invariants for later integration testing. It is not the platform runtime and does not claim causal effects or uncertainty intervals.
2. **Phase 0–1 (current priority, CPU correctness):** finish the DEVS kernel, SD/ABM/DES libraries, hybrid bridges, RNG, declarative IR, experiment runner, and validation harness. Validate each against analytic solutions and pinned reference implementations; reproduce Phase A through this platform. Performance is not a goal; determinism and correctness are.
3. **Phase 2 (Mac, codegen + tensor backend on CPU):** IR → generated C++; batched tensor execution of SD and synchronous populations with an ensemble axis. Differentially tested against Phase 1, which becomes the oracle.
4. **Phase 3 (Tenstorrent):** tensor backend lowered to TT-Metal kernels; host keeps the event path. Differentially tested against Phase 2.
5. **Phase 4:** differentiable calibration on the tensor path.

Rewriting components between phases is acceptable and expected. What must survive every rewrite is the **IR**, the **semantics document**, and the **oracle test suite**. Treat those three as the product; the executors are replaceable.

### 0.3 How to work

- Write `docs/SEMANTICS.md` (§5) before writing primitives. Every primitive's behavior must be specified there first and tested against that spec.
- Test-first against oracles (§7). A primitive without a conformance test does not exist.
- Determinism is a test, not a hope: the same seed and build produce bit-identical numeric results regardless of thread count. Cross-platform and later backend comparisons use an explicit numeric tolerance policy; byte-identical Parquet files are not the contract.
- Do not optimize in Phase 1. No SIMD, no custom allocators, no clever queues. Plain, readable, obviously-correct code. Phase 1 code is the oracle for everything after it; its virtue is being easy to trust.
- Keep the dependency footprint small (§8.3). Every new dependency needs a one-line justification in `docs/DECISIONS.md`.
- When something in this plan is ambiguous or wrong, prefer the interpretation that keeps the IR backend-agnostic and the semantics DEVS-conformant.
- Work in small PR-sized increments. Each milestone (§9) has acceptance criteria; do not mark it done unless every criterion is green in CI.

---

## 1. Goals, non-goals, and evidence of value

### 1.1 Goals

- One kernel, three modeling vocabularies (SD, ABM, DES), and first-class bridges between them.
- A declarative, text-based model IR that humans, LLMs, and analysis tools can read and write.
- Formal DEVS semantics, with a published conformance suite.
- Bit-reproducible runs.
- Ensembles (replications × scenarios × parameter sweeps) as a first-class dimension of execution, not a loop around the simulator.
- Headless, API-first, embeddable: CLI + C ABI + thin Python binding; results as Arrow/Parquet.
- A CPU event backend now; a tensor backend on CPU next; the tensor backend on Tenstorrent after that.

### 1.2 Non-goals (for now)

- Graphical model editor, drag-and-drop, 2D/3D animation, GIS. (Generated static visuals for validation are in scope — §6.15 — an interactive authoring UI is not, ever.)
- Any human-facing data import/transform/wizard flow. Data enters through IR bindings to files and tables (§4.7); agents write those bindings.
- Domain libraries (pedestrian, rail, road traffic, fluids, material handling).
- Distributed (multi-node MPI) single-trajectory simulation. Single trajectories run on one host; parallelism is over the ensemble.
- Optimistic PDES (Time Warp). Conservative only, and only if profiling shows single-trajectory latency matters.
- FMI import. Deferred; the adevs FMI code remains in the pinned upstream snapshot for reference.
- QSS integration. Deferred to a later phase; fixed-step integrators are the reference semantics (matches XMILE/Vensim/AnyLogic).

### 1.3 Measurable platform claims

Each claim below becomes a test or benchmark in the repo. Passing these checks establishes AnkuraFathom's own capabilities; any comparison with another product requires a separate, matched benchmark and an explicit metric.

| # | Claim | Evidence in repo |
|---|-------|------------------|
| 1 | Formal semantics with conformance tests | `tests/conformance/devs/` passes; `docs/SEMANTICS.md` is complete |
| 2 | Bit-reproducible numeric results across runs and thread counts on the same build; declared tolerance across backends/platforms | `tests/determinism/` |
| 3 | Open, diffable, LLM-authorable IR with a JSON Schema and validator | `ir/schema/`, `tools/ir-lint` |
| 4 | Ensemble-native execution: 10⁴ scenarios as one invocation | `bench/ensemble/` |
| 5 | Gradient-based calibration on differentiable model paths in Phase 4 | `tests/calibration/` |
| 6 | Headless/API-first; results as Arrow | `bindings/python/`, `runtime/outputs/` |
| 7 | Accelerator backend (Phase 3) | `codegen/tt/`, `bench/tt/` |
| 8 | Hybrid correctness proven, not assumed: ABM↔SD mean-field agreement, DES↔SD fluid-limit agreement | `tests/hybrid/` |
| 9 | Machine-readable validation: structural + behavioral checks an agent can loop on, with applicable numerical and sampling uncertainty labeled by method | `fathom check`, `tests/validation/` (§6.13) |
| 10 | Full provenance: every result carries model hash, data hashes, seed, versions; any output value can be traced to the components and events that produced it | `runtime/provenance/`, `fathom explain` (§6.14) |
| 11 | Dimensional analysis: units declared in the IR and checked at lint time | `ir-lint --units` (§4.8) |
| 12 | Agent-operable end to end: data → model → run → validate → visuals with no GUI step; documented as an agent playbook | `docs/agent/` (§1.4) |

### 1.4 Operating principle: agents drive the platform

Humans will not import data, transform it, or wire up agents by hand; that is too slow and it is exactly the work AI agents are for. Consequences for the design, all of them binding:

- **The IR, the CLI, the C ABI, and the Python binding are the product surface.** There is no other way in. Everything an agent does maps to: write IR + data bindings → `fathom lint` → `fathom check` → `fathom run` → read results/validation JSON → `fathom viz`/`fathom report` for the human reviewer → iterate.
- **Every command speaks structured I/O.** JSON or Parquet in; JSON or Parquet out; stable exit codes; diagnostics with error codes, locations (JSON pointers into the IR), and, where possible, a suggested fix. An agent must be able to loop on `lint`/`check` output without a human reading it.
- **Token economy matters.** Agents pay per token to read and write the IR, so: the IR must be compact and regular; the agent-facing docs (`docs/agent/`) are short, example-heavy, and designed to be loaded whole into a context window; verbose outputs (traces, logs) go to files, and the CLI returns summaries with paths.
- **Validation is two-tiered.** Machine tier: `fathom check` and the per-run validation report — numeric, structured, gating. Human tier: generated diagrams and plots the reviewer looks at to confirm the model means what the agent thinks it means. The machine tier must be sufficient for an agent to decide "this model is sound enough to run"; the human tier is for trust, not for operation.
- **Accuracy is reported, not assumed.** Each output reports applicable numerical and sampling uncertainty, with a reason when an estimate does not apply (§6.13). Uncertainty in business assumptions is separate; reduced-precision runs report deviation from the f64 policy.
- **Traceability is a feature, not a log.** Provenance manifests, deterministic replay, and `fathom explain` (§6.14) exist so that an agent — or a human auditing an agent — can answer "why is this number what it is" mechanically.

---

## 2. Architecture overview

```
                 ┌──────────────────────────────────────────────────┐
                 │  Authoring: JSON IR (hand / LLM / xmile2ir tool) │
                 └───────────────────────┬──────────────────────────┘
                                         │ validate (schema + semantic lint)
                                         ▼
                 ┌──────────────────────────────────────────────────┐
                 │  IR (in-memory graph) + partitioner              │
                 │  assigns each component to a backend             │
                 └──────────┬───────────────────────────┬───────────┘
                            │                           │
              ┌─────────────▼─────────────┐  ┌──────────▼───────────────┐
              │ EVENT BACKEND (CPU)       │  │ TENSOR BACKEND           │
              │ DEVS kernel (adevs-derived│  │ batched over             │
              │ rewrite), async DES,      │  │ (scenario, agent)        │
              │ async populations, SD via │  │ SD, sync populations,    │
              │ tick atomics              │  │ integrators, Philox      │
              │ Phase 1: IR-driven generic│  │ Phase 2: CPU C++         │
              │ atomics (interpreter)     │  │ Phase 3: TT-Metal kernels│
              │ Phase 2: generated C++    │  │                          │
              └─────────────┬─────────────┘  └──────────┬───────────────┘
                            │      tick protocol / bridge tensors        │
                            └──────────────────┬───────────────────────┘
                                               ▼
                 ┌──────────────────────────────────────────────────┐
                 │  RUNTIME: experiment runner, ensemble scheduler, │
                 │  RNG streams, outputs → Arrow/Parquet, C ABI     │
                 └───────────────────────┬──────────────────────────┘
                                         ▼
                          CLI  ·  Python (nanobind)  ·  platform integration
```

Key idea: AnyLogic's "hybrid" is one DES clock with three vocabularies on top. fathom does the same thing with a formal kernel (DEVS) and adds a second executor for the parts that are tensors in disguise.

---

## 3. Locked design decisions

These are decided. Record them as ADR-001…ADR-010 in `docs/DECISIONS.md` on day one.

1. **DEVS is the kernel formalism.** Parallel DEVS with dynamic structure. Every primitive is specified as an atomic or coupled DEVS model. The kernel is a new C++20 implementation informed by adevs's abstract simulator (§6.1); a pinned upstream adevs source snapshot stays in `third_party/` as the Phase 1 reference oracle and source for studying the hybrid/event-detection algorithms. If upstream code is copied into the new kernel, preserve its license notice and record the provenance.
2. **Populations, not agents, are atomic models.** An agent population is one DEVS atomic wrapping N agents in struct-of-arrays layout. Never one atomic per agent. Internally the population may keep a timer heap for asynchronous agents, but the DEVS interface is at the population level.
3. **Two population modes in the IR:** `sync` (updates on a fixed dt in ordered phases; eligible for the tensor backend) and `async` (event-exact statechart timeouts; event backend only). A model may mix them.
4. **SD reference semantics are fixed-step.** Euler with model-specified dt is the reference (matches XMILE/Vensim/PySD). RK2/RK4 are options. Continuous components are DEVS atomics with `ta = dt`. QSS is deferred.
5. **Counter-based RNG.** Philox4×32-10, implemented in-repo with published test vectors. Every random draw in the IR is a named stream; the value of a draw is a pure function of `(seed, stream_id, scenario, replication, entity, step, draw_index)`. Define a collision-free, width-checked mapping into the 128-bit counter and 64-bit key before implementation; the informal tuple is not itself a valid packing. Draw values therefore do not depend on execution order or thread count.
6. **Ensemble is an IR/runtime axis.** `experiment.scenarios` and `experiment.replications` produce a scenario index that every backend treats as a batch dimension.
7. **IR is JSON with a JSON Schema**, versioned (`ir_version`), with a small pure expression language (§4.5). No embedded general-purpose code. A DSL front-end may come later; it compiles to this JSON.
8. **Precision policy is explicit in the IR.** Numeric fields declare `f64` or `f32`, and stocks/accumulators declare `accumulate: f64|f32`. CPU reference runs are f64. Reduced precision is validated against f64 with a documented tolerance policy (§7.6).
9. **Execution path is IR → generated code.** Phase 1 is a data-driven interpreter (generic atomics parameterized by the IR) because it is the fastest route to a trustworthy oracle. Phase 2 adds codegen to C++; Phase 3 adds codegen to TT-Metal. Interpreter and generated code must agree bit-for-bit at f64.
10. **Deterministic tie-breaking.** Simultaneous event delivery uses a documented stable order by `(time, component_id, sequence_number)`; within a population, by agent index. All inputs delivered to one atomic at the same time are gathered before its confluent transition. The ordering cannot replace DEVS confluent semantics. Documented in SEMANTICS.md and tested.

---

## 4. Model IR (v0)

Files: `ir/schema/ankurafathom-ir.schema.json`, `ir/include/ankurafathom/ir/*.hpp`, `docs/IR.md`.

### 4.1 Top level

```json
{
  "ir_version": "0.1",
  "name": "bass_diffusion_hybrid",
  "time": { "unit": "day", "horizon": 365, "dt": 0.25 },
  "precision": { "default": "f64" },
  "units": { "system": "si+business", "strict": true },
  "data": [ ... ],
  "parameters": [ ... ],
  "components": [ ... ],
  "outputs": [ ... ],
  "checks": [ ... ],
  "experiment": { ... }
}
```

- `time.dt` is the default continuous step; components may override with a coarser multiple (`dt_multiplier: k` ⇒ that component steps every `k·dt`). No component may step finer than `dt`.
- `parameters` are named scalars/vectors with defaults; experiments override them per scenario.
- `data` binds external tables to the model (§4.7); `checks` declares model-specific validation assertions (§6.13); `units` turns on dimensional analysis (§4.8).

### 4.2 Types

`f64`, `f32`, `i64`, `i32`, `bool`, `enum<...>`, `vec<T, n>`, `agent_ref<Population>`, `entity_ref<EntityType>`, `time`. Stocks and other accumulators carry `accumulate: "f64"|"f32"`.

### 4.3 Component kinds

Each component has `id` (unique, snake_case), `kind`, and kind-specific fields. The kinds:

**System dynamics**
- `stock` — `init`, `inflows: [flow ids]`, `outflows: [flow ids]`, `non_negative: bool`, `accumulate`.
- `flow` — `expr`.
- `aux` — `expr` (evaluated every tick; dependency-ordered; cycles without a stock are an IR error).
- `table` — piecewise-linear lookup: `x: [...]`, `y: [...]`, `extrapolate: "clamp"|"linear"`.
- `delay` — `input`, `duration`, `order: 1|3|n`, `type: "material"|"information"` (Vensim DELAY1/DELAY3/SMOOTH/SMOOTH3 semantics, exactly).
- `sd_group` — namespacing/containment only.

**Agent-based**
- `population` — `agent_schema: {field: type}`, `size: n | "dynamic"`, `mode: "sync"|"async"`, `dt_multiplier`, `phases: [phase]`, `statechart?`, `space?`, `network?`, `broker_topics?`.
  - `phase` — `{ "id", "rule": expr-over-agent-fields, "order" }`. In `sync` mode phases run in order each step; each phase reads the previous phase's committed state (no intra-phase read-after-write across agents — Jacobi semantics; document it).
  - `statechart` — `states`, `initial`, `transitions: [{from, to, trigger: timeout(expr)|message(topic)|condition(expr)|rate(expr), guard?, action?}]`. In `sync` mode timeouts are quantized to the population step; in `async` mode they are event-exact.
  - `space` — `grid {w, h, wrap, moore|von_neumann}` or `continuous {dims, bounds}`; `network` — `csr` adjacency or generator `{erdos_renyi|watts_strogatz|barabasi_albert, params, stream}`.
  - `broker_topics` — named message channels with a bounded per-step capacity (needed for the tensor backend).
- `agent_rule` helpers available in expressions: `neighbors(radius)`, `net_neighbors()`, `count(pop, pred)`, `sum/mean/min/max(pop, expr)`, `sample(pop, k, stream)`.

**Discrete-event process**
- `entity_type` — `schema: {field: type}`.
- `source` — `entity_type`, `interarrival: expr|stream-draw`, `limit?`, `start?`.
- `queue` — `capacity`, `discipline: fifo|lifo|priority(expr)`.
- `delay_block` — `duration: expr`, `capacity?`.
- `resource_pool` — `capacity: expr|agent-backed(population, field)` (bridge, see below).
- `seize` / `release` — `pool`, `units`, `priority?`, `preempt: bool`.
- `select_output` — `branches: [{prob|condition, to}]`.
- `sink` — records entity exit; optional `stats`.
- `conveyor`/`batch`/`split`/`match` — deferred; list them as `reserved` in the schema.
- `flow_link` — `from`, `to` (edges of the process graph).

**Events and control**
- `event` — `schedule: at(t)|every(expr)|condition(expr)`, `action`.
- `parameter` (top-level), `constant`.

**Bridges** (the important part)
- `aggregate` — `population`, `expr`, `op: sum|mean|count|min|max`, exposes a scalar readable by SD/DES expressions at each step. Semantics: value is the aggregate at the population's last committed step.
- `agent_stock` — declares that a population field is a continuous state integrated per agent with `inflow_expr`/`outflow_expr` (SD inside agents).
- `pulse` — `event`/`trigger` → adds `amount` to a `stock` at the event time (event → SD).
- `spawn` — event or DES sink → creates agents in a dynamic population; `despawn` — the reverse.
- `agent_pool` — `resource_pool` backed by a population field (e.g., capacity = Σ agent.available_hours); seize/release write back to agents through a broker topic (DES ↔ ABM).
- `entity_agent` — an `entity_type` whose instances are agents of a population (DES entities that carry statecharts).
- `sd_driven_rate` — a DES `source.interarrival` or `select_output` probability read from an SD aux (SD → DES).

### 4.4 Outputs

```json
{ "id": "revenue_ts", "kind": "timeseries", "expr": "cumulative_revenue", "every": 1.0 }
{ "id": "queue_len", "kind": "timeseries", "expr": "queue_len(intake_q)", "every": 0.25 }
{ "id": "won", "kind": "event_log", "source": "won_sink", "fields": ["value", "age"] }
{ "id": "infected_frac", "kind": "timeseries", "expr": "mean(people, state == Infected)", "every": 1.0 }
{ "id": "final_backlog", "kind": "scalar", "expr": "backlog", "at": "end" }
```

Output storage is long format `(scenario, replication, time, output_id, value)` in Arrow; event logs are their own tables. Wide pivots are a Python-side concern.

### 4.5 Expression language

Pure, side-effect-free, statically typed. Grammar (EBNF in `docs/IR.md`):

- literals, identifiers (component ids, agent fields via `self.x`, entity fields via `entity.x`, parameters), `+ - * / ^ %`, comparisons, `and or not`, `if c then a else b`, `min max abs exp log sqrt floor ceil clamp step pulse ramp`.
- random draws: `uniform(a,b,@s)`, `normal(mu,sigma,@s)`, `exponential(rate,@s)`, `bernoulli(p,@s)`, `poisson(lambda,@s)`, `triangular(...)`, `lognormal(...)`, `categorical([w...],@s)`; `@s` is a stream name declared in `streams`. A draw without a stream is an IR error.
- lookups: `table_id(x)`.
- population reductions and neighborhood queries (§4.3).
- `t` (current time), `dt`, `scenario` (index), `replication`.

No loops, no assignment, no recursion. Anything that needs those is a phase sequence or a statechart, not an expression. This restriction is what makes both backends possible; do not relax it.

### 4.6 Validation (`tools/ir-lint`)

Schema validation, then semantic checks: unique ids; type-checked expressions; dependency graph of `aux` is a DAG; every stream referenced is declared; `sync` populations only use constructs the tensor backend supports (a whitelist in `ir/tensor_eligible.md`); dt multipliers are integers ≥ 1; bridges reference compatible kinds; units consistent (§4.8); every `data` binding resolves and its schema matches. Emit machine-readable diagnostics (JSON): `{code, severity, pointer, message, suggestion?}` where `pointer` is a JSON Pointer into the IR. An LLM authoring loop consumes these directly; write them for that reader.

### 4.7 Data bindings (`data`)

Agents bring data in by writing bindings, never by a UI. A binding names a table (Parquet, Arrow IPC, or CSV; local path or URI), declares the expected schema, and says how the model consumes it. Loaders hash the file and record the hash in provenance (§6.14).

```json
{ "id": "consultants_init", "source": "data/consultants.parquet",
  "schema": { "group": "enum<Group>", "level": "i32", "available_hours": "f64", "tenure": "f64" },
  "use": { "kind": "population_init", "population": "consultant", "id_column": "employee_id" } }

{ "id": "pipeline_history", "source": "data/opportunities.parquet",
  "schema": { "created_at": "time", "group": "enum<Group>", "value": "f64", "stage": "enum<Stage>", "outcome": "enum<Outcome>", "closed_at": "time" },
  "use": { "kind": "entity_replay", "entity_type": "opportunity", "arrival_column": "created_at" } }

{ "id": "utilization_target", "source": "data/utilization_weekly.parquet",
  "schema": { "week": "time", "group": "enum<Group>", "utilization": "f64" },
  "use": { "kind": "calibration_target", "output": "utilization_ts", "join": ["group"], "loss": "rmse" } }

{ "id": "seasonality", "source": "data/seasonality.parquet",
  "schema": { "t": "time", "factor": "f64" },
  "use": { "kind": "exogenous_series", "interpolate": "hold|linear", "extrapolate": "hold" } }
```

Binding kinds (v0): `population_init` (agents from rows), `entity_replay` (DES arrivals replayed from a log instead of a stochastic source), `exogenous_series` (a time series readable in expressions as `series_id(t)`), `parameter_table` (parameters by key, e.g., per group), `calibration_target` (observed series matched to an output; used by `fathom check --fit` and Phase 4). Add kinds by ADR.

Rules: bindings are read-only and immutable during a run; schema mismatches are lint errors, not warnings; a model may declare a binding `required: false` with a synthetic fallback so the pilot runs before real data exists.

### 4.8 Units (`units`)

Every `stock`, `flow`, `aux`, `parameter`, agent field, and entity field MAY declare `unit` (e.g., `"USD"`, `"hours/week"`, `"people"`, `"1"` for dimensionless). With `units.strict: true`, every one MUST. `ir-lint --units` performs dimensional analysis over expressions (a flow into a `USD` stock must be `USD/day` given `time.unit: day`; `table` inputs/outputs carry units; `aggregate` inherits). Unit errors are lint errors. This is a large share of what "accurate as a calculator" means in practice and it is cheap for an agent to satisfy.

---

## 5. Semantics document (`docs/SEMANTICS.md`)

Must contain, before the corresponding code is written:

1. **DEVS definitions used**: atomic `(X, Y, S, δ_int, δ_ext, δ_con, λ, ta)`, coupled models, the abstract simulator (Nutaro's algorithms), dynamic structure rules.
2. **Time**: continuous f64 time in model units; how `dt` ticks are scheduled; the tie-break rule (§3.10); the guarantee that no two components observe different values for "now". A row stamped `t` is the state after all events and commits at `t`; an Euler step computed from the state at `t` commits at `t+dt`, and must not appear in an output stamped `t`.
3. **Per-tick order at time t** (the hybrid protocol):
   1. Commit continuous steps ending at `t`, calculated from the state at their prior tick. Euler: `S(t) = S(t-dt) + dt·(in − out)` using the documented pre-step snapshot.
   2. Deliver all events stamped `t` in stable order (DES, async populations, scheduled events); gather all simultaneous inputs to one atomic before its `δ_ext`/`δ_con`. Apply event-triggered pulses at `t` after the continuous step, so they cannot change an interval that has already ended.
   3. `sync` populations whose step ends at `t` run phases in order with Jacobi semantics, read the post-event SD snapshot, and commit.
   4. Bridges publish aggregates and flush broker topics after their source components commit. Outputs sampled at `t` observe this committed state. A bridge message generated at `t` that targets an earlier phase is consumed at the next eligible step, not retroactively.
   Document this as the mapping to DEVS — each of these is an atomic with its own `ta`, and the ordering is the confluent-transition policy. State explicitly which choices are conventions (and therefore what Vensim/AnyLogic do differently, if anything).
4. **Each primitive** (§4.3): its DEVS state, transitions, and output; edge cases (empty queue, capacity 0, dt not dividing horizon, table extrapolation, delay initialization, negative stocks).
5. **RNG contract** (§3.5): exact counter/key layout; how `draw_index` increments; what happens on dynamic spawn (entity ids are never reused within a run).
6. **Population semantics**: Jacobi phases; neighbor query snapshot semantics; broker capacity overflow behavior (error, never silent drop); async timer heap semantics.
7. **Determinism guarantees** and their scope per backend.
8. **Precision policy** (§7.6).

---

## 6. Component specifications

### 6.1 DEVS kernel (`devs/`)

- Rewrite of adevs's Parallel DEVS + Dynamic DEVS abstract simulator in C++20. Typed ports (Cadmium-style `Port<T>`), no `void*` messages. Keep Nutaro's algorithms; replace adevs's template idioms and threading.
- Event queue: binary heap keyed by `(time, component_id, seq)`. Ladder/calendar queues are Phase 2+ if profiling justifies.
- Dynamic structure: add/remove components and couplings at transition boundaries only.
- Coupled models flatten to a direct-coupling table at model load (adevs does this; keep it).
- Per-component `sequence_number` for tie-breaks.
- Avoid avoidable allocation on fixed-size hot paths. Dynamic populations, queues, and event logs may allocate; allocation behavior is a measured performance property, not a Phase 1 correctness gate.
- Threading in Phase 1: none inside a trajectory. Ensemble parallelism is a thread pool over independent trajectories (§6.7).
- Keep `third_party/adevs/` (BSD, notice preserved) building; the conformance suite runs the same models on upstream adevs and on `devs/` and compares traces.

### 6.2 Hybrid/continuous (`hybrid/`)

- `Integrator` interface: `step(state, derivative_fn, dt)`; implementations `Euler`, `RK2`, `RK4`. Euler is the reference.
- `EventDetector` for condition-triggered events on continuous state: check sign change of `g(state)` each tick; locate crossing by bisection to a tolerance; schedule the event at the located time. Port the algorithm from adevs's hybrid code. (Phase 1 may restrict to tick-aligned detection; document it.)
- Continuous atomic: `ta = dt·k`; `δ_int` integrates; outputs the committed state on its port so others can observe.

### 6.3 SD primitives (`primitives/sd/`)

- Stock, flow, aux, table, delay families with exact Vensim/XMILE function semantics (`DELAY1`, `DELAY3`, `DELAYN`, `SMOOTH`, `SMOOTH3`, `SMOOTHI`, `INTEG`, `PULSE`, `STEP`, `RAMP`, `LOOKUP` with clamp extrapolation).
- Aux evaluation order computed at load (topological sort); cycles are IR errors.
- `tools/xmile2ir`: XMILE → IR converter covering stocks/flows/aux/tables/delays/units-as-metadata. Use PySD's test-models corpus as the acceptance set (§7.3).

### 6.4 ABM primitives (`primitives/abm/`)

- `Population` atomic: SoA storage (`std::vector<T>` per field; Phase 1 may use a simple column store), phases, statechart engine, timer heap for `async`, space (grid with bucketed neighbor lookup; continuous space with uniform-grid binning), network (CSR), broker topics (per-step bounded message arrays).
- Dynamic size: free-list of agent slots; ids monotonic and never reused.
- Neighborhood queries return snapshots taken at phase start.

### 6.5 DES primitives (`primitives/des/`)

- Entity store (SoA by entity type, ids monotonic), process blocks as atomics with typed entity-ref ports, resource pools with seize/release queues and priorities, preemption optional (Phase 1: no preemption; error if requested).
- Statistics collectors per block: throughput, time-in-block, queue length time-average, utilization — computed exactly (time-weighted), since the analytic tests depend on them.

### 6.6 Bridges (`primitives/bridges/`)

One atomic per bridge kind, each with a conformance test in `tests/hybrid/`. Implement in this order: `aggregate`, `pulse`, `sd_driven_rate`, `agent_stock`, `spawn/despawn`, `agent_pool`, `entity_agent`.

### 6.7 Runtime (`runtime/`)

- Model loader: IR JSON → validated IR graph → instantiated DEVS model (Phase 1 interpreter path).
- Expression engine (Phase 1): parse to a typed AST, compile to a compact bytecode or closure tree; evaluate per tick. Must be exact f64 IEEE with a fixed evaluation order (left-to-right, no reassociation; compile with `-ffp-contract=off` and without fast-math so interpreter and codegen agree).
- Experiment runner: expands `experiment` into a scenario table (grid, Latin hypercube, Sobol, explicit list) × replications; schedules trajectories on a thread pool; each trajectory gets `(scenario, replication)` and derives its RNG keys from them. Results written per trajectory then merged. Progress and cancellation.
- Outputs: Arrow tables in memory; Parquet on disk; CSV as a fallback/debug format.
- C ABI (`runtime/include/ankurafathom/c_api.h`): `fathom_load(json) → model`, `fathom_run(model, experiment_json) → results`, `fathom_results_arrow(results) → ArrowArrayStream`, `fathom_free`. Stable and versioned; everything else (Python, future services) sits on this.
- CLI: `fathom lint model.json`, `fathom run model.json [--experiment e.json] [--out results.parquet] [--threads N] [--seed S]`, `fathom trace model.json` (full event trace for conformance tests), `fathom xmile2ir in.xmile`.

### 6.8 RNG (`rng/`)

- Philox4×32-10 implemented from the Random123 paper; tests against the paper's known-answer vectors and against Random123/NumPy outputs for the same counters.
- Publish an injective mapping of scenario, replication, entity, step, and draw index into the Philox key/counter pair with fixed integer widths and overflow diagnostics. The proposed tuple can exceed 128 counter bits; no truncation or hash collision may silently alias two draws. Test boundary values and dynamic spawns.
- Distributions: inverse-CDF or exact-rejection methods with fixed algorithms so every backend produces identical values from identical uniforms. Normal via Box–Muller with the both-outputs-consumed convention documented (or inverse CDF via a fixed-precision erfinv — DECIDE, then freeze). Poisson via inversion for small λ, fixed algorithm for large λ; freeze.

### 6.9 Python binding (`bindings/python/`)

nanobind over the C ABI. Surface: `Model.from_json`, `Experiment`, `run() -> pyarrow.Table`, `lint()`. Nothing else in Phase 1. This is a standalone scripting interface; platform integration is deferred.

### 6.10 Tensor backend (`tensor/`, Phase 2)

- Batched executor for `sync` populations, SD components, integrators, aggregates, and the parts of bridges that are tensor-expressible. Layout: `[scenario, agent, field]` with scenario outermost (so a Tenstorrent core owns one or more scenarios), SoA within.
- Ops needed (this list is the Phase 3 kernel list too): elementwise arithmetic/compare/select; segmented reductions (population→group aggregates); gather/scatter over CSR neighbors; grid stencils; Philox draw; integrator step; table lookup; statechart transition as masked select; broker message packing with bounded capacity.
- Phase 2 implementation: plain C++ loops, `-O2`, no SIMD intrinsics. It exists to prove the lowering and the tick protocol against the event backend, not to be fast.
- Tick protocol between backends (host-orchestrated): at each bridge boundary `t`, event backend drains events ≤ `t`; tensor backend runs its steps up to `t` (possibly several steps if no bridge fires between them — this batching is where Phase 3 throughput comes from); bridge tensors exchanged; repeat.

### 6.11 Codegen (`codegen/`, Phase 2–3)

- `codegen/cpp`: IR → C++ translation unit per model implementing the same atomics as the interpreter but with expressions inlined; compiled via CMake into a shared library exposing the C ABI. Differential test: interpreter vs generated, bit-identical traces and outputs at f64.
- `codegen/tt` (Phase 3): IR → TT-Metal host program + compute/data-movement kernels for the op list in §6.10. Design notes in §10.

### 6.12 Data layer (`runtime/data/`)

Implements §4.7. Arrow-native readers (Parquet, IPC, CSV via Arrow), schema validation against the binding, deterministic row order (sort by declared id column; never rely on file order), content hashing (SHA-256 of the file plus a canonical hash of the decoded table so re-encoding does not change provenance). `exogenous_series` are pre-sampled onto the model's dt grid at load with the declared interpolation so the hot loop does a lookup, not an interpolation.

### 6.13 Validation harness (`fathom check`, `runtime/validate/`)

Two layers, both emitting one JSON report (`validation_report.schema.json`), both designed for an agent to read and act on.

**Structural checks (no simulation):** lint (§4.6) plus: unreachable statechart states; DES graph reachability (every source reaches a sink; no dangling `flow_link`); dead components (never read); stocks with no outflow and unbounded inflow (warning); feedback loop inventory (list of loops through stocks with sign — this is also what the causal layer will consume); parameter ranges declared vs. used; tensor-eligibility report; estimated cost (agents, entities, steps).

**Behavioral checks (simulation):**
- *Extreme-condition tests* (Sterman's standard SD tests, automated): run with each parameter at its declared min/max and with stocks forced to zero; assert declared invariants (`non_negative`, flows go to zero when their source stock is empty, no NaN/Inf). Report which component first violated what.
- *Conservation*: for every declared closed system (`checks: [{kind: "conserved", stocks: [...]}]`) assert Σ stocks constant to tolerance.
- *Model-declared assertions*: `checks: [{kind: "assert", expr: "utilization <= 1.0", when: "always|end"}]`, `{kind: "monotone", output, direction}`, `{kind: "bounds", output, min, max}`.
- *Numerical accuracy*: integration error estimates only for outputs and intervals where step-halving is meaningful, with event alignment and unchanged model semantics; report the method and coverage. Monte Carlo standard errors apply to statistics computed from independent replications, not individual trajectories or event-log rows. Missing or inapplicable uncertainty fields have an explicit reason code.
- *Convergence*: for `sync` populations, agreement of summary outputs between the population dt and dt/2 (the sync↔async gap from §7.4, reported per model).
- *Fit* (`--fit`, when `calibration_target` bindings exist): loss per target, residual series, and a simple over/under-prediction summary.
- *Sensitivity smoke*: one-at-a-time ±10% on every parameter; report outputs' elasticities so an agent (and a human) can see which parameters matter and whether any produce absurd responses.

The report has a top-level `verdict: pass|warn|fail` and a per-check list with codes. `fathom run --require-check` refuses to run a model whose last check verdict was `fail` for the same IR hash.

### 6.14 Provenance and traceability (`runtime/provenance/`)

- **Run manifest** (JSON, written next to results, embedded as Parquet metadata): AnkuraFathom version and git commit; IR canonical hash; every data binding's hashes; experiment definition; seed; backend and precision policy; platform/toolchain; timestamps; validation verdict at run time. Matching manifests identify the same inputs and execution policy; numeric reproducibility is checked against the declared platform/backend tolerance contract.
- **Deterministic replay**: `fathom replay manifest.json` reproduces the run bit-for-bit (Phase 1 guarantees this on the same platform; §8.2 works toward cross-platform).
- **Event trace** (`fathom run --trace`): full `(time, component, port, value)` stream to Parquet; opt-in because it is large. Population and DES statistics are always recorded at output resolution.
- **`fathom explain results.parquet --output revenue_ts --at 120`**: walks the IR dependency graph and the recorded flows to produce a structured decomposition: the output's value, the flows that changed it over the window, each flow's inputs and their values, down to parameters and data bindings, plus the events (won opportunities, spawns, pulses) that fired into it. Output is JSON (for the agent) with an optional rendered tree (for the human). Phase 1 implements the SD/bridge chain; ABM/DES contributions are attributed at aggregate level (which agents/entities contributed how much) and extended later.
- **Lineage in results**: every output row carries `scenario`, `replication`, and the manifest id; every scenario row in the results carries its parameter values. No result is separable from how it was produced.

### 6.15 Visuals for human validation (`fathom viz`, `fathom report`)

Generated, static, from IR and results. No interactive editor, no animation. The purpose is for a human to look and say "yes, that is the model I meant" and "yes, that behavior is plausible."

- `fathom viz model.json --out dir/`: stock-and-flow diagram (SD), process graph (DES), statechart per population, bridge diagram (which layer feeds which), causal loop diagram with loop polarities from the structural check, dependency graph of an output on request. Emit Graphviz DOT + rendered SVG; also Mermaid text (cheap for an agent to embed in a summary for a reviewer).
- `fathom report results.parquet --out report.html`: single self-contained HTML — fan charts per output (median, 50/90% bands over replications), scenario comparison small multiples, phase plots for stock pairs, DES block utilizations and queue-length distributions, population state-share stacked areas, calibration overlays when targets exist, the numerical-accuracy and validation summary, and the run manifest. Plotly-free by default (inline SVG generated in Python or C++; DECIDE) so reports are archivable and diffable.
- Every visual is labeled with the IR hash and manifest id so a human is never looking at a picture of an unknown model.
- Phase 1 delivers `viz` (structure) and a minimal `report` (fan charts + validation summary); richer reports follow the pilot's needs.

---

## 7. Correctness validation — the oracle suite

This is the heart of Phase 1. Directory: `tests/`. Every item below is a CI test, tagged `[fast]` (< 1 s), `[stat]` (statistical, seconds–minutes), or `[nightly]`.

### 7.1 DEVS kernel conformance (`tests/conformance/devs/`)

- Zeigler's GPT (generator–processor–transducer) model; ping-pong; the "confluent" cases (external and internal at the same instant, both orders); zero-time-advance chains; dynamic add/remove mid-run; nested coupled models; message fan-out.
- Every model runs on upstream adevs (via a small adapter) and on `devs/`; full event traces `(time, component, port, value)` must be identical.
- DEVStone LI/HI/HO/HOmod benchmark models at several sizes: trace equality vs adevs; timing recorded (not gated) in `bench/`.

### 7.2 DES (`tests/des/`)

- M/M/1, M/M/c, M/G/1 (deterministic and Erlang service), M/M/1/K: mean queue length, mean wait, utilization vs closed forms (Little's law, Pollaczek–Khinchine, Erlang C). Gate deterministic equations and invariants exactly. For stochastic estimates, use a fixed seed set, a predeclared absolute/relative tolerance justified by a power calculation, and report confidence intervals as diagnostics; do not require every 95% interval to contain truth.
- Open Jackson network (3–4 nodes): per-node stats vs product-form solution.
- Priority queue and preemption-free seize/release ordering: deterministic trace tests.
- Cross-implementation: the same models written in SimPy and Ciw (pinned versions, implemented under `tests/oracles/des/`); compare distribution summaries under declared tolerances. KS statistics are diagnostic, not a single-run CI gate based on a p-value.

### 7.3 SD (`tests/sd/`)

- Analytic: exponential growth/decay, logistic, damped harmonic oscillator (2 stocks), first- and third-order delays step response — compare to closed form at the Euler-discretization level (i.e., test the *discretization* exactly, and separately test convergence to the continuous solution as dt→0).
- SIR, Lotka–Volterra, Bass diffusion vs high-accuracy reference (SciPy `solve_ivp` with tight tolerances, or SUNDIALS if installed) — convergence order checks for Euler (1st) and RK4 (4th).
- **PySD test-models corpus** (`SDXorg/test-models`, pinned commit): run `xmile2ir` on each supported model, simulate, compare to the corpus's expected CSV outputs within tolerance. Track coverage as a percentage; Phase 1 target ≥ 80% of models that use only supported functions.
- Vensim function semantics unit tests: each `DELAY*`/`SMOOTH*`/`PULSE`/`STEP`/`RAMP`/`LOOKUP` against hand-computed values for the first few steps.

### 7.4 ABM (`tests/abm/`)

- Schelling segregation (grid), Boids/flocking (continuous), Boltzmann wealth (network/random), Sugarscape-lite, agent-based SIR. For each: a Mesa implementation (pinned) as oracle; compare distributions of summary statistics across replications (KS/Anderson–Darling), plus invariants (wealth conservation exactly; population count exactly; no agent in two cells).
- Statechart engine: deterministic trace tests for timeouts, message triggers, guards, rate transitions (exponential with fixed stream ⇒ exact expected firing times).
- `sync` vs `async` mode: the same SIR model in both modes must converge in distribution as the sync dt → 0.

### 7.5 Hybrid (`tests/hybrid/`) — the tests that prove the platform's thesis

- **Bass diffusion**: SD version vs ABM version (each agent adopts with probability p + q·adopted_fraction). Mean ABM trajectory over replications must converge to the SD trajectory as N→∞; report the L2 gap vs N.
- **SIR mean field**: ABM SIR (well-mixed) vs SD SIR; same convergence test.
- **DES → SD fluid limit**: a source→queue→server pipeline feeding a backlog stock; as arrival counts scale up, the DES-driven backlog must converge to the fluid-model SD backlog.
- **Agent stocks**: per-agent continuous state (e.g., fatigue integrating workload) with an aggregate feeding an SD flow; conservation and agreement with a hand-built equivalent pure-SD model when agents are homogeneous.
- **agent_pool**: capacity computed from agents must exactly equal the equivalent static `resource_pool` when agent availability is constant — deterministic trace equality.
- **pulse**: a scheduled event adding to a stock; exact expected stock values.

### 7.6 Determinism and precision (`tests/determinism/`)

- Same seed and build, threads=1 vs threads=8 vs threads=64: bit-identical ordered numeric results and event traces. Compare decoded values, since Parquet metadata and writer details can change byte hashes without changing results.
- Interpreter vs codegen (Phase 2): bit-identical traces and outputs at f64.
- Event vs tensor backend (Phase 2) for tensor-eligible models: bit-identical at f64 when evaluation order is fixed (document the exact order used by both; where reductions differ in association, require fixed tree order). If bit-identity proves impractical for reductions, downgrade to ULP-bounded and record why in an ADR.
- Precision policy: f32 and (Phase 3) bf16/fp32-accumulate runs vs f64 oracle: per-output relative error bounds declared in `tests/precision/policy.json`; failures gate the build. Integer, bool, RNG, and statechart-state paths must be bit-exact regardless of float precision.

### 7.7 Robustness

- ASan/UBSan on every PR; TSan on the ensemble runner nightly.
- libFuzzer target on the IR loader and expression parser.
- Property tests (rapidcheck or hand-rolled): stock conservation under closed flow systems; non-negativity flags; event ordering invariants; population count invariants under spawn/despawn.

### 7.8 Pilot-shaped end-to-end (`tests/e2e/`)

The pilot model (§11) on synthetic data, run as a 1,000-scenario experiment; asserts run-to-run determinism, output schema, and a handful of sanity relationships (more capacity ⇒ not less delivered revenue, etc.).

---

## 8. Repository, toolchain, standards

### 8.1 Layout

```
ankurafathom/
  CMakeLists.txt, CMakePresets.json, vcpkg.json (or cmake/deps.cmake with FetchContent)
  docs/            SEMANTICS.md, IR.md, DECISIONS.md (ADRs), ARCHITECTURE.md, TENSTORRENT.md
  ir/              schema/, include/ankurafathom/ir/, src/ (loader, validator, expression parser/typechecker)
  devs/            kernel
  hybrid/          integrators, event detection
  primitives/      sd/  abm/  des/  bridges/
  rng/             philox, distributions
  runtime/         loader, interpreter, experiment runner, outputs, c_api
  tensor/          Phase 2
  codegen/         cpp/  tt/
  bindings/python/ nanobind package `ankurafathom`
  tools/           fathom CLI (lint, check, run, replay, explain, viz, report, trace), xmile2ir, trace-diff, synth-pilot-data
  models/          reference models (IR) + expected outputs
  tests/           conformance/ des/ sd/ abm/ hybrid/ determinism/ precision/ validation/ e2e/ oracles/python/
  docs/agent/      agent playbook: IR quick reference, binding examples, the check→run→explain loop, error-code catalog
  bench/           devstone, ensemble, tt
  third_party/     adevs (pinned source snapshot, BSD notice), Catch2, nlohmann_json, ...
```

### 8.2 Toolchain (Phase 0–2, Apple Silicon Mac)

- Apple clang from current Xcode or Homebrew LLVM (pin one; use the same in CI). C++20. CMake ≥ 3.28, Ninja, ccache.
- Flags: `-Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math`; debug builds with sanitizers; a `-O2` release preset. No `-march=native` in Phase 1, to reduce machine-specific behavior; cross-platform agreement is still measured against the numeric policy.
- CI: GitHub Actions, macOS-arm64 and ubuntu-x86_64 runners. Determinism tests must pass within each build; cross-platform decoded numeric results must meet a declared absolute/relative/ULP policy. Pin a portable libm only if a product requirement or measured drift justifies the cost.
- Python 3.12 for bindings and oracles; oracles pinned in `tests/oracles/python/requirements.txt` (PySD, SimPy, Ciw, Mesa, NumPy, SciPy, pyarrow).

### 8.3 Dependencies (keep it short)

Required: nlohmann/json (IR), Catch2 v3 (tests), Apache Arrow C++ (outputs; heaviest dependency — acceptable, but keep it isolated in `runtime/outputs/` behind an interface with a CSV fallback so the core builds without it). Optional: SUNDIALS (reference solutions and later stiff integration), nanobind (Python), rapidcheck (property tests). Vendored: pinned adevs reference snapshot, Philox (implemented in-repo, ~200 lines).

### 8.4 Coding standards

- No virtual dispatch per agent or per entity; per population/block is fine.
- Avoid avoidable heap allocation on fixed-size hot paths; profile allocation count in performance tests. Do not forbid allocation required for dynamic populations, unbounded queues, or event logs.
- All floating-point evaluation in a fixed order; no reassociation; comment any place where order is non-obvious.
- Every primitive: a header with a doc comment linking to its SEMANTICS.md section, a unit test, and at least one conformance/oracle test.
- clang-format and clang-tidy configs committed; run in CI.
- Error handling: IR/load errors are exceptions with structured diagnostics; runtime invariant violations are `FATHOM_ASSERT` (abort in debug, error return in release). No silent clamping or dropping — ever.

### 8.5 ADRs

`docs/DECISIONS.md`: one entry per decision — context, decision, consequences, date. Start with the ten in §3. Any deviation from this plan gets an ADR before the code lands.

---

## 9. Phases and milestones

Durations are rough; acceptance criteria are not.

### Phase A — Standalone economics reference, before the general kernel

**A0 — Two-practice synthetic pilot.** Implement a small C++ reference model in `AnkuraFathom/` with a CLI, versioned synthetic input, deterministic scenarios, and a documented monthly accounting order. It must distinguish time-and-materials (T&M) from fixed-fee work, and represent demand/bookings, backlog, labor capacity, recognized revenue, payroll and intervention costs. The same synthetic practice data feeds a baseline and four interventions. This is a model of the business mechanics, not yet the general DEVS runtime. Current implementation: `src/economics.cpp`, `models/synthetic_practices.csv`, and `docs/SEMANTICS.md`.

*Accept:* a no-slack T&M AI scenario cannot create revenue solely by reducing hours per engagement; a fixed-fee AI scenario can improve margin at unchanged contract value; added demand can monetize freed T&M capacity; headcount has one authoritative state; all stocks and rates have declared units; the CLI runs the supplied synthetic case and emits reproducible yearly practice and firm results. Tests check those relationships and the monthly accounting identities.

**A1 — IR and uncertainty design from the pilot.** Map A0 mechanics into the declarative IR, record any missing constructs, and define which parameters are observed, calibrated, estimated externally, or assumptions. Add synthetic planted truths and holdout targets before stochastic Monte Carlo claims. Causal effect estimates, if supplied later, are inputs with their own provenance; simulator fit alone does not identify causal effects.

*Accept:* an IR draft expresses the A0 model without embedded code; the deterministic A0 results remain a reference for Phase 1; uncertainty types and provenance are documented separately for numerical error, sampling error, and uncertain business parameters.

### Phase 0 — Foundation

**M0** — repo, CMake presets, CI on both platforms, sanitizers, clang-format/tidy; pinned adevs reference building and its own examples running; Philox with known-answer tests; IR schema v0.1 + loader + `ir-lint` with schema validation; `docs/SEMANTICS.md` skeleton with §5.1–5.3 written; `docs/DECISIONS.md` with ADR-001…010.
*Accept:* CI green on macOS and Linux; `fathom lint` rejects and accepts fixture models; Philox matches Random123 vectors.

### Phase 1 — Event backend and correctness

**M1 — Kernel.** `devs/` rewrite passing §7.1 with trace equality vs adevs on every conformance model and DEVStone.
*Accept:* zero trace diffs on the declared conformance suite; tie-breaking and confluent transitions tested; zero-time cycles diagnosed. Allocation count is measured but not a correctness gate.

*Status:* Complete for the declared CPU kernel subset; see [M1 evidence and scope](docs/KERNEL_CONFORMANCE.md). The report records 153 differential cases and nongating timing/allocation baselines. Broader custom-atomic rollback and structural APIs remain follow-on work.

**M2 — SD.** Primitives + integrators + `xmile2ir`; §7.3 tests.
*Accept:* analytic tests exact at the discretization level; convergence orders verified; ≥ 80% of the supported PySD corpus within tolerance; coverage report committed.

*Status:* In progress. Six reference models and 72 trajectories now verify nonlinear convergence, with pinned SciPy oracles and analytic/invariant checks; see [SD validation evidence](docs/SD_VALIDATION.md). STEP/PULSE/RAMP boundary semantics and bounded smoothing now have hand/analytic tests. The scalar Euler/RK4 XMILE importer now maps lookup tables and SMTH1/SMTH3/DELAY1/DELAY3 with constant or variable durations, with opt-in signed flows in native SD. Six upstream variants plus four custom PySD fixtures supply 43,598 stock comparisons including reordered variants; the full 67-file audit now records 8 imports and 59 rejections, including a stock-free auxiliary model checked against two exact Stella values. The independent eligibility classification records 8/8 assessable files imported (100%) and 7/8 with compatible complete historical references (87.5%), with 49 malformed files unassessable; see [coverage details](docs/XMILE_COVERAGE.md). See [import scope](docs/XMILE_IMPORT.md). Variable-duration hand, conservation, and convergence checks also pass for the supported order-1/3 Euler subset. General-order cascades (1–255), fixed whole-tick delays, and SMTHN/constant-duration DELAYN/DELAY mappings now have native and source checks. Nested fixed delays use an exact hand oracle because PySD sequential updates differ. RK4 selection now passes native stage tests, 1,272 SciPy-scored source observations with fourth-order convergence, and 132 upstream closed-form checks. The upstream RK4 model ships an Euler export; that conflict stays outside historical coverage. Bounded Euler clipping now supports explicit outflow priority and shared-flow conservation. Absolute start times, sign-filtered flow references without source-stock limits, bounded vendor metadata, and complete fixed-delay output comparison now pass. The 80% historical-agreement gate is met for the assessable subset. The analytic acceptance increment adds 56 closed-form convergence trajectories, 16 delay boundary trajectories and 45 discrete logistic observations. The [M2 acceptance review](docs/M2_ACCEPTANCE.md) records three §7.3 validation areas passing and function semantics partial; explicit variable-duration DELAYN cascade imports and grid-aligned source STEP/RAMP/PULSE mappings now pass. The cascade policy adds 1,386 independent expanded-stock PySD observations; the unexpanded history-dialect trajectory is diagnostic only. An explicit second-order history kernel now passes 101 historical Vensim observations and 1,386 PySD comparisons plus conservation and rollback checks. General history orders remain rejected because independent PySD conservation probes fail; see [history evidence](docs/DELAY_HISTORY.md). Explicit next-tick source inputs now support off-grid starts and fractional intervals, including multiple pulses per dt, with 19,584 exact-rational observations. The off-grid SD checkpoint passed 74/74 normal and ASan/UBSan tests; current platform totals are in [status](docs/STATUS.md). Other history orders still require authoritative references; the chosen off-grid policy does not establish universal vendor timing. The next DES increment now provides finite station queues and deterministic priorities; see M3 status below.

**M3 — DES.** Process blocks, resource pools, exact time-weighted statistics; §7.2 tests.
*Status:* CPU process core accepted locally; see [acceptance scope and evidence](docs/M3_ACCEPTANCE.md). [Finite station queues and non-preemptive priorities](docs/DES_QUEUES.md) now support C++ and declarative DES/hybrid runs, explicit terminal rejection records/counts, rollback, and time-weighted accounting. Hand schedules, 24 bag permutations, and 64 independent integer-clock traces cover admission, ordering, conservation, and statistics. Current full-suite and schema results are recorded in [status](docs/STATUS.md). [Acyclic process graphs](docs/DES_ROUTING.md) now add binary priority routing, merging, routed overflow and discard sinks, with hand conservation checks, transactional retry, and 32 declaration permutations. Hybrid models retain their linear path. [M/G/1 and finite-buffer validation](docs/DES_STATISTICAL_VALIDATION.md) adds eight analytical configurations and 336 preplanned replications, with uncensored cohort waits, occupancy distributions, and exact accounting checks. It also fixes fractional-clock drift found by the mixed-service workload. [Independent station service](docs/DES_SERVICE.md) now provides explicit addressed exponential draws in standalone and linear hybrid models. [Three-station Jackson validation](docs/DES_JACKSON_VALIDATION.md) checks upstream and downstream bottlenecks, 80 marginal/joint analytical metrics, and exact per-job/integral identities across 216 preplanned replications. This covers tandem networks. [Pinned SimPy/Ciw models](docs/DES_ENGINE_ORACLES.md) now reproduce all twelve statistical workloads: 672 replications per engine, 876,921 matching service records, 141,345 matching rejection records, and 444 native/reference gates for means and occupancy distributions. Offline comparison and separate pinned regeneration are wired into CI. [Binary probability routing](docs/DES_PROBABILITY.md) now adds addressed replay, schema/stream checks, transactional confluence, and 82 branching-network analytical gates across 120 preplanned replications. [Queue discipline evidence](docs/DES_DISCIPLINES.md) adds LIFO stations, priority/LIFO C++ resource grants, 96 independent resource schedules, and 96 exact native/SimPy/Ciw queue traces. [Typed entity storage](docs/DES_ENTITY_STORE.md) and [reference process blocks](docs/DES_REFERENCE_PROCESS.md) now provide native columnar records, queue/delay/seize/release composition, leases, resource statistics, and additional exact pinned-engine traces. [Typed declarative graphs](docs/DES_TYPED_IR.md) now add named schemas, standalone queue/delay and shared-resource blocks, multiway condition/categorical selection, admission priority expressions, generated arrivals/service, and capacity schedules. Static checks cover units, ownership paths, deadlock-prone acquisition order, and graph topology. The declarative path matches 120 pinned engine cases and 76,576 observations. Revisit semantics, mutable entity fields, and general typed hybrid graphs remain explicit extensions. Full regression passes 107/107 normal and 107/107 ASan/UBSan tests, with identical DES reports across builds. Broader language and hybrid extensions are listed explicitly in the acceptance review.
*Accept:* all closed-form comparisons meet predeclared numerical/statistical tolerances; Jackson network passes; distribution summaries vs SimPy/Ciw meet stated tolerances without p-value-only gates.

**M4 — ABM.** Populations (sync and async), statecharts, grid/continuous/network, brokers; §7.4 tests.
*Status:* CPU ABM core accepted locally; full regression passes 170/170 normal and 170/170 ASan/UBSan tests, with schema conformance passing in both builds. See [accepted scope and explicit extensions](docs/M4_ACCEPTANCE.md). [ABM foundations](docs/ABM_FOUNDATIONS.md) provide single-occupancy grids, uniform-bin continuous spaces, CSR networks and addressed graph generators, plus transactional bounded message topics. A pinned Mesa/NetworkX reference agrees on 7,504 exact neighborhood queries across 26 spatial/network cases and reproduces on regeneration. [Typed execution](docs/ABM_EXECUTION.md) now adds columnar Jacobi phases, an async timer heap with whole-timestamp rollback, and flat message/timeout/constant-rate statecharts. Native checks cover schema/lifecycle failures, independent sorted-calendar agreement, zero-time guards, deterministic retry and 288 exact rate firings. The [population DEVS wrapper and typed ABM IR](docs/ABM_TYPED_IR.md) now add canonical external-message confluence, transactional result publication, typed synchronous assignments and guarded async statecharts through the CLI. Three fixtures cover fixed and rate-driven behavior, with 297 independent Python-calendar observations across nine experiment contexts. [Declarative neighborhood queries and validated movement](docs/ABM_INTERACTIONS.md) now connect grids, continuous spaces and networks to phases/guards/actions, with 13 pinned Mesa/NetworkX interaction trajectories and 4,212 observations. [Population topic delivery](docs/ABM_TOPICS.md) now stages numeric requests/replies/broadcasts with population effects and rollback, with 24 independent scheduler scenarios and 840 observations. [Phase/statechart publications](docs/ABM_GENERATED_PUBLICATIONS.md) now share transactional outboxes, with deferred tick delivery, bounded timer/message feedback and 400 independent recurrence/calendar observations. [Scheduled agent lifecycle](docs/ABM_LIFECYCLE.md) now provides atomic retirement/birth batches, chart initialization, stable IDs and liveness-aware observations, with 480 independent calendar observations. [Phase/transition lifecycle actions](docs/ABM_BEHAVIOR_LIFECYCLE.md) now support parent-snapshot birth assignments, self-retirement, newborn chart initialization and transactional allocation limits, checked against 669 independent observations. [Population-owned mutable networks](docs/ABM_MUTABLE_NETWORKS.md) now support scheduled edge batches, lifecycle membership and query snapshots with rollback, checked against 873 independent observations. [Declarative ER/WS/BA generators](docs/ABM_GRAPH_GENERATORS.md) now initialize population networks with validated parameter overrides and disjoint RNG streams. They pass 2,808 exact addressed CLI observations and 51 predeclared statistical/analytical gates across 4,608 native and 4,608 pinned NetworkX graphs. [Boltzmann wealth exchange](docs/ABM_WEALTH.md) now supplies the first canonical-model comparison: a native sequential random-activation workload with transactional sweeps, exact integer conservation, 32 predeclared distribution gates against 2,048 pinned Mesa trajectories, 864 exact addressed observations and 16 high-address golden observations. [Schelling segregation](docs/ABM_SCHELLING.md) now adds sequential vacancy relocation on validated grids, exact membership/group/occupancy invariants and another 32 predeclared gates against 2,048 pinned Mesa trajectories, with 864 exact agent states plus 16 high-address golden states. [Boids/flocking](docs/ABM_BOIDS.md) adds synchronous steering on shared continuous snapshots, periodic/reflecting boundaries and transactional finite-state/speed validation, with 32 more predeclared Mesa distribution gates across 2,048 runs per engine, 2,880 paired values (maximum error 2.665e-15) and 960 exact initializer values plus eight high-address values. [Sugarscape-lite](docs/ABM_SUGARSCAPE.md) adds sequential axial movement, harvest/metabolism, starvation and capped regrowth with atomic population/land/ledger rollback and exact resource conservation. Its 32 predeclared Mesa distribution gates pass across 2,048 runs per engine; independent addressed recurrences match 537 agent states, 1,296 land values and 36 ledgers plus high-address histories. [Synchronous network SIR](docs/ABM_SIR.md) adds fixed-contact infection/recovery through typed snapshot phases, whole-tick rollback and exact population/state/immunity invariants. Its 32 predeclared Mesa gates pass across 2,048 runs per engine, with 864 addressed agent states and 16 high-address golden states. [Continuous-time SIR and sync/async convergence](docs/ABM_SIR_ASYNC.md) add a native asynchronous direct-Gillespie model with transactional event/horizon processing, 32 additional Mesa gates, 864 addressed states and 356 event observations. Six full-joint-state refinement curves, 36 native joint-law gates and six finest-sync/async comparisons pass across 18,432 native trajectories; final error-halving ratios are .492–.508. [M4 acceptance](docs/M4_ACCEPTANCE.md) records all five canonical families and the separate sync/async convergence gate passing for the declared CPU scope, with full regression evidence and explicit extensions. Topic-handler lifecycle and behavior-generated graph edits remain extensions.
*Accept:* invariants exact; distributional agreement with Mesa oracles; sync↔async convergence test passes.

**M5 — Bridges and hybrid.** All bridge kinds; §7.5 tests.
*Status:* In progress; [acceptance tracking](docs/M5_ACCEPTANCE.md) inventories all seven bridge kinds and six §7.5 model gates. The [well-mixed SIR/SD suite](docs/HYBRID_SIR_MEAN_FIELD.md) passes four frozen population curves at N=40/160/640, with 3,072 individual runs and 3,072 independent aggregate CTMC runs, 168 distribution comparisons, 84 finest-N mean gates, four native RK4 checks, 96 exact complete-graph equivalence cases and 64 independent addressed count snapshots. Plots, numeric gaps and provenance are stored under `docs/figures/hybrid/`. The [Bass suite](docs/HYBRID_BASS_MEAN_FIELD.md) now adds six population curves across two time steps, 4,608 runs per stochastic engine, 90 independent binomial comparisons, 30 analytic mean/variance gates, 30 finest-N mean gates and 72 exact count snapshots. Five-step Euler/RK4 refinement against closed-form SD separates the fixed-dt population limit from continuous-time convergence; plots and numeric gaps are stored alongside SIR. The [DES fluid suite](docs/HYBRID_DES_FLUID_LIMIT.md) adds four constant-rate queue regimes at N=8/32/128, 3,072 coupled native and independent birth/death runs per engine, 180 distribution comparisons, four scaling curves, 48 addressed count snapshots and 120 declaration-order traces. Native signed pulse stocks conserve whole-job backlog exactly; reflected native SD agrees with the independent fluid law. The [native typed agent-stock bridge](docs/HYBRID_AGENT_STOCKS.md) now passes 108 analytical snapshots, 720 individual values, three first-order Euler refinement curves and 576 homogeneous pure-SD equivalence snapshots, with aggregate flows, conservation and whole-step rollback. The [typed aggregate endpoints](docs/HYBRID_TYPED_AGGREGATE.md) now publish filtered sum/mean/count/min/max and drive SD flow callbacks with explicit snapshot timing and rollback; 1,008 exact-rational snapshots and 10,080 scalar comparisons pass across both declaration orders. The [agent-stock DEVS publisher](docs/HYBRID_AGENT_STOCKS_ATOMIC.md) composes continuous agents, snapshot aggregates and held-input SD across six declaration orders and four clock ratios: 3,024 exact-rational snapshots and 35,280 scalar comparisons pass. The [bounded declarative agent-stock/SD mode](docs/AGENT_STOCK_SD_IR.md) now passes 2,728 independent exact-rational scalar comparisons over 24 generated models, plus 300 experiment observations; expression units, filters, parameter overrides and differing clocks are checked. The [dynamic agent-stock owner](docs/HYBRID_DYNAMIC_AGENT_STOCKS.md) now adds lifecycle/pulse ordering, off-grid integration and explicit inventory receipts, with 7,872 independent snapshots and 818,496 exact scalar checks. General declarative bridge graphs remain open. [Typed event pulses](docs/HYBRID_TYPED_PULSES.md) now support payload-dependent signed vectors with canonical keys, replay checks and simultaneous application; 3,024 exact-rational snapshots pass across 144 configurations. [Typed lifecycle events](docs/HYBRID_LIFECYCLE_BRIDGE.md) now drive population-owned births/retirements with destination and replay checks; 8,064 independent scheduler snapshots pass across 384 configurations. [Shared entity-agent ownership](docs/HYBRID_ENTITY_AGENT.md) now joins statecharts and resource processes through one authoritative identity and immutable dispatch snapshots; 18,000 exact scheduler snapshots pass across 720 configurations. [Scalar-driven typed sources and routing](docs/HYBRID_SIGNAL_RATES.md) now pass 9,408 coupled runs, 41,418 independent event comparisons and fourteen Poisson count gates, including residual hazard preservation and high-address boundaries. The [typed workforce owner](docs/HYBRID_TYPED_AGENT_POOL.md) now passes 21,168 exact independent snapshots across 432 pool/aggregate/SD configurations, including availability, lifecycle, Jacobi phases and transactional broker writeback. The existing sync/async typed ABM can now publish through a native result adapter, validated by 336 exact lifecycle/command/timer snapshots across all 24 graph orders.
*Accept:* Bass and SIR mean-field convergence curves committed as plots + numeric gaps; DES→SD fluid limit passes; deterministic bridge trace tests pass.

**M6 — Runtime.** Experiment runner (grid/LHS/Sobol), thread-pool ensembles, Arrow/Parquet outputs, data layer (§6.12) with `population_init`, `exogenous_series`, `parameter_table`; run manifest and `fathom replay` (§6.14); C ABI, CLI, Python binding; §7.6 thread-count determinism; §7.7 robustness.
*Status:* In progress. [Ordered CPU ensembles](docs/RUNTIME_EXPERIMENTS.md) now provide private trajectory execution, canonical merge/progress, cooperative cancellation, deterministic failure selection, strict CLI thread/seed controls and atomic CSV publication. The 1,000-scenario/three-replication CLI gate passes at 1/2/7/16 threads (81,000 identical CSV observations); native tests pass at 1/2/7/32 threads, including ThreadSanitizer. The fixed floating-point policy now propagates to interpreter consumers. [Grid/LHS/Sobol expansion](docs/RUNTIME_SCENARIO_DESIGNS.md) now adds 510,964 independent coordinate comparisons, 1,156 LHS gates and 15,570 analytic CLI observations, plus a dedicated experiment schema. [Arrow/Parquet outputs](docs/RUNTIME_OUTPUTS.md) now provide typed observation tables, IPC/Parquet files, optional SDK builds and failure-safe publication; 622,546 decoded rows agree exactly across formats and 1/8/64 threads. [Validated data snapshots](docs/RUNTIME_DATA.md) now load local CSV/Parquet/IPC with exact schemas, sorted unique keys, owned values and independent file/canonical SHA-256 identities. [Native population initialization](docs/POPULATION_DATA_BINDING.md) now maps validated rows transactionally into typed agents with source-key receipts, checked units/conversions and independent cross-format evidence. [Native series and parameter inputs](docs/DATA_INPUT_BINDINGS.md) now add deterministic hold/linear sampling and exact keyed parameter lookup, tested against rational interpolation and real SD execution. General hybrid loading, general data-binding composition, broader provenance/replay and cross-platform package qualification remain open; subsequent increments are detailed below.
*Accept:* 1,000-scenario experiment has identical decoded numeric results across thread counts on the same build and meets the cross-platform numeric policy; `fathom replay` reproduces the ordered numeric results under the same policy; data-binding hashes change when and only when the data changes; fuzzers run 10 min clean nightly; `pip install -e . && python -c "import ankurafathom"` works from the repository root (where `pyproject.toml` lives).

The [declarative parameter-table increment](docs/PARAMETER_DATA_IR.md) now resolves
local CSV/Parquet/IPC rows into standalone SD defaults before component validation.
It checks typed keys, parameter references, dimensional units and duplicate targets;
explicit scenario overrides take precedence. Loaded models retain source/hash/key
receipts and run without rereading data. CLI outputs protect source aliases.
[Declarative exogenous series](docs/SERIES_DATA_IR.md) now expose unit-checked
`series_id(t)` calls with hold/linear interpolation and endpoint holding. Euler,
RK4 stages, outputs and delays read owned snapshots; independent rational-stage
comparisons cover formats, grids and thread counts.
[Declarative population initialization](docs/POPULATION_DATA_IR.md) now maps local
tables into sync/async typed ABM populations with canonical IDs, complete source
receipts and existing model invariant checks. All three M6 data-input kinds have
bounded declarative paths. General composition, optional fallbacks
and cross-platform package qualification remain open; the interfaces are implemented below and M6 is incomplete.
[Strict local manifests/replay](docs/RUN_MANIFESTS.md) now provide default sidecars for CLI file runs,
captured raw/canonical input identities, expanded scenarios, build/precision policy
and independently checked ordered IEEE-754 result hashes. Replay permits a thread
change, verifies inputs/build policy and compares numeric identity before output.
It requires original local inputs. Version 0.2 manifest-backed outputs carry row
lineage and effective parameters; Arrow/Parquet embed the full manifest, including
empty results. Replay preserves original provenance across output/thread changes.
`verify-results` checks saved schema 0.2 CSV/Arrow/Parquet artifacts against an
explicit manifest without re-execution, including numeric bits, schema and lineage.
`verify-results results.parquet --embedded` also verifies binary artifacts using
their embedded manifest and one decoded table, without a sidecar or original inputs.
Legacy verification and full §6.14
acceptance remain open.

[C ABI 1 foundation](docs/C_API.md) now provides a shared library with opaque
model/result ownership, length-delimited JSON and file loading, single/ensemble
execution using the same validators, ordered observation access and numeric
identity, and allocation-free thread-local diagnostic storage. C callers exercise
the boundary and compare exact results to CLI runs. CPU Arrow C Stream export now
preserves schema 0.1 observations with independent stream/array/schema lifetimes,
including an explicit unavailable status in CSV-only builds. C ABI provenance
and execution callbacks are now exposed below; broader platform qualification remains open.

The optional [nanobind Python package](docs/PYTHON_API.md) now provides
`Model.from_json`, immutable experiment specifications, `run() -> pyarrow.Table`,
and loader-backed `lint()`. It uses only the C ABI, releases the GIL during native
work and transfers owned observations through Arrow capsules. Structured errors
retain C status/code/pointer/truncation. Normal, ASan/UBSan and CSV-only contracts
cover exact C parity, concurrent use and independent buffer lifetime. Build-directory
use is supported. [Local packaging](docs/PACKAGING.md) now builds a source archive
and platform-specific Python wheel with the private C runtime, pinned PyArrow
dependency, relative loader paths and third-party notices. A separate CMake install
exports the native C SDK and CLI. Isolated installation/relocation passes the Python
contract, an installed C11 consumer and an analytic CLI check on macOS. Broader
platform release qualification remains open; no package has been published.

[C/Python run provenance](docs/API_PROVENANCE.md) now captures stable manifests
with input/data identities, expanded scenarios, execution/build policy and ordered
numeric hashes. The C JSON getter works without Arrow; a second stream exporter
and Python `run(..., provenance=True)` add schema-0.2 lineage and embedded receipts.
Manifest version 0.3 explicitly represents memory inputs/output, while existing
file-manifest formats remain unchanged. Saved Arrow/Parquet artifacts verify through
the CLI; version-0.3 replay is explicitly unsupported.

[C/Python execution callbacks](docs/API_EXECUTION_CALLBACKS.md) now expose ordered
calling-thread progress and cooperative cancellation at trajectory boundaries.
Cancelled runs publish no partial results and join active workers; Python callback
exceptions preserve their original object and traceback. Existing ABI-1 layouts and
numeric execution remain unchanged. Intra-trajectory cancellation and general binding composition remain open. M6 is not complete.

CLI `run --out PATH` now creates `PATH.manifest.json` and schema-0.2 lineage by
default. `--manifest` overrides the destination; `--no-manifest` explicitly selects
legacy output. Stdout and replay retain their prior behavior. Existing sidecar
entries reject before output replacement; input-alias checks and staged publication
apply equally to automatic and explicit destinations. Publication remains two
separate renames, with the manifest committed last.

[Portable file-backed bundles](docs/PORTABLE_REPLAY.md) now capture original model,
experiment and CSV/Parquet/IPC data bytes into a staged directory. Relocated replay
requires no original inputs, retains the original manifest ID, validates raw and
canonical identities and expanded scenarios, and enforces the existing strict
build-policy/numeric checks. Fixed members, bounded file sizes, symlink/inventory
checks and output-alias protection apply. API memory receipts, embedded-manifest
replay and cross-platform tolerance remain outside this increment. Next: M6
acceptance gates and remaining data-binding gaps. The [M6 acceptance audit](docs/M6_ACCEPTANCE.md)
records completed contracts and open requirements, including cross-platform numeric
evidence, general bindings and missing CLI interfaces. Root editable installation
now passes an isolated local import/analytic/error contract; other platforms remain open.
The [fuzz harness](tests/fuzz/README.md) now provides instrumented expression/loader
targets and scheduled CI. ADR-A08 bounds expression source/nesting/tree depth after
a sanitizer corpus reproduced a stack overflow; acceptance results are recorded
in the session log.

**Current priority (2026-09-28):** defer further reporting work at the user's
request. Work through small, independently checkable examples before expanding
the pilot. [Pricing and staffing cases](examples/worked_validation/README.md)
now validate complete trajectories against exact arithmetic and an explicit event
ledger, including wrong-model controls. This changes sequencing, not acceptance
criteria; general reporting remains a later requirement.

The requested [benchmark campaign](tests/benchmarks/README.md) now extends this
priority: rerun existing analytic/reference tests, add exact C22/Life/random-walk/
lattice/coupling workloads, and retain an explicit inventory of unimplemented
external models. Passing custom adapters does not close whole-library or
declarative-language coverage. Empirical calibration/parameter recovery remains
separate from engine validation.

Completed validation increments add Isaac-2011 StupidModel computational behavior
across versions 1–16 and C21 event-contact bouncing-ball cases. Their adapters
exercise existing native primitives; original-data docking and the general
state-event/DAE capability remain open. GUI and display testing are excluded from
this correctness campaign at the user's direction. Acceptance focuses on engine
results, timing, state transitions, invariants, convergence and independent oracles.
See the benchmark scope before treating
these as full external-suite conformance.

C17R now has a bounded hexagonal epidemic adapter: exhaustive local transition
checks, thirty full-state trajectories with once-only interventions, and independent
ODE accuracy/convergence checks. Spatial policies are benchmark code over native
population phases. Broader spatial/parameter studies and ODE intervention events
remain open; see the campaign's explicit scope.

**M7 — Agent interface and validation.** `fathom check` structural and behavioral layers (§6.13) including extreme-condition tests, conservation, model-declared assertions, applicable step-halving estimates, and Monte Carlo standard errors on replicated statistics; `ir-lint --units` (§4.8); `entity_replay` and `calibration_target` bindings with `check --fit`; `fathom explain` for the SD/bridge chain; `fathom viz` structure diagrams and minimal `fathom report` (§6.15); `docs/agent/` playbook with worked examples of the full loop.
*Status:* Bounded [native declared validation](docs/DECLARED_VALIDATION.md) is now
implemented for standalone SD: conservation, output bounds, monotonicity and
unit-checked arithmetic assertions, with scenario/time failure locations and
explicit report coverage. `run --require-check` recomputes checks against the
captured model/experiment before publishing. The synthetic pilot runner retains
validation reports and requires matching input/result identities. The
[current preview](artifacts/pilot-diagrams/results/report.md) passes all declared
rules. [Native standalone SD explanations](docs/EXPLANATIONS.md) now verify
recorded run identities, reconstruct Euler/RK4 flow accounting, and retain
dependencies/source receipts in JSON or self-contained HTML. Bass diffusion and
the pilot have independent accounting comparisons. Clipped flows and hybrid
bridges remain outside this explanation subset. [Native SD structure diagrams](docs/VISUALIZATIONS.md)
now emit SVG/DOT/Mermaid and graph JSON, with model hashes and optional verified
manifest input linkage. Stock/flow and output-focused dependency views are
linked from each pilot practice. This does not complete the
broader structural inventory, automatic extremes,
step-halving/uncertainty, fit/sensitivity, hybrid validation, bridge explanation, hybrid diagrams/general native reporting or
the independent-agent acceptance exercise. A partial [playbook](docs/agent/README.md)
is available; M7 remains in progress.
*Accept:* `tests/validation/` has a deliberately broken model per check kind and each is caught with the right code and pointer; an agent (run one — Claude Code or equivalent) given only `docs/agent/` and the CLI can go from a Parquet table + a one-paragraph brief to a passing `check` and a rendered report without human help — this is a real test, run it and record the transcript; `explain` on the Bass model produces the expected decomposition; every diagram carries the IR hash.

**M8 — Pilot model v0.** Reproduce and extend the Phase A reference in the IR with a synthetic data generator producing the Parquet tables the bindings expect; §7.8.
*Status:* A [working economics preview](examples/ankura_pilot/README.md) now translates
the two-practice Phase A model into general standalone SD, binds synthetic Parquet
practice/calendar tables, and runs all sixteen intervention combinations over
sixty months through CLI and Python. Monthly/yearly values, NPV and payback are
compared against the separate Phase A implementation. The
[review artifact](artifacts/pilot-diagrams/results/report.md) is available. This early
end-to-end preview prioritizes the user's pilot goal; it does not close M6's
remaining qualification/composition gates or M7's broader validation/bridge-explanation/viz/report
requirements. Native declared checks now pass for the preview. M8 is not accepted.
*Accept:* runs end-to-end from CLI and Python with data bound from files; `check` verdict is `pass`; outputs land in Parquet with the documented schema and manifest; sanity relationships hold; `viz` and `report` outputs reviewed by Arni.

Phase 1 exit: the platform is a trustworthy, slow, correct, agent-operable oracle.

### Phase 2 — Codegen and CPU tensor backend

**M9 — C++ codegen.** IR → generated C++ → shared lib via the C ABI.
*Accept:* bit-identical to the interpreter on every model in `models/` and `tests/`; build time per model recorded; manifests record which executor produced the results.

**M10 — Tensor backend (CPU).** `sync` populations, SD, integrators, aggregates, eligible bridges as batched ops over `[scenario, agent]`; partitioner assigns components; tick protocol implemented.
*Accept:* bit-identical (or ULP-bounded per ADR) to the event backend on all tensor-eligible models; hybrid models with mixed assignment pass §7.5; f32 runs meet `precision/policy.json` against f64 and the deviation is reported in the run's accuracy block.

**M11 — Ensemble benchmarks.** `bench/ensemble/`: scenarios/second for the pilot model and for Bass/SIR at N = 10³…10⁶ agents, both backends; committed as baseline numbers.
*Accept:* numbers exist and are reproducible; no correctness regressions.

### Phase 3 — Tenstorrent

**M12 — Device design.** `docs/TENSTORRENT.md` (§10) written and reviewed by the kernel team before code: host/device split, memory layout, tile padding, kernel list, precision policy, differential test plan.

**M13 — TT codegen for the op list.** Kernels for §6.10 ops; host program generator; device-resident multi-step batching between bridge boundaries.
*Accept:* every op has a unit test vs the CPU tensor backend (bit-exact for integer/bool/RNG; within policy for floats); Bass and SIR run device-resident.

**M14 — Hybrid on device.** Full tick protocol with the host event backend.
*Accept:* pilot model and all tensor-eligible hybrid tests pass differential testing vs Phase 2 within policy; manifests and `explain` work unchanged on device-produced results.

**M15 — Scale.** Single-card, then multi-card, then Galaxy; ensemble throughput vs CPU baseline.
*Accept:* scaling curves committed; determinism across device counts.

### Phase 4 — Differentiable path

**M16** — forward-mode sensitivities then adjoint through integrators and `sync` phases on the tensor backend; gradient-based calibration of the pilot model against `calibration_target` bindings; finite-difference checks; sensitivities surfaced through `check` and `explain`.
*Accept:* gradients match finite differences within tolerance on the test models; calibration recovers known synthetic parameters; `explain` reports ∂output/∂parameter alongside the flow decomposition.

---

## 10. Tenstorrent phase — design notes for `docs/TENSTORRENT.md`

Written for the kernel team to refine; the agent should draft, not finalize.

- **What runs where.** Host CPU: DEVS kernel, DES, `async` populations, scheduling, I/O. Device: everything tensor-eligible, batched over scenarios and agents. The event path is control-flow-heavy and pointer-chasing; it does not belong on the compute units.
- **Batching between bridges.** Throughput comes from running many device steps without a host round-trip. The partitioner should compute, per model, the maximal device-resident step count between bridge boundaries and report it; models whose DES↔tensor coupling fires every step will be host-bound, and the report should say so.
- **Layout.** Scenario-major so a core (or core group) owns whole scenarios; SoA fields tiled and padded to the device's native tile size; agent counts padded with a validity mask. Reductions (population→group aggregates) as segmented reductions with fixed tree order so results are reproducible across core counts.
- **RNG on device.** Philox is designed for exactly this: each draw is computed from its counter with no shared state. Implement the same counter packing as the CPU; test bit-exactness of the raw uniforms across CPU and device before anything else.
- **Precision.** Decide per tensor class: stocks/accumulators fp32 (or higher if the device path allows); agent scalar fields as the IR declares; statechart states and counts as integers. Validate the whole model against the f64 CPU oracle under `precision/policy.json`, not op-by-op only — accumulated drift over long horizons is the failure mode to watch.
- **Kernel list** = §6.10 op list. Prioritize by what the pilot model and the Bass/SIR tests need: elementwise, masked select, segmented reduce, Philox, integrator step, table lookup. Gather/scatter over networks and grid stencils second.
- **Host program.** Generated per model; owns buffer allocation, kernel launch sequence per step, bridge tensor upload/download, and result readback into Arrow.
- **Differential testing** is the acceptance mechanism for every kernel: the CPU tensor backend is the oracle.

---

## 11. Pilot model target (what the platform must express first)

A firm with business groups; growth decisions evaluated by simulation. Synthetic data until real data arrives (`tools/synth-pilot-data`). Start with two practices that differ in fee mix and demand slack. The Phase A implementation is a deterministic economics reference; M8 reproduces it in the general hybrid runtime. Neither claims that an intervention's causal effect has been identified from observational data.

- **Common accounting grain.** Use month for the economics clock and declare each input's unit. Won work enters two separate backlog stocks: T&M contracted baseline work-hours and fixed-fee contracted baseline work-hours. Fixed-fee contract value is recorded at booking and recognized as baseline work completes; T&M revenue is actual billed delivery-hours × realized hourly rate. A work-hour is the labor required before an intervention; productivity changes actual hours required. Do not treat a dollar stock as a dollar-per-month flow.
- **Demand and capacity.** Prospective work and stage conversion determine won work by practice and fee type. Capacity is paid FTE × available paid hours × deliverable share. Delivery draws from the appropriate backlog subject to capacity, staffing mix, and priority rules. Unused capacity remains slack; it is not automatically revenue. Hiring has an explicit lag and attrition changes the sole authoritative workforce count.
- **DES — sales and engagement lifecycle (M8).** `entity_type Opportunity {group, fee_type, baseline_hours, contract_value, stage, age}`; stage queues and conversions lead to won/lost sinks. Won opportunities create fee-specific backlog. Staffing and delivery events consume labor; invoicing and collection can follow later without being confused with revenue recognition.
- **ABM — workforce (M8).** `population Consultant {group, level, available_hours, utilization, tenure, task_exposure, adoption}` in `sync` mode, weekly step. The population count is authoritative headcount; the SD economics reads its aggregate. Hiring/attrition spawn/despawn agents. There is no separate independently integrated SD headcount stock.
- **SD — economics (Phase A reference, then M8).** Fee-specific backlog conservation; cumulative recognized revenue and operating costs; utilization as actual delivered hours divided by paid available hours; operating contribution as recognized revenue minus payroll, delivery, license, development, automation, and integration costs; margin as contribution divided by revenue. Revenue and costs use the same time grain.
- **Decisions as explicit interventions.** Copilot, tool build, process automation, and acquisition each declare start, lag, adoption/effect, and costs. AI and automation reduce actual labor hours per baseline work-hour; fixed-fee contract value stays fixed, while T&M billed hours fall unless additional won work fills the freed capacity. Acquisition adds workforce and demand after integration lag. Scenarios allow combinations, with shared adoption and capacity rules so benefits cannot be double-counted.
- **Data bindings.** `population_init` for consultants from an HR/utilization table; `entity_replay` for historical opportunities (used for calibration and as a warm start); `parameter_table` for per-group conversion rates and delivery rates; `exogenous_series` for seasonality; `calibration_target` for weekly utilization and monthly revenue by group. All with `required: false` and synthetic fallbacks until real data exists.
- **Checks.** Fee-specific backlog conservation; headcount equals the agent population count in M8; `0 <= utilization <= 1`; backlog non-negative; no demand and no initial backlog ⇒ no revenue; no capacity ⇒ no delivered revenue; fixed-fee contract value is invariant to productivity; a no-slack T&M AI gain cannot increase revenue by itself.
- **Outputs.** Per practice and firm: annual recognized revenue, margin, utilization, headcount, backlog; scenario incremental cash flow, NPV, payback, and a demand/win-rate break-even threshold. Report assumptions and parameter uncertainty separately from Monte Carlo standard error and numerical error. A deterministic Phase A result carries no fabricated error bar.
- **Experiment.** Start with baseline plus each of the four interventions and selected combinations in Phase A. Add stochastic replications and a Sobol sweep only after the planted synthetic truth and validation targets are in place; record which parameter intervals came from data and which are assumptions.
- **Human validation artifacts.** `viz` structure diagrams for a business-group lead to confirm the model of their group; `report` fan charts per decision scenario.

The M8 version must exercise the bridges actually needed by this model and the full agent loop (data → IR → check → run → explain → report). Separate conformance models exercise unused bridge kinds and the `async` population mode. The pilot is the decision-model integration test, not a reason to force every engine feature into one business model.

---

## 12. Risks

| Risk | Mitigation |
|------|-----------|
| Cross-platform float differences (libm and compiler behavior) | Test decoded values against a declared numeric policy; consider a portable libm only if the observed differences affect decisions |
| Bit-identity between event and tensor backends proves impractical for reductions | ADR downgrading to ULP-bounded, with fixed tree order documented |
| IR too weak for real models (needs loops/code) | Add primitives, not code: every "I need a loop" is a missing phase/statechart/block; keep the expression language pure |
| Hybrid tick protocol subtly disagrees with Vensim/AnyLogic conventions | Document conventions in SEMANTICS.md; PySD corpus and Bass/SIR tests catch it |
| Arrow dependency weight on macOS/CI | Isolate behind interface; CSV fallback |
| Scope creep toward AnyLogic's UI and domain libraries | §1.2 non-goals; ADR required to add any |
| Device-resident batching defeated by fine-grained DES coupling in real models | Partitioner reports host-bound coupling; model authoring guidance to coarsen bridge dt |

---

## 13. Open questions for Arni (decide before M2; defaults in parentheses)

1. Normal/Poisson draw algorithms to freeze (default: inverse-CDF via a fixed erfinv for normal; PTRS for large-λ Poisson, inversion for small).
2. Portable libm strategy if the measured cross-platform numeric differences exceed the declared tolerance policy (default: use the platform libm and report differences; vendor only if needed).
3. Arrow C++ as a hard dependency or optional with CSV core (default: optional, on by default).
4. Whether `async` populations are needed in the pilot at all (default: no — everything `sync` at a weekly step; keep `async` for DES-carried agents only).
5. License for the platform (adevs is BSD; default: Apache-2.0 or BSD-3).
6. Platform name: resolved as **AnkuraFathom**; public CLI `fathom`.
7. Units strictness for the pilot (default: `strict: true` from day one — retrofitting units is painful).
8. Report rendering: SVG generated in C++ (no Python dependency for `report`) or a small Python renderer over pyarrow (default: Python in Phase 1; move to C++ only if the CLI must be dependency-free).
9. Whether AnkuraFathom should also expose an MCP server so agents call it as tools rather than shelling out (default: not in Phase 1; the CLI's JSON contract is designed so a thin MCP wrapper is trivial later).
