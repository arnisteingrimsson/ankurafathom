# AnkuraFathom

The [control center](application/control_center/README.md) is the local operator
workspace for project configuration, model inspection, saved runs, traces and
validation evidence. It opens each project's separate customer UI.

```sh
.venv-runtime/bin/python -m application.control_center.server
```

Open `http://127.0.0.1:8086`. Browsing the console does not start simulations.

Current focus: [independent benchmark workloads](tests/benchmarks/README.md) and
[worked examples with independently checked results](examples/worked_validation/README.md).
Reporting expansion is deferred while we validate model mechanics.

The [synthetic T&R AI decision case](examples/ai_decision/README.md) adds operational
Parquet exports, independently checked parameter recovery, and native experiments
for pipeline growth, fixed-fee mix and acquisition under AI hours reductions.
Its assumptions are synthetic; historical reconstruction is an accounting check,
not an out-of-sample business backtest.

The [150-person T&R monthly pilot](examples/tr_pilot/README.md) extends this with
seven billable levels and practice support, loaded costs, attrition/hiring,
level-specific AI, fee/cash timing, constrained pricing, BD feedback and imperfect
operational data. It runs through the native engine with independent monthly
checks. Its expected-FTE and commercial assumptions remain synthetic.

AnkuraFathom is a standalone hybrid simulation platform in development. A working
[synthetic Ankura preview](examples/ankura_pilot/README.md) now runs two practices,
five years and all sixteen intervention combinations through the general SD
runtime and CLI/Python interfaces. [Review the scenario results](artifacts/pilot-diagrams/results/report.md)
and [yearly figures](artifacts/pilot-diagrams/results/yearly.csv). The separate C++ economics
reference remains an independent comparison. Native [declared validation](docs/DECLARED_VALIDATION.md)
now verifies accounting, capacity and operating constraints across the pilot
scenarios, with matched validation/run identities. Inputs/effects are synthetic;
broader M7 validation/reporting and full M8 acceptance remain open.
[Verified SD explanations](docs/EXPLANATIONS.md) now trace saved pilot results
through integrator-consistent flow accounting and data dependencies, with eight
HTML/JSON explanation pages linked from the scenario report. Native
[SD structure diagrams](docs/VISUALIZATIONS.md) now add stock/flow and dependency
views in SVG, DOT and Mermaid, with verified input linkage to each pilot run.

The CPU correctness platform is under construction. It currently has tested DEVS, SD (including lookup and delay primitives), DES (including a resizable resource pool and a source→server→sink path with fixed or reproducible exponential schedules), ABM, hybrid, RNG, and experiment primitives, plus limited declarative SD, DES, bounded two-way SD/DES, homogeneous ABM–SD adoption, and scheduled agent-pool formats. C++ bridges can drive DES pool capacity from a committed ABM population and map aggregate grants to individual agent assignments. A reference agent-pool transaction stages workforce changes, ABM phases, queue decisions, and ownership together, and has a single-atomic DEVS wrapper. The declarative agent-pool subset exposes that transaction through an explicit schedule, named synchronous capacity rules, and typed observations. Optional engagement delivery queues work for staffing, holds its assigned agents for a fixed duration, and releases them automatically at completion. Event-based time integrals track occupied and available capacity, queues, service, and headcount; utilization uses total capacity-time across workforce changes. Opt-in checked DEVS steps roll back cloneable atomics and event state when a transition fails. The [status table](docs/STATUS.md) distinguishes these subsets from the remaining Phase 1 work.

DES stations now support [finite queues and deterministic non-preemptive priorities](docs/DES_QUEUES.md), plus [LIFO and resource arbitration](docs/DES_DISCIPLINES.md), including terminal rejection counts in C++ and declarative DES/SD runs. Run `./build/fathom run models/bounded_priority_process.ir.json` for the checked example. Standalone DES also supports [acyclic process graphs](docs/DES_ROUTING.md) with binary priority/probability routing, merging, backup service, and explicit discard paths; run `./build/fathom run models/routed_process.ir.json`. The [statistical validation suite](docs/DES_STATISTICAL_VALIDATION.md) covers M/G/1 and finite queues using frozen analytical targets and a separate replication-planning pilot. [Independent station service](docs/DES_SERVICE.md) now supports per-station exponential draws in DES and linear hybrid models; the [Jackson suite](docs/DES_JACKSON_VALIDATION.md) checks two three-station networks against marginal and joint analytical targets. Run `./build/fathom run models/independent_service_process.ir.json` for the declarative example. [Pinned SimPy/Ciw comparisons](docs/DES_ENGINE_ORACLES.md) now check all twelve statistical workloads, including finite and joint occupancy distributions, against independently generated reference samples. [Probabilistic routing](docs/DES_PROBABILITY.md) adds two branching-network cases, 120 planned replications, and exact addressed replay; run `./build/fathom run models/probability_process.ir.json`.

[Typed DES graphs](docs/DES_TYPED_IR.md) now expose named entity schemas, separate queues/delays, shared resources with leases, condition/categorical routing, generated arrivals, independent exponential service, and scheduled capacities. They validate units and ownership paths and execute checked DEVS steps. Run `./build/fathom run models/typed_resource_process.ir.json` for the priority-resource example.

[Typed ABM models](docs/ABM_TYPED_IR.md) now run synchronous field-update phases or asynchronous flat statecharts through the DEVS kernel. They support typed records, unit-checked guards/actions, messages, timeouts, reproducible rate transitions and numeric observations. Run `./build/fathom run models/typed_abm_async.ir.json` for a workflow where completion and restart coincide, or `./build/fathom run models/typed_abm_rates.ir.json` for independent agent event streams. [Neighborhood queries and validated movement](docs/ABM_INTERACTIONS.md) now support grid, continuous and network interactions; run `./build/fathom run models/typed_abm_neighbors.ir.json`. [Topic delivery](docs/ABM_TOPICS.md) adds transactional requests, replies and broadcasts; run `./build/fathom run models/typed_abm_topics.ir.json`. [Phase/statechart publications](docs/ABM_GENERATED_PUBLICATIONS.md) now use transactional outboxes; [scheduled agent births and retirements](docs/ABM_LIFECYCLE.md) now preserve stable IDs, timers and live membership. Run `./build/fathom run models/typed_abm_lifecycle.ir.json`. [Phase/transition lifecycle actions](docs/ABM_BEHAVIOR_LIFECYCLE.md) now support conditional reproduction and replacement, with bounded allocation and newborn chart initialization; run `./build/fathom run models/typed_abm_behavior.ir.json`. [Mutable networks](docs/ABM_MUTABLE_NETWORKS.md) now support scheduled edge edits and automatic lifecycle cleanup; run `./build/fathom run models/typed_abm_network_updates.ir.json`. [Declarative ER/WS/BA graph generators](docs/ABM_GRAPH_GENERATORS.md) now support reproducible initialization and validated scenario parameters, with 51 predeclared statistical/analytical gates against pinned NetworkX; run `./build/fathom run models/typed_abm_network_generator.ir.json`. Behavior-generated graph edits and topic-handler lifecycle remain extensions; canonical-model and sync/async convergence evidence is recorded below.

[Boltzmann wealth exchange](docs/ABM_WEALTH.md) is the first canonical ABM model validated against pinned Mesa: 32 predeclared distribution gates across 2,048 trajectories per engine, with exact integer conservation and transactional sequential sweeps. Run `./build/fathom_wealth` for a standalone native example. [Schelling segregation](docs/ABM_SCHELLING.md) adds sequential relocation on validated grids, another 32 Mesa distribution gates and exact population/group/occupancy checks; run `./build/fathom_schelling`. [Boids/flocking](docs/ABM_BOIDS.md) adds synchronous steering, periodic/reflecting boundaries and another 32 Mesa distribution gates, with 2,880 paired trajectory values; run `./build/fathom_boids`. [Sugarscape-lite](docs/ABM_SUGARSCAPE.md) adds sequential harvesting, starvation and regrowth with exact resource accounting and 32 further Mesa distribution gates; run `./build/fathom_sugarscape`. [Network SIR](docs/ABM_SIR.md) adds synchronous infection/recovery on fixed contact graphs, 32 Mesa distribution gates and exact state/population checks; run `./build/fathom_sir`. [Continuous-time SIR and sync/async convergence](docs/ABM_SIR_ASYNC.md) add 32 asynchronous Mesa gates and full-joint-state refinement evidence; run `./build/fathom_sir_async`. [M4 acceptance](docs/M4_ACCEPTANCE.md) records the completed model/convergence gates and declared CPU scope.

Staffed delivery is also available as the C++ `AgentPoolProcessAtomic` component. It combines due completions with same-step workforce inputs and publishes committed results to coupled components; tests route completion counts into an SD stock. Exact deadline handling preserves simultaneous events, and checked retries cover both transaction failures and downstream publication failures. See the [delivery component contract](docs/SEMANTICS.md#agent-pool-delivery-as-a-devs-component) for message ordering and coupling limits.

The declarative `agent_pool_sd` mode connects staffed delivery to SD through one completion-to-stock bridge. It supports mixed staffing/stock observations and SD parameter overrides. The [example model](models/agent_pool_sd.ir.json) reproduces a hand-calculated completion, revenue, cost, and utilization trajectory; see [its semantics](docs/SEMANTICS.md#declarative-staffed-delivery-to-sd) for exact event timing and the current scope.

The [kernel conformance report](docs/KERNEL_CONFORMANCE.md) records 153 differential cases against pinned adevs, including all four DEVStone families, larger nested graphs, both confluent policies, and dynamic replacement/rewiring. Compiled hierarchies now support same-step root passthrough and edits between steps, including subtree replacement and future-input rerouting while preserving surviving model clocks. M1 is complete for the declared CPU kernel contract. [DEVStone timing and allocation records](bench/README.md) are informational; broader Phase 1 library work remains open.

The [SD validation report](docs/SD_VALIDATION.md) covers six independent reference models and 72 Euler/midpoint/RK4 trajectories, including convergence orders, conservation, and stage-failure isolation. The stored references keep routine C++ testing offline; CI separately verifies regeneration with pinned SciPy. Standalone SD now supports STEP/PULSE/RAMP with tested boundaries and explicit smoothing state. The [XMILE importer](docs/XMILE_IMPORT.md) maps lookup tables and SMTH1/SMTH3/DELAY1/DELAY3 with constant or variable durations into explicit components, including signed flows. It also maps fixed whole-tick DELAY and general-order SMTHN/DELAYN (orders 1–255). DELAYN defaults to constant duration; explicit `--delayn-policy cascade` supports variable durations with 1,386 independent expanded-stock PySD observations. Explicit `--delayn-policy history2` adds a conservative second-order history kernel, with 101 historical Vensim observations and 1,386 PySD comparisons; other history orders remain rejected after reference conservation failures. See [history evidence](docs/DELAY_HISTORY.md). Grid-aligned source STEP/RAMP/PULSE map to separate native functions, preserving XMILE pulse quantity semantics. Explicit `--input-policy next_tick` adds off-grid schedules and multiple pulses per tick, validated against 19,584 exact-rational observations; see [timing rules](docs/OFFGRID_INPUTS.md). Six upstream file variants plus four custom PySD fixtures provide 43,598 matching stock-value comparisons, including reordered variants. The complete fixed-delay source check and stock-free auxiliary fixture add 158 auxiliary and 26 flow comparisons. Native standalone SD preserves absolute start times; the importer offers `--outputs all` and safely expands sign-filtered flow references without source-stock limits. The [corpus audit](docs/XMILE_COVERAGE.md) records 8 imports and 59 rejections: 100% structural coverage of eight eligible files, with 87.5% having compatible complete historical references; 49 malformed files have unknown eligibility. Nested fixed delays are checked against exact hand calculations because PySD updates their state sequentially. Bounded Euler clipping now preserves shared-flow amounts and explicit outflow priority; see the [execution contract](docs/SEMANTICS.md#bounded-euler-clipping). The assessable-corpus gate is met. Another 56 analytic convergence trajectories now validate exponential growth/decay and delay cascades; see the [M2 acceptance review](docs/M2_ACCEPTANCE.md). M2 remains open for its remaining source-function contracts.

Build and run on macOS or Linux with a C++20 compiler, CMake, and Python 3.10+ for the importer and test contracts:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/fathom --input models/synthetic_practices.csv --output build/yearly.csv --summary build/summary.csv
./build/fathom lint models/decay.ir.json
./build/fathom run models/decay.ir.json --experiment models/decay.experiment.json --out build/decay_runs.csv
./build/fathom run models/lookup.ir.json
./build/fathom run models/agent_pool.ir.json
./build/fathom run models/agent_pool_phases.ir.json
./build/fathom run models/agent_pool_delivery.ir.json
./build/fathom run models/agent_pool_sd.ir.json
./build/fathom run models/agent_pool_utilization.ir.json
./build/fathom run models/delay.ir.json
./build/fathom run models/process.ir.json
./build/fathom run models/tandem_process.ir.json
./build/fathom run models/process.ir.json --experiment models/process.experiment.json
./build/fathom run models/stochastic_process.ir.json --experiment models/stochastic_process.experiment.json
```

The CLI evaluates all 16 combinations of Copilot, tool build, process automation, and acquisition over five years. `yearly.csv` contains practice and firm totals. `summary.csv` contains incremental NPV and payback versus the baseline. Read [docs/SEMANTICS.md](docs/SEMANTICS.md) for the monthly accounting order and limits of this first model.

The supplied synthetic case has a T&M-heavy Disputes practice with demand slack and a fixed-fee-heavy Advisory practice with a growing backlog. This deliberately tests both sides of the AI productivity question. Scenario effects and costs are illustrative constants applied to both practices; they are not calibrated Ankura assumptions. The NPV is a discounted operating-contribution proxy, excluding tax, financing, and working-capital effects.

The [implementation plan](IMPLEMENTATION_PLAN.md) defines the full model IR, event kernel, validation harness, and later accelerator phases. The generic CLI currently supports a small stock-and-flow IR with lookup tables, orders 1–255 and fixed whole-tick delays, and scenario parameter overrides; an acyclic multi-stage DES graph with fixed or addressed-exponential arrivals, binary priority routers, and rejection paths; and a hybrid subset in which each DES completion adds a typed pulse to an SD stock. An optional bounded bridge turns a committed stock level at a grid tick into a new DES arrival or changes a Poisson arrival rate while preserving the pending event's hazard. A separate `abm_sd` subset runs seeded binary adoption, publishes the adopted count to an SD stock, and lets other SD flows read that stock. Run `./build/fathom run models/abm_adoption.ir.json` for the ABM–SD example. It cannot yet express the consulting economics reference model. Stochastic fixtures change across experiment replications and replay exactly from the same seed and draw addresses.

[Well-mixed SIR → SD convergence](docs/HYBRID_SIR_MEAN_FIELD.md) validates the native individual ABM against an independent count CTMC and SciPy: four cases, three population sizes, 3,072 trajectories per stochastic engine, stored plots and numeric gaps. Run `ctest --test-dir build -R '^hybrid_sir' --output-on-failure`. [Bass adoption → SD convergence](docs/HYBRID_BASS_MEAN_FIELD.md) adds six population curves, 4,608 runs per stochastic engine, independent binomial and closed-form checks, and separate time-step refinement. Run `./build/fathom_bass` for the native example. [DES → SD fluid convergence](docs/HYBRID_DES_FLUID_LIMIT.md) adds four queue regimes, exact arrival/completion pulse accounting, 3,072 runs per stochastic engine and 180 independent distribution comparisons. Run `./build/fathom_queue_backlog` for the composed native example. [Typed agent continuous stocks](docs/HYBRID_AGENT_STOCKS.md) add simultaneous Euler integration, conservative agent/global transfers and aggregate-driven SD flows, validated against independent closed forms and equivalent pure SD. Run `./build/fathom_agent_stocks` for the native example. [Typed snapshot aggregates](docs/HYBRID_TYPED_AGGREGATE.md) publish five reducers into SD flow callbacks with checked DEVS rollback and exact-rational timing tests. Run `./build/fathom_typed_aggregate` for the native example. [Agent-stock DEVS composition](docs/HYBRID_AGENT_STOCKS_ATOMIC.md) now publishes continuously integrated population snapshots into the aggregate/SD path; run `./build/fathom_agent_stocks_coupled`. [Declarative continuous agents and SD](docs/AGENT_STOCK_SD_IR.md) expose unit-checked agent stocks and filtered aggregate flows; run `./build/fathom run models/agent_stock_sd.ir.json --out /tmp/agent-stock-sd.csv`. [Typed event pulses](docs/HYBRID_TYPED_PULSES.md) post typed DES payload quantities to SD; run `./build/fathom_typed_event_pulse`. [Typed lifecycle events](docs/HYBRID_LIFECYCLE_BRIDGE.md) connect DES completions to population-owned births/retirements; run `./build/fathom_typed_lifecycle`. [Shared entity-agent ownership](docs/HYBRID_ENTITY_AGENT.md) gives statecharts and DES one authoritative record; run `./build/fathom_entity_agent`. [SD-driven typed arrivals and routing](docs/HYBRID_SIGNAL_RATES.md) connect scalar projections to addressed sources and categorical routing; run `./build/fathom_signal_rate`. [Typed workforce pools](docs/HYBRID_TYPED_AGENT_POOL.md) join staffing, transactional broker writeback and population aggregates; run `./build/fathom_typed_agent_pool`. [Dynamic agent stocks](docs/HYBRID_DYNAMIC_AGENT_STOCKS.md) add timestamped lifecycle/pulse changes and inventory receipts; run `./build/fathom_dynamic_agent_stocks`. [M5 tracking](docs/M5_ACCEPTANCE.md) records the remaining bridge contracts.

The [pinned upstream adevs snapshot](third_party/adevs/UPSTREAM.md) builds as `adevs_reference` and has an upstream schedule test in CTest. It is an oracle for the separate DEVS kernel; it is a local upstream snapshot, not a hosted fork. The economics executable does not link to it.

Standalone models can select `"integrator": "rk4"`; Euler remains the default.
Run `./build/fathom run models/rk4_growth.ir.json` for time-dependent growth.
The XMILE RK4 mapping has independent SciPy convergence and closed-form checks.
The newly imported upstream RK4 model has a documented Euler-reference conflict;
it is not counted as a full historical trajectory pass.

[Ordered experiment execution](docs/RUNTIME_EXPERIMENTS.md) adds `--threads` and
`--seed`, deterministic ensemble merging, progress/cancellation and atomic CSV
publication. A 1,000-scenario CLI experiment has identical results across thread
counts; runtime tests also pass under ThreadSanitizer.

[Arrow/Parquet results](docs/RUNTIME_OUTPUTS.md) now add typed observation tables
and atomic binary file output, with exact decoded values across 1/8/64 threads.
Build with the optional Arrow SDK and run `fathom run model.json --out results.parquet`
or `--out results.arrow`. The CSV-only build remains available.

[Validated data snapshots](docs/RUNTIME_DATA.md) now load local CSV, Parquet and
Arrow IPC through the native data API. Schemas, unique keys and finite values are
checked before rows are sorted and both file/content hashes are recorded. Run
`./build-arrow/fathom_data_table models/data/synthetic_parameters.csv` for the
synthetic example. [Native population initialization](docs/POPULATION_DATA_BINDING.md)
now maps file rows into typed agents with transactional rollback and identity
receipts; run `./build-arrow/fathom_population_from_data models/data/synthetic_parameters.csv`.
[Native series/parameter adapters](docs/DATA_INPUT_BINDINGS.md) now sample external
inputs and select parameter rows by typed keys; run `./build-arrow/fathom_series_from_data models/data/synthetic_parameters.csv models/data/synthetic_seasonality.csv`
for a synthetic SD example. [Declarative parameter tables](docs/PARAMETER_DATA_IR.md)
now bind local data to standalone SD model parameters with unit checks, source
receipts and explicit scenario override precedence. Run
`./build-arrow/fathom run models/data/parameter_decay.ir.json`.
[Declarative exogenous series](docs/SERIES_DATA_IR.md) now provide unit-checked
`series_id(t)` functions with hold/linear interpolation at actual Euler/RK4
evaluation times. Run `./build-arrow/fathom run models/data/seasonal_stock.ir.json`.
[Declarative population initialization](docs/POPULATION_DATA_IR.md) now loads
sync/async typed ABM agents from validated tables, preserving source-key receipts
and the existing lifecycle/statechart/spatial checks. Run
`./build-arrow/fathom run models/data/workforce.ir.json`.
[Run manifests and strict replay](docs/RUN_MANIFESTS.md) now record captured input
hashes, expanded scenarios, build policy and ordered numeric identity. Use
`--out results.csv --manifest run.json`, then `fathom replay run.json --threads 1`.
This first path requires the recorded local inputs to remain available.
Manifest-backed CSV/Parquet/Arrow results now include the manifest ID and effective
scenario parameters per observation. Parquet and Arrow also embed the full manifest;
replay preserves the original identity. Runs without manifests retain their schemas.
Use `fathom verify-results run.json --results results.parquet` to check a saved
schema 0.2 artifact's numeric identity and lineage without rerunning the model.
Arrow and Parquet also support `fathom verify-results results.parquet --embedded`,
using the manifest inside the result when no sidecar is available.

The [C ABI foundation](docs/C_API.md) now builds `libankurafathom`: validated JSON/file
loading, single/ensemble execution, owned results, exact observation access and
thread-local diagnostics. Build `fathom_c_api`; the public header is
`runtime/include/ankurafathom/c_api.h`. Arrow-enabled builds export independently
owned C Streams with explicit release rules. The optional [Python package](docs/PYTHON_API.md)
now provides `Model.from_json`, `Experiment`, `run() -> pyarrow.Table`, and `lint()`
through nanobind over this C interface. Enable `FATHOM_ENABLE_PYTHON` to build it.
[Local packaging](docs/PACKAGING.md) provides Python wheels/source archives and
a native CMake-installed C SDK/CLI; no publishing step is required.
[API provenance](docs/API_PROVENANCE.md) adds a C manifest getter and Python
`run(..., provenance=True)` with embedded run receipts and verifiable saved results.

[Scenario designs](docs/RUNTIME_SCENARIO_DESIGNS.md) add canonical grids, addressed
Latin hypercubes and bounded Sobol designs, with frozen independent references.
Examples: `models/decay.{grid,lhs,sobol}.experiment.json`.

[C/Python execution callbacks](docs/API_EXECUTION_CALLBACKS.md) provide ordered trajectory progress and cooperative cancellation; Python accepts `run(..., progress=callback)`.

CLI file runs now create `<output>.manifest.json` by default, with schema-0.2 lineage in results. Use `--manifest PATH` to select another sidecar or `--no-manifest` for legacy output; stdout remains unchanged. See [run manifests](docs/RUN_MANIFESTS.md).

[Portable replay bundles](docs/PORTABLE_REPLAY.md) capture a file-backed run with `fathom bundle run.json --out bundle-directory`. `fathom replay bundle-directory` works after relocating the bundle and removing the original inputs, while retaining strict build-policy and numeric checks.
