# AnkuraFathom reference semantics

This document specifies the Phase A economics reference and the implemented CPU simulation subsets. Later semantics remain tracked in `docs/STATUS.md`.

## Expression resource limits

The reference interpreter accepts at most 65,536 source bytes per expression,
256 active recursive parser levels, and 256 AST levels. Parentheses and unary
plus consume parser levels; flat arithmetic chains consume AST levels. Oversized
expressions reject with `invalid_argument` in C++ and the existing `IR_EXPR`
diagnostic/pointer through the loader. Split larger expressions into auxiliaries.
Accepted expressions preserve their existing evaluation order. These limits bound
parsing, evaluation, dimensional inference and tree destruction; see ADR-A08.

`MIN(a,b)` and `MAX(a,b)` require equal argument dimensions. The pure
`IF_POSITIVE(c,a,b)` requires a dimensionless condition and equal branch dimensions;
it evaluates only `a` when `c > 0`, otherwise only `b`. Nonfinite conditions reject.
Both branches are checked for symbols and units even if one is never evaluated.
These operations preserve exact zero-demand behavior without epsilon denominators.

Standalone SD supports `{id, kind: "aux", expr, unit}`. Auxiliaries form a checked
DAG and evaluate in deterministic topological order, with identifier ordering for
ties, before flows and output expressions. Each derivative invocation recomputes
them from that stage's state/time, including RK4 stages. They may read parameters,
stocks, time, other auxiliaries and tables/series. Delay-output dependencies reject;
delay inputs/durations may read auxiliaries independent of delay outputs. Tick
input functions in auxiliaries require Euler. Cycles, bad units and unresolved
symbols fail at load time; execution failures retain the auxiliary's JSON pointer.
Hybrid-mode auxiliary declarations remain outside this standalone SD increment.

## Units and state

The clock is one month. Each practice has one authoritative paid workforce count, `fte` (people). Its two backlog stocks are T&M and fixed-fee work measured in **baseline labor hours**: hours that a contract would require without an intervention. Revenue and costs are USD. A flow of baseline hours delivered in one month is baseline hours/month; billed hours and actual labor hours are hours/month. Available delivery capacity is `fte × paid_hours_per_fte_month × delivery_share` actual labor hours/month. Utilization is actual labor hours divided by `fte × paid_hours_per_fte_month` and cannot exceed `delivery_share`.

## Monthly order

For months 1 through 60, in each practice:

1. Apply acquisition headcount at month 13 if selected. The acquired team adds both FTE and potential work at that date. No other hiring or attrition is modeled yet.
2. Calculate prospective baseline work-hours from the practice's monthly pipeline and its annual demand growth. Multiply by the win rate and split bookings into T&M and fixed fee using `fixed_fee_share`. Add won baseline hours to the corresponding backlog.
3. Calculate the actual labor-hours needed per baseline hour. Intervention effects arrive after their start/lag, ramp as documented in code, and multiply sequentially. The multiplier is bounded below by 0.5. This is an explicit overlap assumption, not an observed effect.
4. Divide delivery capacity between fee types in proportion to their outstanding *actual labor-hours required*. Deliver up to that capacity and reduce each backlog by the delivered baseline hours. This proportional policy is deterministic and conserves each backlog.
5. Recognize T&M revenue as actual delivered/billed hours × realized hourly rate. Recognize fixed-fee revenue as delivered baseline hours × contracted value per baseline hour. Fixed-fee value per baseline hour is set equal to the input rate for the synthetic case; productivity never changes that contract price. Invoicing and cash collection are outside Phase A.
6. Expense monthly payroll for all paid FTE, non-payroll variable delivery cost for actual delivered hours, and applicable intervention costs. Operating contribution equals recognized revenue minus these costs. Annual outputs sum the monthly flows. The annual headcount column is end-of-year FTE; utilization is the ratio of annual actual delivery hours to annual paid hours; backlog is end-of-year baseline hours.

Backlog identity for each fee type and month: `ending = starting + won - delivered`, within floating-point tolerance. There is no independent SD headcount stock or implied conversion of saved hours into revenue. A no-demand T&M practice can bill fewer hours after productivity improves; a fully subscribed T&M practice may use freed capacity on more engagements and preserve billed hours. Fixed-fee productivity can improve contribution by reducing variable delivery cost; paid payroll only falls if a future model explicitly changes staffing policy.

## Scenario economics

The baseline and all 15 nonempty combinations use the same practice inputs. Copilot begins after month 1 and ramps adoption over six months; tool build begins after month 9 and ramps over six months; process automation begins after month 6 and ramps over six months; acquisition takes effect in month 13. Assumed effectiveness and costs are constants in the reference implementation and are not estimates from Ankura data. Incremental NPV discounts each scenario's monthly contribution difference from baseline at 10% annually. Payback is the first month when cumulative undiscounted incremental contribution becomes nonnegative *after* a negative cumulative balance has occurred and remains nonnegative for the rest of the horizon; blank means no sustained payback within 60 months. A scenario with no initial investment can have an empty payback value even when its NPV is positive.

Phase A has no randomness, Monte Carlo standard error, integration error estimate, causal attribution, hiring lag beyond the acquisition date, engagement-level queue, or financing/cash collection model. These are later milestones and will be labeled separately when implemented.

## CPU DEVS kernel semantics (initial implementation)

The initial generic kernel implements Parallel DEVS atomic models at one host clock. An atomic supplies `time_advance`, `output`, `internal_transition`, `external_transition(elapsed, input_bag)`, and `confluent_transition(input_bag)`. The confluent transition is explicit; the kernel does not silently choose an internal/external order. Atomics are connected by directed typed ports; nested coupling is flattened before execution, and both flat and compiled nested structure can change between steps.

Time begins at zero, uses finite nonnegative `double` values, and never moves backward. An atomic's next internal transition occurs at its last transition time plus its nonnegative time advance; infinity means passive. At each minimum timestamp, **all** imminent atomics produce outputs from their pre-transition state. The kernel routes every output through declared couplings, assembling one input bag per recipient. It then transitions each affected atomic exactly once: internal when imminent with an empty bag, external when only a bag arrives, confluent when both occur. Output order and bag order follow source component ID, output position, then destination coupling order. Every affected model receives the same timestamp; ties do not cause one model to observe another model's post-transition state. A zero-time advance schedules a new microstep at the same physical time. A run has a transition-count limit to diagnose zero-time cycles.

Calendar-owning atomics can override `next_event_time()` with an absolute deadline. That deadline is authoritative instead of recomputing `last_transition + time_advance`; a supplied positive infinity passivates the atomic. Finite deadlines must be no earlier than the current clock and last transition, and the ordinary time advance must still be nonnegative and valid. The default returns no override, preserving relative scheduling. The kernel also calls `external_transition_at(time, elapsed, bag)`: its default delegates to `external_transition(elapsed, bag)`, while timestamp-aware atomics may override it to preserve and validate the exact event time. Both hooks avoid assuming that adding a rounded elapsed duration reconstructs the original absolute timestamp. No tolerance merges distinct event times.

The initial kernel's guarantees are exact trace reproducibility within one build and model order, finite-time validation, and explicit simultaneous-input handling. Component IDs are stable insertion-order IDs. Components and couplings may be added or removed **between** completed event steps; removed IDs are never reused, and their scheduled events and couplings are invalidated. Structural changes from inside a transition are rejected. External inputs can be injected at a finite time no earlier than the current clock into an active component. They arrive before coupled outputs in the same input bag, ordered by injection call; their `Input.source` is the `external_source` sentinel. A same-time internal event and injection cause a confluent transition. Removed components discard pending injections. Continuous integration inside the kernel and multi-thread execution are subsequent milestones. These are tracked in `docs/STATUS.md` rather than implied by the presence of the kernel.

`step_transactional` and `run_until_transactional` are opt-in checked execution. Before each event step, the simulator clones every active atomic and copies the event and injection queues, clock, and scheduling metadata. If any output or transition throws, it restores that pre-step state and rethrows, so the same event can be retried after the underlying cause is corrected. All active atomics must implement `clone`; an unsupported atomic causes rejection before the step begins. The built-in DES and hybrid atomics implement this contract. A clone must own all mutable transition state; external side effects and changes to objects referenced outside the simulator are not covered. Rollback replaces atomic objects, so callers must reacquire a component through `model(id)` rather than retaining a raw pointer after failure. Completed earlier steps remain committed. The ordinary `step` and `run_until` APIs retain their existing behavior and do not promise rollback.

Nested coupling is flattened before execution. A coupled component's input and output ports are boundary nodes, not independently scheduled atomic models. Within one parent, a source is either the parent's input boundary or a direct child's output; a destination is either the parent's output boundary or a direct child's input. Following those directed paths from an atomic output reaches zero or more atomic inputs and root output ports. Following paths from a root input reaches atomic inputs and/or root output ports, including through an empty nested group. Each distinct path delivers one message. Atomic routing uses destination-ID and port order, preserving declaration order for equal destinations. Root-input expansion and boundary output paths retain traversal/declaration order. Pure boundary cycles, including cycles disconnected from current sources, are rejected at compilation and on edits. Cycles through atomics remain legal and are subject to the event-step limit.

### Root boundary events

`CompiledCoupling::inject(time, port, value)` queues one root input. At its timestamp, the simulator expands the input using the routes then in effect. Passthrough produces boundary output in that same step, without an artificial atomic, delay, or additional microstep. An empty hierarchy can therefore have a scheduled root event with no atomic transitions. Multiple root inputs preserve injection-call order and path multiplicity. Direct atomic injections and expanded root inputs precede atomic emissions in recipient bags; simultaneous internal events remain confluent.

`StepResult::boundary_injections` records original root inputs and `boundary_emissions` records resolved root outputs. Passthrough outputs come first in injection/path order, followed by routed atomic outputs in native emission/path order. `CompiledCoupling::boundary_outputs(step)` reads this stored result; later rewiring or removal cannot reinterpret an earlier result. `StepResult::injections` continues recording delivered atomic inputs, including those expanded from root inputs.

A new root input requires a connected root port and a finite timestamp no earlier than the current clock. Already queued root inputs survive edits: their destinations are resolved at delivery. If all routes have disappeared, the event is still consumed and recorded, with no delivery. Direct atomic injections remain bound to their destination ID and are discarded when that atomic is removed. Checked-step rollback restores queued root events together with atomic state; a subsequent successful edit before retry changes their routing just as it would for any future event.

### Compiled hierarchy edits

After `CoupledBuilder::compile`, use the returned `CompiledCoupling` object's `add_coupled(parent)`, `add_atomic(parent, model)`, `connect(parent, source, out, destination, in)`, `disconnect(...)`, and `remove(component)` between event steps. Parent and direct-child checks are identical to the builder's. Disconnect removes one declared edge; remove retires an atomic or an entire coupled subtree, including descendant atomics, boundary links, pending atomic outputs, and direct injections. Root removal is forbidden. Atomic and group IDs each remain stable and are never reused.

Each individual edit stages a topology copy, validates/expands all routes, then commits routing and membership together. Invalid endpoints, missing edges, boundary cycles, and invalid schedules of atomics added after initialization leave the running graph unchanged. Surviving atomics retain their exact state, last-transition time, and queued deadline. Newly inserted atomics start at the current simulator clock. Removed atomics are also skipped during initial scheduling if removal occurs before the first step. Edits never require cloning surviving atomics.

The public `simulator` remains the interface for stepping, checked execution, model observation, and direct atomic injection. Its flat structural mutation methods reject calls on a compiled hierarchy; this prevents a second graph definition from bypassing hierarchy validation. Standalone flat simulators can use `replace_routing(Routing)` to install a validated complete routing table. The earlier mutable `CompiledCoupling::root_inputs`/`root_outputs` tables have been replaced by this simulator-owned routing and recorded boundary results.

All compiled edits are rejected during output or transition callbacks. Each API call is a separate committed edit; there is no multi-operation edit transaction, subtree reparenting API, or model-requested structural transition. A replacement is expressed as removal and insertion between steps. See the [hierarchy differential fixtures](../tests/conformance/devs/hierarchy.cpp) for a running subtree replacement and future-input rewiring example.

`disconnect(source, source_port, destination, destination_port)` removes one declared flat coupling path between active endpoints. Duplicate declarations continue delivering once per remaining path. Missing edges or inactive endpoints throw `std::out_of_range` without changing routing. Like other structural operations, disconnect is rejected during output/transition execution and may be used between steps to rewire a graph.

The [kernel conformance suite](KERNEL_CONFORMANCE.md) compares canonical per-step traces with pinned adevs. It preserves timestamps, microstep boundaries, transition kinds, input-bag multiplicity, and outputs, while sorting simultaneous component records and unordered port/value bags. That normalization applies to the test fixtures, not to native runtime ordering; a separate test checks raw native ordering with distinct payloads.

## SD stock-and-flow semantics (initial implementation)

An SD model contains a fixed vector of stocks and directed flows. A flow evaluates a finite rate (nonnegative by default, signed by explicit opt-in) in stock-units per unit time from the **same pre-step state** and current time. It subtracts that rate from its source stock, adds it to its destination stock, or crosses the model boundary when one endpoint is absent. The derivative of every stock is the sum of all inflows minus outflows. A flow may connect two stocks with different declared business units only after a later units checker proves the conversion; the initial C++ API leaves unit declarations to the caller and therefore must not be treated as a fully validated IR model.

Euler is the reference: `S(t+dt) = S(t) + dt × f(S(t), t)`. Midpoint RK2 evaluates once at `(S(t), t)` and once at `(S(t)+dt/2 × k1, t+dt/2)`. Classical RK4 uses the standard weighted four derivatives. All stocks commit together after the step. Nonnegative stocks reject a materially negative result; the library never silently clamps it. Negative or nonfinite flow rates, time steps, or committed states are errors. The analytic tests distinguish exact Euler discretization from convergence to the continuous solution.

A lookup table is an ordered set of at least two finite `(x, y)` knots with strictly increasing `x`. Between knots, it uses piecewise-linear interpolation; a query exactly at a knot returns that knot's value. Beyond the range, `clamp` returns the nearest endpoint value and `linear` extends the first or last segment. A nonfinite query or result is an error. Lookup evaluation is pure and does not update an SD stock. The IR subset can declare a table with input and output units and call it from a flow expression with one argument; the argument's inferred unit must match the table input unit.

An Euler delay has 1–255 equal-duration stages; order is a fixed integer. The material variant stores quantities; stage duration is `delay_time / order`, each stage's output rate is its quantity divided by that duration, and all stage transfers are evaluated from the same pre-step state. The first stage receives the external input rate, and the final stage's output leaves the pipeline. Each stage initially contains `initial_output_rate × stage_duration`. Thus `pipeline_after = pipeline_before + dt × (input_rate − output_rate)` for every step, even when delay time changes. The information variant stores values, initializes all stages to the initial output value, and updates each stage toward its preceding stage (the first toward input) at rate `(target − stage) / stage_duration`; its output is the last stage's value. It does not assert material conservation. The step requires finite positive delay time, finite input, and `0 < dt ≤ stage_duration`; material input and initial output must be nonnegative. A smaller delay than the time step is an error rather than an implicit change in order. These are explicit Euler semantics following the constant-delay equations in [Vensim DELAY1](https://www.vensim.com/documentation/fn_delay1.html), [DELAY3](https://www.vensim.com/documentation/fn_delay3.html), [SMOOTH](https://www.vensim.com/documentation/20480.html), and [SMOOTH3](https://vensim.com/documentation/fn_smooth3.html). Variable-delay and other-integration-mode equivalence to Vensim is not yet claimed.

The [SD validation suite](SD_VALIDATION.md) checks Euler, midpoint, and RK4 against six independent reference trajectories with four timestep resolutions. It separates exact discrete Euler behavior from convergence to continuous solutions, checks conservation and bounded growth, and verifies that failures at any integration stage leave stock state uncommitted. Its error/order gates apply to the declared smooth, nonstiff fixtures; they do not provide adaptive error control or guarantees for arbitrary user models.

## DES FIFO single-server semantics (initial implementation)

Scheduled sources and service stations expose absolute event deadlines to the kernel. Single-/multi-server stations, routers/discard sinks, and completion sinks preserve exact timestamps through the timestamp-aware external callback. Service deadlines remain fixed across intervening arrivals; positive durations that cannot advance the absolute clock are errors. See the [fractional-clock regression](DES_STATISTICAL_VALIDATION.md#clock-regression-found-by-the-workload).

An entity has a unique ID, arrival time, and positive service duration. A station has one active entity and an unbounded FIFO waiting queue. Arrivals at time `t` join the queue in input-bag order; if idle, the first starts immediately. The station outputs the active entity just before its completion transition. If completion and arrival share a timestamp, the confluent transition completes the active entity, queues the new arrivals, then starts the oldest waiting entity. Thus an arrival at the exact completion time cannot overtake someone already waiting.

Queue length is the number **waiting**, excluding the active entity. Busy state is zero or one. Their integrals advance by old state × elapsed time at each transition; `mean_queue_length(horizon)` and `utilization(horizon)` include the unobserved interval from the last transition through the requested horizon. Waiting time is service-start time minus arrival time. No entity is dropped silently. This original single-server reference has no capacity limits or priority support; use the multi-server station with one slot for those features. Preemption remains unsupported.

The multi-server station has a fixed positive integer number of identical server slots and defaults to an unbounded FIFO waiting queue. New work fills idle slots in ascending slot order; queued work starts in arrival-bag order. Each service duration is the entity's base duration multiplied by the station's positive `service_scale` (default 1), fixed on admission. Its next event is the minimum remaining service time. If multiple slots finish at that timestamp, it emits all completions in slot order before state changes, then frees every due slot and fills them from the existing queue. In a confluent transition, those completions happen before same-time arrivals join the queue, so new arrivals cannot pass waiting work. A downstream station can accept a completed entity from an upstream station at that completion time. It preserves the original source arrival for end-to-end cycle time, clears the prior completion timestamp, and records its own entry time for local waiting statistics. Queue area integrates waiting count, and busy area integrates busy slot count; utilization divides busy area by capacity times horizon. Entity IDs are unique over a station run, including the single-server station. An invalid arrival bag leaves the station state unchanged even if time would have advanced or completions would have occurred. Optional finite waiting capacity, LIFO, and non-preemptive priority extend these defaults under the [queue contract](DES_QUEUES.md); preemption and dynamic service-slot capacity remain unsupported.

A resource pool is a DEVS atomic with nonnegative integer capacity and a positive maximum request size. The one-argument constructor sets both to the same positive value; the two-argument constructor permits zero initial capacity or a request limit larger than initial capacity. A seize request has a unique request ID and positive integer units no greater than the configured request limit; a release names an active allocation. The pool maintains `available + allocated_units = capacity` and defaults to a strict FIFO waiting queue. At an external event timestamp, the entire input bag is validated before state changes. Releases are applied in bag order; an optional single capacity update is then applied; existing waiters receive capacity first; finally new seize requests join in bag order and are granted from the head while they fit. A capacity update may set zero, but cannot reduce capacity below the units still allocated after same-bag releases. Requests can queue at zero capacity or while larger than current capacity and wait for a later expansion. A request behind a too-large head waits even if it would fit. Grant messages are emitted in a zero-time microstep after admission. At a confluent timestamp, previously pending grants are emitted from pre-transition state, then cleared before applying the new bag; newly earned grants emit in a later microstep at the same timestamp. Request IDs are never reused. The optional priority/LIFO extensions are specified below. Preemption is explicitly rejected, and the pool does not yet expose time-weighted wait statistics.

A scheduled DES source takes a finite list of entities ordered by nondecreasing arrival time. At a timestamp it emits all entries with that timestamp as one output bag, preserving list order; its internal transition advances past that batch. IDs must be unique and service durations positive. A completion sink is passive and validates that each delivered entity has a finite completion timestamp matching the current DEVS time. It records completed entities in input-bag order and sums `completed_at − arrived_at` as total cycle time. Both source and sink reject unexpected input types or duplicate IDs. An optional C++ schedule generator draws independent exponential interarrival and service durations from two adjacent Philox streams, addressing each draw by scenario, replication, entity ID, step, stream, and draw index. It accumulates arrivals from a nonnegative start time, refuses address overflow and non-increasing floating-point arrival times, then feeds the same scheduled source. Standalone processes support finite queues and [acyclic branching/rejection routing](DES_ROUTING.md). Hybrid processes retain the linear path.

## Queue and resource arbitration

Process stations support `fifo`, `lifo`, and non-preemptive `priority`. LIFO admission retains input-bag order and the existing finite-capacity rule; accepted work is never evicted. Dispatch removes the most recently admitted waiting entity, including the last accepted entity of a simultaneous bag. Each idle slot dispatches in slot-index order. Active service never changes when newer work arrives. At confluence completions free capacity, admission commits, then dispatch selects among all accepted waiters. Thus LIFO deliberately reverses same-bag service order, while overflow selection still follows admission order. FIFO and priority retain their existing semantics.

The C++ resource pool supports the same three disciplines. A `Seize` carries request ID, units, optional signed integer priority (default zero), and `preempt` (default false). Any `preempt=true` request is rejected atomically; active allocations cannot be revoked. Releases precede a capacity update, then all new requests enter the queue before dispatch. FIFO removes the oldest waiter; LIFO removes the newest. Priority dispatch chooses the lowest integer priority, preserving earlier admission across transitions; new equal-priority requests in one bag are ordered by request ID. Priority admission canonicalizes a bag by priority then ID. Dispatch stops if the selected head needs more units than are available: smaller requests never bypass it, regardless of discipline. Capacity can fall to zero but cannot fall below allocated units. Zero-time grant publication, clone state, and transactional bag/confluence rollback retain their existing meanings. Agent-backed pools retain their FIFO default; general resource nodes and expressions are separate declarative work.

## Typed entity column store

`EntityStore<Tag, Fields...>` stores one column per declared C++ field and one liveness column. Supported field types are `double`, `int64_t`, `bool`, and `std::string`; real values must be finite. A record is the corresponding typed tuple. `EntityRef<Tag>` contains an explicit store ID and an entity ID, preventing accidental C++ interchange of distinct entity tags. Store IDs are supplied by the owner and must be unique within a model; clones retain the same ID. A reference from a different store is rejected even if its entity ID exists locally.

Entity IDs increase monotonically from the configured first ID, fit within 48 bits for addressed draws, and are never reused. Retirement marks an existing live row inactive and retains its field values for audit; ordinary record/field access rejects retired rows. Read-only columns include all allocated rows and align with the liveness column; row offset is entity ID minus first ID. Read-only column views may be invalidated by a successful mutation. Callers retain IDs, not vector addresses. `alive` returns false for unknown or retired IDs within the correct store namespace. Empty batches have no effect.

Spawn and update batches validate every field, reference, and duplicate before committing any row. A failed batch leaves columns, liveness, active count, and the next ID unchanged. Successful updates preserve identity and row order. Store copies own independent column data, so an atomic that owns a store by value can participate in the kernel's checked-step rollback. This initial library does not grant transactional safety to shared mutable stores held outside an atomic's copied state; process composition must establish ownership before using such references. Declarative field names/units, runtime schemas, and process-block bindings are separate increments.

### Runtime field schemas

`RuntimeEntityStore<Tag>` provides the same namespace, identity, liveness, and transactional batch contract when field types are known at model load time. Its immutable schema is an ordered list of unique, nonempty field names and exact kinds (`real`, `integer`, `boolean`, `string`); an empty schema is permitted for identity-only entities. Each field owns a homogeneous typed column. Record values are tagged variants and must match the schema exactly, without integer/real/bool coercion. Named or indexed reads validate the field and live reference. Real values must be finite. Units and expression binding belong to the declarative model, not this storage container. Runtime schemas do not weaken the compile-time tag distinction or store namespace check. Copies own all schema, column, and liveness data independently; failed creation/update batches and exhausted IDs leave all state unchanged.

## Reference queue and delay blocks

`EntityToken<Tag>` carries a typed entity reference and a signed integer priority snapshot. `QueuePull` requests a positive number of tokens; unfilled demand remains pending. The default `ProcessMessage<Tag>` is the variant of those two message types. Blocks validate store namespace, 48-bit identity, message type, and port; the store owner remains responsible for existence/liveness. A block cannot admit the same identity twice in a run, including after rejection. No block mutates the referenced columns.

`ReferenceQueue` accepts tokens on port 0 and pulls on port 2. It emits selected tokens on port 1 and rejected tokens on port 3 in a zero-time publication step. Waiting capacity is optional; zero permits only tokens covered by outstanding same-bag demand. Pulls in a bag are applied before admission, then accepted tokens dispatch by FIFO/LIFO/priority using the station rules above. Already accepted waiting work is never evicted. Capacity bounds only the queue remaining after dispatch; selected/rejected publication buffers are in flight. Demand addition checks integer overflow. Queue area integrates waiting count; cumulative waiting time is charged when a token is selected. Reading observations never changes state. Invalid input, statistics overflow, and invalid confluence leave queue, demand, counters, and pending output unchanged.

`ReferenceDelay` delays each token by a finite positive duration, evaluated once at admission by a deterministic, side-effect-free provider and cached as an absolute deadline. A constant-duration constructor is also available. Missing capacity means unbounded parallel delays. A positive finite capacity limits tokens in delay; excess input is published on rejection port 2 at the same time in a later microstep. Successful completions emit unchanged tokens on port 1, ordered by deadline then admission order. Due completions free slots before confluent admission. Duration validation precedes admission even when full; accepted deadlines must be finite and strictly advance the clock. Busy area integrates active token count, and completed residence time sums their actual entry-to-completion intervals.

A finite delay emits `QueuePull` credits on port 3: its initial capacity at time zero and one credit for each completion. Coupling a queue's token output to the delay and those credits back to that queue implements capacity-controlled queue→delay service. The queue must be the sole source of the delay's tokens for the credits to represent all free slots. At time zero, source arrivals and the initial credit are collected in one queue input bag. A completion's credit can release new work in the following zero-time microstep. Infinite delays emit no credits. Direct bounded-delay arrivals may instead use explicit rejection semantics and ignore the credit port. This is a C++ composition contract; declarative topology validation and resource seize/release bindings are later work.

Both blocks own their calendars, queues, identities, and statistics by value. Clone and checked-step rollback therefore preserve token ownership without cloning or modifying external entity stores. Duration providers must not mutate external state; their captured references must outlive the block. The initial atomics start at simulation time zero. General re-entry, cancellation, time-varying capacity, and preemption remain unsupported.

## Reference seize/release and shared resources

An entity token may carry resource leases, each with pool namespace, request ID, and positive unit count. Lease pool namespaces must be unique within a token. A lease request ID's low 48 bits equal the entity ID; its high 16 bits identify the seize block. Model owners allocate distinct seize-block IDs per pool. Tokens preserve all unrelated leases through queue, delay, and resource stages.

`ReferenceSeize` accepts tokens on port 0 and pool grants on port 2. Each new token produces one `Seize` on port 3 in a zero-time step. The request uses the block/entity composite ID, a positive unit count computed once at admission, the token's priority, and no preemption. A constructor request for preemption fails explicitly. Units providers are deterministic and side-effect free. A token already holding that pool is rejected. Tokens wait in the block until their exact request ID and unit count are granted; unknown, duplicate, or mismatched grants fail atomically. A valid grant appends its lease and emits the token on port 1 in a later zero-time step. In a bag containing grants and arrivals, grants must refer to requests that already existed before that bag.

`ReferenceRelease` accepts tokens on port 0 and requires exactly one lease for its configured pool. Admission removes that lease from the forwarded token and buffers both `Release` on port 3 and token output on port 1. Both publish at the same physical time in the next microstep. Checked DEVS execution commits the pool release and downstream receipt together; a failed receiver restores both. A downstream seize publishes its request in a later microstep, after the release. Other leases are preserved. Duplicate tokens, missing/invalid leases, malformed ports, and wrong store namespaces fail without partial changes.

`TypedResourcePool<Message>` adapts the existing value-owned resource pool to a larger typed message variant containing seize/release/grant/capacity messages. Grants route on output port `1 + (request_id >> 48)`, so each seize block receives only its own grants. It retains the original queue disciplines, safe resize, head blocking, zero-time grants, clone state, and explicit preemption rejection. It also integrates allocated units, total capacity, and waiting count over exact event intervals. Utilization is allocated-unit area divided by capacity area, or zero when capacity area is zero; observations project the last committed state without mutation. Statistics overflow rolls back a transition. Seize/release ports must be coupled to the same configured pool; pool namespace validation belongs to the model owner. These blocks do not access shared mutable entity columns. General declarative topology/schema integration follows the native protocol tests.

## Reference source, sink, and selection

`ReferenceSource` releases pre-existing unleased tokens from a finite schedule. Times are finite, nonnegative, and nondecreasing; identities are unique in the configured store namespace and fit 48 bits. Equal-time schedule order is meaningful and preserved. Output port 0 emits the complete due bag at its absolute deadline. No field mutation or new identity allocation occurs during release; the store owner prepares the records before the run.

`ReferenceSink` is passive and accepts only unleased tokens on port 0. It validates identity uniqueness and namespace and evaluates a pure origin-time provider once per receipt. Origins must be finite, nonnegative, and no later than receipt. The sink records exact receipt time and accumulates end-to-end cycle time; it does not retire or change externally owned entity rows. A failed bag, provider failure, or cycle-sum overflow leaves all records, counters, and time unchanged. A mean cycle time requires at least one receipt.

`ReferenceSelect` supports an ordered list of pure conditions with a final otherwise exit, or a categorical probability vector. Conditions short-circuit at the first true rule; ports are one-based rule indices and the final default is `number_of_conditions + 1`. No conditions means unconditional port 1. An empty callback is invalid. Probability ports are one-based category indices. There are at most 65,535 exits. Every token selects exactly one port, preserves all token/lease metadata, and publishes its cached choice in the next zero-time microstep. Callback failure, invalid identity, and repeated identity roll back the complete input bag. Output and clone never reevaluate the rule.

`rng::Categorical` accepts one or more finite probabilities in `[0,1]` whose compensated sum differs from one by at most `1e-12`. It normalizes by that sum to account for representation error and caches cumulative cutoffs; zero-probability entries cannot be selected. Selection compares the open Philox uniform strictly below each cutoff, with the last positive category handling the floating-point tail. The reference selector uses Philox word zero at seed/scenario/replication/entity/step-zero/stream/draw-zero, validating the complete address even for a deterministic vector. Categorical selection has the same finite-word resolution as the existing uniform transform. These are native library contracts; the declarative loader will validate branch topology and expression units separately.

## Declarative typed DES graphs

A standalone `mode: des` document containing an `entity_type` component selects the typed process dialect. It cannot mix legacy `server`/`router` components with typed blocks. Entity types declare ordered fields (`id`, `type`, and numeric `unit`). Types, ordinary components, parameters, and outputs have globally unique identifiers. Field names are unique within a type and cannot shadow a parameter or reserved expression symbol. Real and integer fields carry dimensions; boolean and string fields cannot enter numeric expressions. Integer fields preserve signed 64-bit values, but an integer read by a numeric expression must be exactly representable within ±2^53. Numeric literals remain dimensionless, so dimensioned durations use a field or parameter.

Each `source` names its entity type and a schedule of `{arrival, values}` records, with an optional dimensionless integer `priority` expression. Records have exactly the declared fields. JSON real fields accept any finite JSON number; integer fields require a JSON integer within signed 64 bits; boolean/string fields require their exact JSON types. Source records are allocated before execution. Type namespaces and component execution order use sorted identifiers. Within each type, sources allocate monotonically increasing IDs in source-ID order and then schedule order, independently of declaration ordering. Equal-time source order therefore uses canonical source IDs. All records remain immutable during a run; source release is the arrival event and sink receipt is process exit, without shared-store mutation.

Every process block explicitly names its `entity_type`. Entity-flow `links` connect compatible types and one named output to one destination; fan-out, cycles/revisits, dangling exits, unreachable blocks, and incoming links to sources are rejected. Sources use `out`; queues and delays use `out` and optional `rejected`; seize/release use `out`. Conditional `select_output` branches have unique ports and unit-compatible numeric comparisons (`lt`, `le`, `eq`, `ne`, `ge`, `gt`), followed by a required `otherwise` port. Probability branches contain only probabilities and require a unique explicit Philox `stream`; branch order determines category index. Every branch, including zero-probability branches, must have one link. Condition expressions use only parameters and numeric entity fields, without event-time symbols or stateful functions.

Queues require an `out` link to a finite-capacity `delay_block`, and that queue must be the delay's sole input. The loader adds the return credit link. Other delays may have unlimited capacity or receive direct arrivals with a required rejection route when bounded. A finite queue also requires an explicit rejection route. Durations are positive, time-dimensioned expressions cached at admission. Queue disciplines and source priority snapshots follow the native block contracts.

`resource_pool` declares nonnegative capacity, positive maximum request units, and FIFO/LIFO/priority discipline. `seize` names a pool and a positive dimensionless integer units expression; `preempt` may only be false. `release` names the pool. The loader assigns unique seize-block IDs per pool and wires request/grant/release messages automatically; users declare only entity-flow links. Static path analysis requires consistent held-pool sets at every merge, one acquisition per held pool, and a matching release before every sink. Nested acquisitions must follow increasing pool-ID order, conservatively preventing circular hold-and-wait. A declared pool must be used by at least one seize. Requests above configured maximum fail; requests above currently available capacity may wait, preserving the existing head-blocking contract.

The runtime runs checked DEVS steps, reads observations after all due microsteps, and leaves unfinished work in its queue/delay/resource state at the horizon. Observation spacing cannot change transitions or addressed choices. Outputs expose component counts and native time-weighted statistics, with zero for means before any elapsed time/completions. Typed DES parameters support ordinary experiment overrides; field values and arrival schedules remain fixed. These contracts do not add dynamic entity mutation, loops, preemption, or hybrid graph integration. Generated sources and scheduled capacities use the explicit extension below.

Queue and seize blocks may override the incoming priority snapshot with a pure admission-time provider. The result is cached in the forwarded token, and neither dispatch, grant, clone, nor output reevaluates it. Queue providers are allowed only with priority discipline and run before admission/overflow arbitration; a failed provider rolls back the entire bag. Seize evaluates units and priority against the original incoming token, then caches both. The declarative `priority` expression has dimension one and must evaluate to a signed 32-bit integer; missing expressions retain incoming priority. Entity fields remain unchanged.

### Generated typed arrivals and capacity schedules

A typed source uses exactly one explicit `schedule` or `generator`. A generator has `count` (0–1,000,000), nonnegative `start` (default zero), one constant typed `values` record, and `interarrival` with `kind: constant` plus a time-dimensioned `interval` expression, or `kind: exponential` plus an inverse-time `rate` expression and explicit stream. Expressions read parameters only and evaluate once per run. Every interval/rate must be finite and positive even for count zero. The first entity arrives after one interval from start; each later time adds its interval to the previous arrival. Times must advance and remain finite. Entities beyond the observation horizon remain scheduled, with stable allocated IDs but no emitted count yet. Identity allocation is identical to explicit source records; source count changes therefore change the IDs of later canonical sources of the same type.

A delay's `duration` may instead be an exponential policy `{kind: exponential, rate: expression, stream}`. Rate expressions may read numeric entity fields and parameters and have inverse-time dimension. The positive rate and sampled duration are evaluated once at admission. Each exponential arrival or service draw uses word zero at seed/scenario/replication/entity/step-zero/stream/draw-zero. Every stochastic source, delay, and selector owns a distinct explicit stream, checked even for empty sources. This adds no anonymous random expression calls.

Resource capacity accepts a bounded nonnegative integer or a dimensionless parameter expression. An optional `schedule` supplies strictly increasing `{time, capacity}` changes inside the horizon, evaluated from the run's parameters before execution. Capacity expressions must produce exact integers between zero and one million. Changes are queued as external pool events on its capacity port. The native release-before-resize rule applies to releases in the same pool input bag; a release still traveling through another block's microsteps is not yet available. Shrinking below allocated units fails atomically, including when delivery finishes at that physical time but its release message has not arrived. Statistics integrate actual committed capacity, and observations use allocated-unit area divided by capacity area.

## Synchronous ABM population semantics (initial implementation)

A population stores records in monotonically increasing agent-ID order. Spawn appends a record with a new ID. Despawn marks it inactive; IDs are never reused. A phase takes a snapshot of all records at the start of that phase, computes each active agent's next value against that immutable snapshot, and commits all values together. Later phases see the preceding phase's committed values. This is Jacobi semantics within a phase and ordered composition between phases. Inactive records remain addressable in snapshots but are excluded from active count and updates. Spawn/despawn and phase-list changes are rejected during a step. If any phase throws, no phase's new values commit. Typed populations, async timers, statecharts, spatial/network structures and message topics have separate contracts below.

## Typed populations and asynchronous timers

`TypedPopulation<Tag>` reuses the runtime column store's exact real/integer/boolean/string schema, namespace-checked references, monotonic 48-bit IDs and retirement tombstones. It exposes read-only storage, atomic spawn/update/retire operations and synchronous phases. Each phase reads one immutable store snapshot in increasing live-ID order; all returned records are schema-checked before committing the phase. A whole step is transactional across every phase. Population mutations, phase changes and recursive steps from a callback are rejected. Callbacks must be pure: external side effects and mutable captured state cannot be rolled back. Tombstones are retained rather than compacted or reused in this CPU reference increment.

`AsyncPopulation<Tag>` owns the same typed store and a binary timer heap ordered by `(absolute time, agent ID, timer ID)`. Timer IDs are monotonic unsigned 64-bit sequence numbers (the maximum value is reserved for exhaustion); they are never reused after a successful commit. A timer contains an opaque nonempty kind and generation token. Times must be finite, nonnegative and at least the committed clock; exact equality defines simultaneity, with no tolerance. Scheduling for an inactive/foreign agent fails. Explicit cancellation must name an existing timer; retirement cancels all that agent's remaining timers. A cancelled timer cannot fire.

Rules return an effect batch: timer cancellations, unique record updates, unique retirements, births in declaration order (each with optional initial timers), then new timer schedules. Updating and retiring the same agent in one batch is rejected. The whole batch is validated/staged before commit. A timestamp step stages the clock advance, repeatedly removes the least due timer and applies its rule against the latest staged store. Thus simultaneous events have ordered event semantics, not Jacobi semantics. Newly scheduled same-time timers re-enter the heap order; a configurable positive event budget bounds zero-time cycles. Every event at that timestamp, including births, ID allocation, cancellations and newly scheduled timers, rolls back if any rule or validation fails. Earlier successful timestamps remain committed. `run_until(horizon)` includes all events at the horizon and advances an idle clock to it; it rejects backwards/nonfinite horizons. Queries do not advance clocks. Copies of idle populations own independent stores/calendars. The population DEVS wrapper and declarative subset below add external input bags and kernel execution around these native schedulers.

## Flat typed-agent statecharts

The native scheduling primitives below are also used by the population DEVS wrapper described after this section.

`Statechart<Tag>` is an immutable rule definition over three distinct reserved agent fields: integer state, real entry time and integer entry generation. State IDs are declared nonnegative integers; transitions have unique nonempty names, declared source/target states, integer priority and one of message, timeout or constant-rate triggers. Lowest `(priority, transition name)` wins when several message transitions are enabled. Guards inspect the pre-transition typed store; one selected action returns a full record. The engine overwrites the reserved fields after the action and validates the complete record. Entry increments generation (bounded to 0–65535 for the RNG layout), so self-transitions also re-enter and re-arm timers. Initialization requires generation -1 and explicitly selects the initial state. Failed evaluation leaves the caller's store untouched.

On entry, timeout transitions arm at `entry time + duration` (finite nonnegative duration); constant-rate transitions draw one exponential waiting time using Philox `(scenario, replication, agent ID, entry generation, transition stream, 0)`. Every rate transition owns a distinct stream within a chart. Rate zero is disabled; positive rates must produce a finite strictly later time. Positive timeout durations must also advance the floating-point clock; zero timeouts are allowed and bounded by the async event budget. Timed transitions are armed in `(priority, name)` order. Guards are evaluated at firing, and a false guard consumes that timer without resampling until re-entry; rates are constant between entries. Stale generation/source timers are ignored. Re-entry leaves stale timers in the population calendar, where they may cause harmless no-op events before being removed; callers must not use timer counts as transition counts. Message triggers are delivered explicitly by the caller; topic-broker/DEVS delivery is not implicit. Chart timers use a reserved prefix plus transition name; models must not forge them or share that prefix between multiple charts on one agent. This increment supports flat states, not hierarchical/parallel states, entry/exit actions, or changing hazards. Definitions and all callbacks are pure so returned effects can participate in the population's timestamp transaction.

## Typed population DEVS wrapper and declarative ABM

`PopulationAtomic<Tag>` owns either a typed synchronous population or an asynchronous population. Port 0 accepts timestamped commands `(agent, sequence, kind)`; port 1 publishes a value-owned result containing the committed store, processed timer trace and ordered commands. Input bags validate exact timestamps, sort by `(agent ID, sequence)`, and reject duplicate keys. Input handlers are pure functions returning population effects. Commands must target active agents. Synchronous effects may update, retire and spawn records but cannot use timers or cancellations. A synchronous population starts at zero and ticks at integer index times `dt`, with finite strictly advancing deadlines.

At time `t`, the async wrapper drains previously due timers, applies the scheduled lifecycle batch, processes the sorted command bag against sequential staged snapshots, then drains newly armed timers at `t`. Its event budget bounds the combined timer trace. The sync wrapper processes inputs before the phases whose tick is `t`; later same-time microsteps do not repeat that tick. This ordering is an explicit convention. The complete wrapper transition (including timers, inputs, phases, identities, clock and result preparation) commits atomically. An earlier unprocessed deadline cannot be skipped. Outputs are empty during event detection; a zero-time publication microstep emits the committed result. Confluence during publication emits the old result and stages a new transaction at the same time. Deep clones support checked-kernel rollback, including downstream publication failure, without repeating a previously committed transition. Callbacks remain pure; populations inserted into a simulator must have clocks matching insertion time.

`mode: "abm"` declares exactly one `population` component with `execution: "sync"` or `"async"`, typed fields (all with units), and initial agent records assigned IDs in array order. Synchronous `phases` are ordered lists of field assignments; every assignment in a phase reads the same agent/population snapshot. Expressions read numeric/boolean fields, parameters and explicitly declared neighborhood query aliases, with `NONNEGATIVE` as the only function; time and neighbor symbols are not implicit. Integer arithmetic is restricted to exact f64 integers within ±(2^53−1), boolean results to 0 or 1. String fields are stored but cannot be expression targets or numeric outputs. Initial records contain exactly the declared fields. Field and parameter names cannot collide.

Async populations declare one flat `chart` with reserved state/entry/generation field bindings, initial state, states and transitions. Agents begin with generation -1; initialization occurs at time zero before observation. Guards are dimensionless arithmetic expressions interpreted as nonzero; transition assignments read the pre-transition snapshot and cannot assign engine-owned fields. Timeout/rate expressions read parameters only, are evaluated once per run (including scenario overrides), and must have time/reciprocal-time units. Rate streams are unique, even for disabled rates. Scheduled `messages` supply time, agent, sequence and event; equal-time entries are canonicalized by the wrapper. No arbitrary timers, births or retirements are exposed in this IR increment. Outputs select an agent's numeric/boolean field, active count, sum of a numeric field, or count in a declared state. The existing `time.dt` controls observations and synchronous ticks; every event and publication microstep at or before the observation time is drained exactly. Spatial/network bindings and neighborhood queries follow the contract below. Population topic delivery follows the broker contract below. Hierarchical statecharts, mutable hazards and cross-population IR links remain unsupported.

## ABM spatial indexes and neighborhoods

Population spatial bindings construct value-owned indexes from live typed records. Grid bindings require two distinct integer fields; continuous bindings require one to three distinct numeric fields with exact conversion of integer coordinates. Coordinate records are authoritative: periodic normalization applies to the index only, so stored coordinates may remain unwrapped. A pure population validator checks the initial store and every complete mutation batch, synchronous phase, and asynchronous event effect. Complete proposed positions are validated together, allowing swaps but rejecting occupied destinations and bounded-domain violations even when no neighborhood is queried. A failed validator restores the enclosing step/timestamp, including clocks and identities. Synchronous external effects use a single validated mutation batch.

Declarative populations may bind one grid or continuous space and one simple network initially declared over the initial agent IDs. Population-owned network membership and scheduled edge edits are specified below. Named queries select `space`, outgoing `network`, or all live `population` neighbors, in increasing ID order. Self is excluded unless `include_self` is true. Queries compute `count`, `sum`, `mean` (zero for an empty neighborhood), or `count_same` (exact field equality with the requesting agent, including strings). Count results are dimensionless; sum/mean carry the numeric field's unit. Grid coordinates/radii are dimensionless; continuous coordinates and radius have the declared space unit. The declarative grid is capped at one million cells. Grid radius is an integer and selects Moore or von Neumann distance; continuous radius uses Euclidean distance. Query aliases cannot collide with fields/parameters and cannot reference other queries. They are available to phase expressions, guards and actions, and as per-agent outputs. Phase/guard/action expression environments evaluate all declared queries against their population snapshot; async events see earlier events' staged updates. Output sampling evaluates the selected query. Query evaluation checks numeric finiteness and exact integer-sum range. Topic delivery, lifecycle actions and scheduled network edits are specified below.

`GridSpace` is a two-dimensional single-occupancy integer lattice with positive dimensions, optional toroidal wrapping, and either Moore (Chebyshev distance) or von Neumann (Manhattan distance) queries. Each ID has exactly one cell and each cell at most one ID. Wrapped placements/moves normalize coordinates; bounded placements reject coordinates outside the lattice. Queries include cells at distance at most the nonnegative integer radius, exclude the requesting agent by default, deduplicate wrapped cells, and return IDs in increasing order. Radius zero therefore has no neighbors when self is excluded. Space copies are independent snapshots. A movement batch validates unique active IDs and all destinations, releases all moving agents' old cells together, then places them together: swaps are legal, duplicate destinations and collisions with stationary agents fail without mutation. Add/remove and failed batches preserve both position and occupancy indexes.

`ContinuousSpace` supports one to three dimensions, finite half-open bounds `[lower, upper)`, one positive bin width, and optional periodic boundaries. Positions are finite and normalized when periodic; unsupported overflow in normalization fails. Agents may share positions. Uniform spatial bins select candidates; exact Euclidean distance (minimum-image distance when periodic) filters them. Radius is finite and nonnegative; boundary equality is included and requesting-agent exclusion removes only that ID, not co-located peers. Queries return increasing IDs and do not mutate the space. Batch movement is transactional and copies provide immutable neighborhood snapshots. Bin widths that do not divide the domain and small periodic domains must not lose or duplicate neighbors. The population owner controls ID creation/liveness and uses the same snapshot for every agent in a synchronous phase.

## ABM networks and bounded message topics

Typed populations own a transactional publication outbox. A synchronous phase may supply a pure publisher beside its record rule; both read the same pre-phase snapshot, and publications accumulate in phase/agent/list order. They become deliverable only after the complete tick, so no delivery interleaves Jacobi phases. Async effect batches may contain publications, committed with records/timers; a selected statechart transition evaluates its publisher against the pre-transition store and assigns the transitioning agent as sender. Initialization, stale timers and false guards do not publish. Payloads must be finite with a nonempty topic and live namespace-correct endpoints. Retiring a queued sender/receiver is rejected until the outbox is drained. Native callers can inspect or take the outbox; taking it is an explicit mutation. Copies and failed steps retain independent unchanged outboxes. Wrapping a population with an undrained outbox is rejected because its publications lack an owner delivery time.

At a wrapper timestamp, already-due timers, scheduled lifecycle batches and ordinary commands first update staged state and buffer their publications. External topic publications and their reply cascades drain before that buffered batch; separating these batches prevents collisions between explicit external sequences and generated sequences. Generated sequences are allocated as publications enter the broker, sharing the existing sender counters with replies. Native topic-handler effect publications join the next round before its explicit reply list. Afterwards the wrapper repeatedly drains newly due timers and their publications until quiescence. The timer budget is cumulative across these drains, and the delivery budget spans all rounds. Synchronous tick publications drain after every phase has completed, without rerunning phases. Round numbers increase across nonempty batches in the complete wrapper transition. Every outbox, generated ID, delivery, timer and phase effect participates in the same rollback boundary.

Declarative synchronous phases and chart transitions may include `publish` arrays with the same unit-checked topic/value bindings as topic replies. Here `receiver` is `self`, `broadcast`, or an existing numeric agent ID; `sender` and `message_value`/`message_sender` require a topic-handler context and are rejected outside it. Publication expressions read pre-phase/pre-transition fields, parameters and neighborhood aliases. Phase `assign` remains required (an empty array is valid). Transition selection, priorities, guards and generation checks retain their meanings. Topic handlers still do not implicitly fire chart transitions; delayed messages, dynamic subscriptions and cross-population routing remain outside this subset.

Population topic delivery is owned by `PopulationAtomic`, with numeric payloads and optional pure topic handlers. Port 2 accepts timestamped publications with namespace-checked live sender/optional receiver references and explicit sender-local sequences. An absent receiver broadcasts to all live agents, including the sender. At a timestamp, already-due async timers run first, then ordinary port-0 commands, then topic delivery rounds, then newly armed immediate timers (or the synchronous tick's phases). All external publications in one bag enter the first pending batch together. Each round flushes the broker, visits topics lexicographically, publications by `(sender, sequence)`, and recipients by increasing ID. Each callback reads the latest staged store; returned assignments are simultaneous within that callback. Replies are buffered until the next round at the same physical time. Topic capacity counts publications in one pending round, not broadcast fan-out.

Handlers return population effects and numeric publications. Reply sender is the handling agent; reply sequences come from a transactional monotonic counter per agent, separate from externally supplied sequences. Uniqueness remains per topic/pending batch. Publications require finite payloads and live endpoints. Topic handlers cannot create or retire agents in this increment; native async handlers may schedule/cancel timers, and sync handlers reject timer effects. A positive delivery budget counts every recipient callback, including guarded no-ops. Overflow, unknown topics/endpoints, duplicate external sequences, callback/spatial failures, reply cycles or later timer/phase failures restore the whole wrapper transition, including broker buffers, reply identities, counters and clock. Empty rounds clear visible buffers. Result publication includes the canonical delivery trace; checked downstream retries do not redeliver messages.

Declarative populations may declare `topics` (`id`, `capacity`, payload `unit`, optional dimensionless `guard`, `assign`, `publish`) and scheduled `publications` (`time`, `topic`, `sender`, optional `receiver`, `sequence`, numeric `value`). A topic handler runs for each live recipient. Expressions read recipient fields/queries/parameters plus reserved `message_value` in the topic unit and dimensionless `message_sender`; those names cannot be fields, parameters or queries when topics are used. Assignment RHSs and reply expressions read the pre-handler snapshot. Each reply selects a declared topic, an expression `value` in its payload unit, and `receiver`: numeric agent ID, `self`, `sender`, or `broadcast`. Reply list order allocates sequences. Async handlers cannot assign chart-owned fields; receipt does not implicitly trigger a chart transition. A false guard emits nothing. A population `delivery_budget` defaults to 100000. Delayed replies, dynamic subscriptions, topic-handler lifecycle effects and cross-population routing remain later work.

`CsrNetwork` stores a simple directed or undirected graph using sorted stable vertex IDs, CSR row offsets, and sorted adjacency IDs. Isolated vertices are retained. Construction rejects duplicate vertices, missing endpoints, self-loops, and duplicate edges (including reversed duplicates in an undirected graph). Directed neighborhoods are outgoing neighbors. Reads are immutable; copied networks are independent phase snapshots. Edge edits validate the complete addition/removal batch before committing: removals must exist, additions must be new, neither list may repeat an edge, and an edge cannot appear in both lists. A failed edit preserves all adjacency and offsets. Vertex/liveness changes belong to the population owner, which can construct a replacement graph explicitly.

Graph generators operate on sorted vertex IDs and produce undirected simple graphs. Erdős–Rényi tests every unordered pair in lexicographic order with a Bernoulli draw; the pair ordinal occupies the 48-bit entity address. Watts–Strogatz requires an even ring degree smaller than the vertex count, builds the nearest-neighbor ring, then considers clockwise edges by distance and source index for rewiring. A selected edge chooses uniformly among vertices other than its source and current neighbors; if none exist it remains unchanged. Barabási–Albert starts from a star on `m+1` vertices and attaches each later vertex to `m` distinct existing vertices with probability proportional to their degrees before that vertex's edges are added. The initial star center is the first sorted ID. All generator probabilities and Philox context fields are validated even for empty/degenerate results.

Watts–Strogatz and Barabási–Albert use an addressed word sequence with entity zero and a 32-bit ordinal split between step (high 16 bits) and draw-index (low 16 bits), always word zero. Exhaustion fails instead of wrapping. Integer choices use rejection sampling: discard words below `2^32 mod bound`, then take the remainder modulo bound. Bounds are 1–2^32; endpoint probabilities still consume their specified draw. Barabási–Albert selects each target from cumulative degree weights in sorted ID order, sets that selected weight to zero, and reduces the remaining total before the next choice. Vertex/edge declaration order cannot alter a generated graph. Different graphs must own different streams. These generator contracts fix the initial graph and draw traversal; they do not claim seed-for-seed equality with another library's default PRNG.

`TopicBroker<Payload>` owns declared named topics with a per-publication-batch message capacity (zero is allowed). A message has sender ID, optional receiver ID (absent means broadcast), and an explicit sender-local sequence number. The tuple `(topic, sender, sequence)` is unique within a pending batch. `publish_many` validates topic existence, duplicate keys, and all topic capacities and stages payload copies before committing anything; overflow is an error, never silent loss. Payloads are owned by value. `flush` replaces each visible batch with its pending messages ordered by sender then sequence, then clears pending buffers for the next batch. An empty flush clears old visible messages. Messages published while consuming a visible batch remain pending until the next flush. Reads for a receiver include its direct messages and broadcasts in that same canonical order. Broker copies are independent snapshots; callers own population/liveness checks and flush timing. This library does not itself impose wall-clock delivery or mutate agents, and it is distinct from the resource-ownership broker.

Network row spans remain valid across reads and failed edits; a successful edge edit invalidates them. Broker visible references remain valid across topic declarations and publications (including publication while iterating a visible batch); a successful flush invalidates them. Assigning or destroying either object also invalidates its views.

## Event-to-SD pulse bridge (initial implementation)

`ClockedSD` is an atomic wrapper around the SD stock-and-flow model. It schedules fixed grid ticks but accepts pulses at any finite event time between ticks. Grid timestamps are calculated as integer tick index times `dt`, matching the IR observation grid without cumulative addition drift. On an off-grid event it integrates from the last committed time to the event time, then applies all pulses in bag order. At a tick with an event, the confluent transition integrates the interval ending at the tick, applies pulses, then schedules the next grid tick. Thus a pulse at `t` never changes the interval that ends at `t`; it can change every later interval. Splitting an Euler interval at an off-grid event changes the numerical discretization, so that split is part of the explicit reference semantics and must be compared at the same event times across backends.

`EntityToPulse` converts completed DES entities to numeric SD pulses through a zero-time DEVS microstep. The bridge emits once after receiving a same-time input bag, summing its mapped amounts. If a completion coincides with an SD grid tick, the continuous interval commits first; the bridge's zero-time output then changes the stock at that same physical timestamp. The typed coupled-model payload is a `std::variant` of entity and numeric amount, with explicit extraction at each primitive boundary.

`PopulationToStock` aggregates a committed synchronous ABM population into an SD stock. After ordinary events and the SD interval ending at tick `t` have committed, the orchestrator runs the population's declared phases and then sends one tick trigger to the bridge. The bridge visits active records in stable agent-ID order, sums a finite nonnegative numeric contribution per agent, and emits the difference from its previously published total as one zero-time stock pulse. A negative pulse represents an agent departure or a lower contribution. The target stock must start at zero and be reserved for this aggregate if equality with the active-agent sum is required. A failed contribution calculation leaves the bridge's published total unchanged. The bridge does not change or own the population, and its population reference must outlive it. Its first trigger publishes the complete current population; repeated triggers with no change emit nothing.

`PopulationToCapacity` is a C++ bridge from a committed synchronous ABM population to a DES resource pool. After ordinary DES events at tick `t` are drained and the population phase commits, one `CapacitySample` trigger sums nonnegative integer capacity units from active agents in stable ID order. The bridge emits one `SetCapacity` message in a zero-time microstep, even if the total is unchanged. The pool applies it through its dedicated capacity port; any newly feasible FIFO grants emit in a later microstep at the same physical time. A failed contribution or sum overflow leaves the bridge state unchanged. The population reference must outlive the bridge. Contributions represent **total eligible resource units**, not free units after current allocations. An agent departure that would take total capacity below active allocations must first release or transfer those allocations; the pool rejects an unsafe reduction instead of silently preempting work. This bridge does not record ownership of granted units; the broker below does. The separate transactional `agent_pool` IR mode below declares one combined population, pool, and ownership broker.

`AgentPoolBroker` is the C++ ownership ledger for those aggregate pool grants. A pool `Grant` is sent to its input port; the broker assigns the requested integer units across active agents in ascending stable ID order, using each agent's total capacity less units already assigned. It emits one `AgentAssigned` event per agent share in a zero-time microstep. A `Release` must be delivered to both the pool and broker; the broker frees the request's exact shares and emits `AgentUnassigned` events. Within one broker input bag, releases precede grants. Request IDs cannot be reused. A bag that cannot be assigned, has duplicate request IDs, or observes an agent capacity below already assigned units is rejected without committing any ledger change. The broker's `allocated_to(agent_id)` view is available to ABM rules. Before committing an ABM departure or capacity change, the orchestrator can pass the candidate population snapshot to `preflight`; this rejects changes that strand work on an agent. The broker does not mutate population records, and cross-atomic pool/broker delivery is not yet one transaction. The orchestrator must drain and validate the release and grant microsteps before changing population capacity.

`TransactionalAgentPool` is the reference transaction owner for one population, pool, and broker. A transaction at finite nondecreasing time `t` stages releases in the broker, applies agent value updates, departures, and hires to a copied population, runs requested synchronous ABM phases, validates per-agent ownership, then sends releases, the new total capacity, and new seize requests to a copied pool in one bag. Registered population phases may run first. Additional transaction phases receive a stable snapshot and the staged broker ledger; all active agents in one phase read the same snapshot, and later phases see preceding phase results. This lets rules respond to released work without exposing an intermediate population. The transaction maps any resulting grants into the copied broker and checks that pool and broker allocation counts and units agree. The candidate state and time replace the live state only after all steps succeed. A failed request, unsafe departure, invalid release, phase, or capacity callback leaves the live population, queue, allocations, IDs, and time unchanged. A release and departure in the same transaction shrink capacity before the pool can regrant that freed unit to an existing waiter. Agent capacity callbacks and phase rules must have no externally visible side effects. `TransactionalAgentPoolAtomic` exposes this owner as one DEVS component with a transaction command input and a zero-time committed result output. Other components see only the result, never intermediate pool or broker state. To retry an injected command after a transition failure, run the simulator in checked transactional mode; ordinary simulator execution still aborts without whole-step rollback.

## Declarative IR 0.1 agent-pool subset

`mode: "agent_pool"` declares one integer-capacity population, one FIFO resource pool, and an explicit `agent_pool` bridge with matching units and endpoints. The population's initial `agents` array assigns stable IDs starting at zero. Each scheduled change has a finite nondecreasing time and may contain releases, capacity updates, departures, hires, and seize requests. Hires receive the next stable IDs. Each change is one `TransactionalAgentPool` commit in that order; separate changes at the same time follow array order. A failed change reports `IR_AGENT_POOL_RUNTIME` at its schedule index and publishes no partial state. Unsafe capacity reductions and departures are rejected, so active assignments are never silently moved or preempted. Request IDs are unique for the run and cannot be reused after release.

The runner applies all changes at or before an observation grid time before sampling, treating decimal timestamps within eight floating-point epsilon units of a grid time as tied. Off-grid changes therefore first appear in the next grid row. Pool outputs expose capacity, available units, allocated units, allocation count, and waiting count. Population output exposes active count; agent outputs expose alive, capacity, and allocated units by stable ID. A request's `granted` output is cumulative: it stays one after that request is released. The fixture in `models/agent_pool.ir.json` checks a release-plus-departure while a request waits, a later hire that grants it, and a safe capacity reduction. This subset uses scheduled transactions and integer capacities; typed agent state, conditional/statechart rules, stochastic hiring, process routing, and cost or revenue behavior are later IR work.

The optional `agent_pool.phases` array declares named rules as `{id, capacity_expr, unit}`. A schedule entry invokes them with `phases: [id, ...]`; an entry may contain only phase invocations, and a name may be invoked more than once. Definitions alone do not run. Each rule uses the pure arithmetic expression language and must produce the pool's capacity unit. `capacity` and `allocated` read the current agent; `total_capacity` and `active` read the active population snapshot at the start of that phase; `total_allocated` reads the staged ownership ledger. Capacity and allocation quantities carry pool units. `active` and `agent_id` are dimensionless; `t` carries the model time unit and is the scheduled transaction time, including off-grid times. Unknown symbols, functions, references, duplicate IDs, and mismatched units are lint errors.

The transaction releases assignments, applies explicit updates, departures, and hires, then executes the requested phases in order before granting queued or new requests. Every active agent in one phase reads the same snapshot; the next phase sees all results from the preceding phase. Hires participate immediately and departed agents do not participate. Rules read ownership after releases and before new grants. Each result must be finite and exactly integral in `[0,1000000]`; no rounding or clamping occurs. Empty populations execute no per-agent expressions. Ownership is checked against the final candidate population, and an unsafe result rejects the entire transaction. An evaluation failure reports `IR_AGENT_POOL_PHASE` at the schedule invocation with the phase and agent IDs; no release, workforce change, earlier phase, queue change, or clock advance from that transaction commits. `models/agent_pool_phases.ir.json` exercises redistribution from population totals, sequential and repeated phases, hires/departures, and queued grants; tests compare its trajectory with a hand oracle and independently written C++ rules.

## Agent-pool engagement delivery

`AgentPoolProcess<Agent>` owns a transactional agent pool, engagement records, and a completion calendar. Each arrival declares a unique request ID, positive integer capacity units, and a finite positive duration. It enters the pool's FIFO queue and starts only when all requested units can be assigned. A large request may block smaller requests behind it. Delivery occupies its exact agent shares until `start + duration`; a later capacity increase does not shorten an engagement already in service. Completion automatically releases every share. This is one non-preemptive delivery stage; cancellation, preemption, skill matching, routing, and stochastic duration distributions are not implemented here.

At a completion timestamp the process stages all due releases in ascending request-ID order, then applies simultaneous workforce changes and synchronous rules, and finally admits waiting and newly arrived engagements. Existing waiters precede new arrivals, whose order is their array order. The pool, broker, population, records, completion calendar, statistics, and clock commit together. Any failure restores the previous state, including a completion that was being processed, so the same event can be retried. A computed completion must be finite and strictly later than its start; overflow and durations too small to advance the floating-point timestamp are errors. The C++ caller must process `next_completion()` before any later event; `apply` rejects a skipped completion.

In the IR, optional `agent_pool.delivery: {id, time_unit}` enables this stage; `time_unit` must match the model time unit. Schedule entries use `engagements: [{request_id, units, duration}, ...]` and may combine them with workforce changes and phases. Delivery owns all requests and releases, so manual `requests` and `releases` fields are rejected in this mode. Delivery schedule times must be strictly increasing: combine all explicit actions at a timestamp in one entry so a workforce change cannot follow a premature regrant. The runner merges completion events with the explicit schedule, processes the earlier event first, and combines events with exactly equal binary64 timestamps into one transaction. It does not merge near-but-distinct event times; the existing grid tolerance applies only to observation sampling. Completions are processed even when no explicit schedule entries remain. Engagements still queued or running at the observation horizon remain unfinished.

Outputs selected by `process` expose `accepted`, `started`, `completed`, `in_service`, `waiting`, `wait_total`, and `cycle_total`. Counts satisfy `accepted = waiting + in_service + completed`. `wait_total` sums start-minus-arrival for engagements that have started; `cycle_total` sums finish-minus-arrival only for completed engagements. Neither includes a partial duration for unfinished work. Request outputs add `in_service` and `completed`; the existing `granted` output remains cumulative. The fixture `models/agent_pool_delivery.ir.json` checks a departure coinciding with completion, a hire starting queued work, simultaneous completions with phase execution, a multi-agent engagement, and an off-grid final completion against an exact hand schedule. C++ tests also cover calendar rollback and retry, FIFO blocking, and completion-time overflow.

Delivery also exposes cumulative time integrals through the `process` selector. Each committed event adds the preceding interval's duration times its pre-event value; instantaneous arrivals, completions, phases, and workforce changes add no area. This integrates piecewise-constant event state directly, with ordinary binary64 rounding and no observation-grid quadrature.

| Metric | Integrated quantity | Units |
|---|---|---|
| `capacity_time` | Total eligible capacity | Pool unit × model time unit |
| `allocated_time` | Units assigned to in-service engagements | Pool unit × model time unit |
| `available_time` | Unassigned eligible units | Pool unit × model time unit |
| `queue_time` | Waiting engagement count | Engagement × model time unit |
| `service_time` | In-service engagement count, regardless of staffing units | Engagement × model time unit |
| `population_time` | Active agent count, including agents with zero capacity | Agent × model time unit |

`utilization = allocated_time / capacity_time`; it is a ratio of integrals over the full window, not an average of sampled utilization or a denominator based on the final workforce size. `mean_queue`, `mean_in_service`, and `mean_headcount` divide `queue_time`, `service_time`, and `population_time` by elapsed window time. Zero-capacity and zero-duration denominators return zero by convention. The IR window starts at zero. The C++ process starts its window at the supplied pool's clock when it takes ownership. `capacity_time = allocated_time + available_time` within floating-point rounding. Queue and service integrals include unfinished work through the observation horizon, unlike the cohort totals `wait_total` and `cycle_total`. After all work finishes, queue-time equals total waiting time, and queue-time plus service-time equals total cycle time. A multi-agent engagement counts once in service-time but contributes all assigned units to allocated-time.

`AgentPoolProcess::time_statistics(t)` returns a read-only projection from the last committed event through `t`. It cannot look back before that event or forward past a pending completion; the caller must process intervening events first. Querying at a pending completion's timestamp may return the interval areas without executing that completion. Queries do not commit their projected tail, so changing observation density cannot change accumulated results. The IR drains events before each observation and uses the later of the printed grid tick and the last drained event time for its statistics horizon when their binary64 values differ by the documented grid tie tolerance. Failed transactions roll back their accumulated areas with all other state; integral overflow is an error, including during a read-only projection. `models/agent_pool_utilization.ir.json` verifies changing headcount and capacity, off-grid completion, unfinished work, and idle time against hand-derived areas. Its final utilization is `5.75 / 6.5 = 23/26`.

## Agent-pool delivery as a DEVS component

`AgentPoolProcessAtomic<Agent, Message>` wraps staffed delivery as one DEVS atomic. Its default message type is `AgentPoolProcessMessage<Agent>`, a variant of `Change` and `Result`; callers may supply a wider variant containing those types to couple through explicit adapters to SD or other components. Input port 0 accepts delivery changes, and output port 1 publishes committed results, including staffing grants, ownership changes, and completed request IDs. Construct it from an idle transactional pool or an initial population, capacity function, and maximum request size. A supplied pool's clock must match the simulator time when it is inserted. `core()` provides read-only process state and statistics.

There are two internal event states. A completion deadline first produces no output; its transition processes all due completions together with any inputs in that DEVS step. After a successful commit, the result is pending for a zero-time publication step. Repeated `output()` calls only copy that result; they do not rerun phases or calculate speculative grants. With input at a completion deadline, the confluent transition releases due assignments, applies workforce actions and phases, then grants queued work in one transaction. With input at a publication step, the old committed result is emitted first and the new transaction's result is published in a subsequent microstep. Completions, clocks, and ownership use exact absolute timestamps, including a subsequent transaction at the same physical time.

One input bag may contain multiple arrival-only changes and at most one change with workforce actions or phase execution. All commands must declare the exact event timestamp. Arrivals concatenate in deterministic input-bag order, while the consolidated workforce command runs before any grants. Two workforce commands, incorrect ports or payloads, timestamp mismatches, duplicate engagement IDs, and manual staffing requests or releases are errors. Consolidating workforce actions avoids silently choosing an order for noncommuting phases. Actions arriving in a **later microstep** at the same physical time form a separate transaction: they cannot retroactively change an earlier grant. Send changes that must precede a completion's regrant in that completion step's bag.

The process and atomic support deep copies of their owned population, broker, queue, calendar, records, statistics, and pending result. Use the simulator's checked transactional APIs to retry a failed coupled step. A failed completion restores its calendar and workforce input; a downstream failure during result publication restores the pending result without undoing or rerunning the transaction committed in the preceding step. Callback purity remains required; simulator rollback cannot restore external side effects. Direct callers of the elapsed-only transition supply the authoritative time in the command; simulator execution uses the timestamp-aware hook to reject even distinct timestamps whose subtraction happens to round to the same elapsed value.

`tests/hybrid_agent_pool_process_atomic_tests.cpp` compares the complete result trace with the standalone delivery engine and exact hand totals, couples completion counts into an SD stock, checks output purity and both confluent states, retries phase and downstream failures, and exercises floating-point deadline and ownership-clock regressions. The standalone `agent_pool` runner retains its event loop as a reference. The `agent_pool_sd` subset below exposes one declarative composition of the wrapper.

## Declarative staffed delivery to SD

`mode: "agent_pool_sd"` combines an `agent_pool` section with mandatory `delivery`, an `sd` section containing parameters and stock/flow components, and exactly one `completion_to_stock` bridge. The agent-pool schedule, named capacity phases, ownership constraints, and strictly increasing schedule times keep their existing contracts. IDs are unique across SD parameters/components, population, pool, delivery, and phases. SD flow expressions cannot refer to `dt`, because completion events may split an integration interval. Tables, delays, feedback to staffing, multiple bridges, and arbitrary process graphs are outside this subset.

The bridge's `from` must name the delivery process; `to` must name an SD stock. Its positive finite `amount` has that stock's units and is added once per completed engagement, regardless of staffing units. A batch of N completions publishes `N * amount` as one pulse. Staffing-only results emit no pulse and do not split an SD interval. Overflow in a batch amount, stock update, or SD flow is an error. This is a constant amount per completion; the fixture's revenue is a synthetic example, not a fee recognition or calibrated economics model.

The runner constructs a delivery atomic, an `AgentPoolCompletionToPulse` adapter, and `ClockedSD` on the shared DEVS clock. It runs checked transactional steps and drains all same-time publication and pulse microsteps before observing. Completions and workforce changes scheduled at exactly the same timestamp commit together before a pulse is published. SD integrates up to each pulse time using its preceding stock state, then applies the pulse. Its tick calendar and external transition use the kernel's exact absolute timestamps, so off-grid inputs cannot shift a subsequent tick through elapsed-time rounding. Parameter overrides apply to the declared SD parameters.

Observations include the existing pool, population, agent, request, and process selectors plus `{id, stock}`. Rows preserve output declaration order. The coupled runner drains events with timestamps **at or before** `step * dt`, including every microstep at the horizon. Unlike the standalone agent-pool observation convention, it does not pull a completion a few ulps after the grid time into the earlier sample. A completion one representable instant after the horizon remains unfinished. This preserves a single exact event clock for both delivery and SD; observation tolerance never changes physical event order.

`models/agent_pool_sd.ir.json` completes five engagements at times 1, 2, 2, 3, and 3.25, with simultaneous departures, a hire, capacity phases, queued work, and a multi-agent engagement. With 100 USD per completion and a 20 USD/day cost flow, its day-4 revenue and cost stocks are 500 and 80. A separate stock integrates revenue over time and ends at 875 USD·day, verifying that the off-grid completion reaches SD at 3.25. Staffing utilization remains `23/26`, matching the standalone reference. Tests compare all staffing observations, verify mixed output order and exact horizon behavior, and check parameter overrides, repeatability, unfinished work, idle delivery, and structured failure diagnostics. For these piecewise-constant flows refinement preserves the exact totals; arbitrary SD flows retain Euler discretization error.

Lint checks bridge endpoints, units, required sections, component/output IDs, supported expressions, and selectors. Capacity-phase runtime failures retain `IR_AGENT_POOL_PHASE` and their schedule pointer. Other coupled event failures report `IR_AGENT_POOL_SD_RUNTIME`; no partial result rows are returned from a failed run. Existing delivery-statistics failures retain their agent-pool diagnostic.

## Counter-based RNG contract (initial implementation)

The raw generator is Philox4×32-10 with the Random123 constants and 10 rounds. `seed` is an unsigned 64-bit key, split low word then high word. A draw address has bounded fields: scenario, replication, stream, step, and draw index each fit in 16 bits; entity ID fits in 48 bits. The 128-bit counter words are `c0 = entity low 32`, `c1 = entity high 16 | stream << 16`, `c2 = scenario | replication << 16`, and `c3 = step | draw_index << 16`. This layout is injective over its declared domain; out-of-range fields cause an error rather than truncation. A draw is a pure function of the seed and address, so execution order cannot change its raw words. The implemented transforms are open-interval uniform, Bernoulli, and inverse-CDF exponential. Stream-name assignment, broader distributions, and draws beyond 65,535 per entity per step are not yet implemented.

## Declarative IR 0.1 SD subset

The initial model file declares a time unit, fixed step and horizon, and optional standalone integrator; scalar parameters; stocks; directed flows; piecewise-linear tables; 1–255-stage Euler delays and fixed whole-tick delays with numeric or expression durations; and timeseries stock or delay outputs. It is parsed before a run. Component and parameter IDs are unique. Flow and delay-input expressions are pure arithmetic over finite literals, stocks, delay outputs, parameters, `t`, and `dt`, with `+ - * /`, unary signs, parentheses, and single-argument calls to declared tables. Operator precedence is conventional; evaluation follows the expression tree without reassociation. Undefined symbols or tables, malformed expressions, invalid endpoints, division by zero, nonfinite results, and time grids whose horizon is not an integer number of steps are errors. The loader emits stable diagnostic codes and JSON pointers. Units are mandatory; multiplication and division combine base-unit exponents, addition and subtraction require equal dimensions, table arguments must have the declared input unit, delay inputs must match delay output units, and every flow must have the connected stock's unit divided by the model time unit. This is a narrow unit grammar without conversion scales or arbitrary powers; units outside it are rejected.

At each IR tick, all delay outputs are read from their current stages. Stock flow rates and all delay inputs then evaluate against one shared pre-step context. Candidate stock and delay states are computed separately and committed together after both succeed. Material/information durations are reevaluated each tick; fixed-delay durations are evaluated only at initialization. The `component` output selector can expose a delay output, while the existing `stock` selector retains its stock-only meaning.

## Declarative IR 0.1 DES subset

`mode: "des"` selects an event-exact process model. It declares one source, one or more fixed-capacity stations, one completion sink, and optional binary priority/probability routers and discard sinks in a directed acyclic graph. Merging is supported; output-port fan-out, cycles, disconnected nodes, and duplicate routes are errors. Component/link declaration order does not determine graph execution order. See the [routing contract](DES_ROUTING.md) for ports, terminal losses, completion-path checks, and microstep ordering. A source has exactly one of `schedule` or `exponential`. A fixed schedule contains unique unsigned entity IDs, nondecreasing arrivals between time zero and the horizon, and positive base service durations. Each station may declare a positive `service_scale`; by default its own service time is base duration times that scale. Optional `service: {"kind":"exponential", "rate":..., "stream":...}` draws an independent station-local duration and applies the scale to that draw. The original entity duration remains unchanged. Station streams must be distinct from each other and from source/bridge streams; sampled-service entity IDs fit in 48 bits. See the [station service contract](DES_SERVICE.md). The exponential generator declares a finite entity count, positive arrival and service rates in reciprocal model-time units, a nonnegative start, a first entity ID, and the first of two adjacent Philox stream numbers. It generates interarrival and base service durations by inverse CDF; entity ID increments per generated entity. Every generated arrival and service draw uses the experiment seed, scenario ID, replication index, entity ID, step zero, the corresponding stream, and draw index zero. Generated arrivals after the horizon remain unsent. Without `--experiment`, the seed, scenario, and replication indices are all zero. No parameters or expressions are used by this subset. The same `time.dt` grid used by SD is an **observation grid only** for DES: events run at their exact timestamps, and each row at time `t` includes every event at `t`. At a station completion time, completions free slots before admission; existing accepted work reserves capacity, then dispatch follows the chosen discipline. See the [finite queue and priority contract](DES_QUEUES.md).

Source metric `emitted` counts entities released so far. Per-station metrics `accepted`, `rejected`, `completed`, `waiting`, and `busy` are instantaneous counts after all events at the sample time. `queue_mean` and `utilization` are station-local time-weighted means from time zero to the sample time, and both report zero at time zero. Sink metrics `completed`, `cycle_total`, and `cycle_mean` cover completed entities only and measure cycle time from the original source arrival; `cycle_mean` reports zero until the first completion. Service that extends beyond the horizon remains in process and is not counted as completed. Optional `queue_capacity` limits waiting places and `discipline` selects `fifo`, `lifo`, or `priority`; scheduled entities may declare an integer `priority`. Overflow without a rejection route is recorded as terminal loss. A rejection route may send the entity to backup service or an explicit discard sink. Router metrics are `received`, `matched`, and `otherwise`; discard sinks expose `discarded`. This subset does not represent cycles or resource-pool nodes. The binary probability rule is defined below.

## Declarative IR 0.1 hybrid subset

`mode: "hybrid"` nests an `sd` section and a `des` section under one time grid. The SD section currently supports parameters, stocks, and flows; the DES section retains a single linear source–station(s)–sink path; standalone graph routers, discards, and rejection routes are explicitly rejected in hybrid mode. All component and parameter IDs are unique across both sections. Exactly one `completion_to_stock` bridge maps each entity that completes the last station to a fixed positive amount in a target SD stock. The declared amount unit must match the target stock unit. The sink receives that completion too, so completion counts and stock pulses describe the same entities. Outputs can select SD stocks or the existing DES metrics.

An optional `stock_to_arrival` bridge closes a bounded feedback loop. It names an SD stock and the first DES station, a finite threshold in that stock's unit, a positive service duration in the model time unit, a first entity ID, and a positive maximum number of generated entities. At every observation time, including zero, the runner first drains all ordinary DES events and completion pulses through that time. It then samples the committed stock. If the stock is at or above the threshold and the bridge has generated fewer than its maximum, it injects one observation into the feedback atomic. That atomic emits one new entity at the same physical time in a zero-time microstep, and the runner drains it before recording outputs. A completion caused by the new entity cannot occur at the same time because service duration is positive. The feedback ID range must not overlap any fixed or exponential source ID. The threshold is a level trigger, so an unchanged stock above threshold generates one entity on each later tick until the maximum is reached; there is no hidden edge-detection state.

An alternative `stock_to_rate` bridge drives a bounded Poisson arrival source at the first DES station. At every observation time, after draining completions and pulses, it sets the rate to `base_rate + gain × committed_stock` in reciprocal model-time units. A negative or nonfinite computed rate is a runtime error. The source draws one unit-exponential hazard for each potential entity from its declared Philox stream; its service duration uses the adjacent stream and a declared positive service rate. Between tick updates it accumulates hazard at the current rate. A rate change preserves the unconsumed hazard, so a rate of zero pauses the next arrival without redrawing it. An arrival coincident with a rate update is emitted under the old rate before the update takes effect. IDs must be disjoint from the original source; both draw streams must be disjoint from the original source and every station service stream. The bridge supports at most one million generated entities and has no unbounded event generation.

The hybrid runtime runs the event kernel through each observation time, including all same-time microsteps, before sampling. A completion exactly on a grid tick first closes the preceding SD interval, then adds its pulse. A completion between ticks splits the Euler interval at its event time. A hybrid flow may use `t` but may not refer to `dt`, because a single nominal step can be split into unequal intervals. Scenario overrides reach SD parameters; exponential sources use the experiment seed, scenario, and replication addresses. The fixture in `models/hybrid_completion.ir.json` checks pulses at times 1, 2, and 2.5 against a direct C++ composition and a hand-derived stock trajectory. The stochastic fixture checks exact replay, distinct replication streams, and stock/completion conservation at every observation. The rate fixture checks accepted counts against independently accumulated exponential hazards when the rate changes at day 1. Other bridge kinds, tables, delays, and ABM populations are outside this IR subset.

## Declarative IR 0.1 ABM–SD subset

`mode: "abm_sd"` declares one homogeneous binary-adoption population, an SD stock-and-flow section, and one `adoption_to_stock` bridge. The population has a fixed positive count, a deterministic number of initially adopted agents with the lowest IDs, and two named SD parameters with reciprocal-time units: innovation and imitation. A synchronous ABM phase runs after the SD interval ending at each positive grid tick. An adopted agent remains adopted. Each unadopted agent sees the same fraction adopted in the pre-phase population snapshot and adopts independently with probability `(innovation + imitation × prior_fraction) × dt`. The default parameter values must be nonnegative and their sum times `dt` at most one; scenario overrides are checked before a run. Draws use Philox address `(scenario, replication, agent ID, step index, stream, 0)` and the experiment seed. The horizon is limited to 65,535 steps by this address layout.

At time zero, no adoption phase runs. The aggregate bridge publishes the initial adopted count to its target SD stock. At each later tick, SD flows first integrate using the previous committed stock state; then the ABM phase commits and the bridge publishes the change in adopted count through a zero-time pulse. Outputs at the tick see the post-bridge stock and population state. The target stock must start at zero and may not be the endpoint of an SD flow, though other flows may read it. This subset has fixed population size and no spawn, despawn, network, heterogeneous roles, or general ABM rules; those remain C++ primitives or later IR kinds.

The loader checks known fields, types, graph topology, time bounds, identifiers, and component-specific rules before execution. CI also validates every accepted fixture and structural mutations against the Draft 2020-12 schema with an independent validator, then compares acceptance to `fathom lint`. Schema-valid semantic mutations must fail loader checks; current counts are recorded in [status](STATUS.md). The schema is not yet executed inside the C++ CLI; topology, dimensional, and expression rules remain loader checks.

The stochastic DES oracle uses independent Philox-addressed runs of M/M/1 with arrival rate 0.5 and service rate 1, and M/M/2 with arrival rate 1.2 and per-server service rate 1. The closed-form stationary targets are M/M/1 utilization 0.5, mean waiting queue length 0.5, mean wait 1; M/M/2 Erlang-C utilization 0.6, mean waiting queue length 0.675, mean wait 0.5625. The tests use finite runs started empty, so their predeclared absolute gates allow transient bias and autocorrelation. Exact event order, conservation, and stage-local area accounting are checked separately on deterministic schedules.

## Binary probabilistic routing

A standalone DES router may specify exactly one selection rule: `priority_at_most`,
or `probability: {"match": p, "stream": s}`. Probability `p` is finite in `[0,1]`;
the unsigned 16-bit stream is distinct from every source, station-service, and
other probabilistic-router stream in the model. The source's reserved service
stream remains reserved even when every station overrides service duration.
Hybrid graphs remain outside this subset.

Each admitted entity chooses `match` iff the open uniform from Philox word zero
is strictly less than `p`. Its address is `(seed, scenario, replication, entity ID,
step=0, stream=s, draw_index=0)`. Entity IDs fit in 48 bits; scenario and replication
fit in 16 bits. A router validates the complete transition on a candidate copy.
Invalid probabilities, addresses, metadata, or repeated IDs are errors. Even a
probability of zero or one validates the entity address. Repeated visits are not
defined here; routing never consumes a mutable RNG cursor.

Selection occurs on input and buffers exactly one output per input, preserving
entity metadata. Publication occurs at the same physical time in the next DEVS
microstep. Repeated `output()` calls do not draw again. Confluence publishes the
old buffer, clears it, and accepts the new bag. Cloning and checked simulator
rollback preserve pending choices. Different input-bag orders may change FIFO
publication order but cannot change an entity's chosen branch. Both routes are
required and structurally validated, including at probabilities zero and one.
The existing `received`, `matched`, and `otherwise` counters keep their meanings.

## Experiment subset

An experiment JSON file contains an unsigned 64-bit seed, a replication count from 1 to 65,536, and scenarios with strictly increasing 16-bit IDs and finite overrides for declared model parameters. The runner visits scenarios in file order and replications in ascending order, then emits each trajectory's observations. A trajectory cannot contain duplicate `(time, output_id)` pairs or nonfinite values. These indices match the Philox draw-address fields. DES exponential sources, independent station service, and ABM adoption consume seed, scenario, and replication addresses. Deterministic SD and DES with fixed schedules and only source-duration service remain identical across replications. The coordinator runs sequentially and accumulates sample means and unbiased sample variances in a fixed order. The CLI writes long CSV rows `(scenario, replication, time, output_id, value)` when `--experiment` is supplied. Manifests, data hashes, replay CLI, cross-platform numeric policy, and parallel execution are future gates.

## SD test inputs and smoothing

Standalone SD expressions support uppercase `STEP(height,start)`,
`PULSE(start,width)`, and `RAMP(slope,start,end)`. These names are reserved IDs;
lookup tables remain single-argument calls and may be nested with these inputs.
Unary negation preserves the operand's dimension.

The test-input dialect follows [Vensim STEP](https://www.vensim.com/documentation/fn_step.html),
[PULSE](https://www.vensim.com/documentation/fn_pulse.html), and
[RAMP](https://www.vensim.com/documentation/fn_ramp.html):

- STEP returns its height exactly when `t + dt/2 > start`.
- PULSE returns dimensionless 1 exactly when `start < t + dt/2 < start + width`.
  Width zero means one dt. This is unit height, not unit area or an instantaneous
  stock increment. A pulse narrower than dt can be missed; exact half-step
  endpoint ties are excluded.
- RAMP uses tick time `t`, returning zero through start, then
  `slope*(min(t,end)-start)`. The supported contract requires `end >= start`.

Arguments must be finite; dt must be positive and the midpoint representable.
Negative pulse widths, unrepresentable pulse endpoints, and result overflow
fail. Time arguments require model-time dimensions; pure numeric constant
expressions in these positions are interpreted in model-time units. Named
dimensionless parameters cannot substitute for dimensional time parameters.
STEP preserves height units and RAMP multiplies slope units by time.

The standalone declarative runner is Euler and evaluates these inputs from
one shared pre-tick state. All three functions carry implicit `t` and `dt`
dependencies, so loaders reject them in hybrid flows where off-grid events
split steps. The C++ helpers in `sd/test_inputs.hpp` require the caller to pass
the tick origin and hold the value over any midpoint/RK4 stages; they do not
infer or maintain a clock. RK discontinuity alignment is not claimed.

`sd::EulerSmooth(order, initial_input, duration, optional_initial_value)` owns
1–255 information-delay stages. With no explicit initial value, every
stage starts at `initial_input` (SMOOTH/SMOOTH3); an explicit value selects
SMOOTHI/SMOOTH3I initialization. The existing bounded Euler update uses shared
old stage values and requires `dt <= duration/order`. A failed step preserves
all stages and the stored duration. Signed values are allowed. This is a C++
stateful helper; these names are not hidden-state expression functions. In IR,
use `kind: "delay", type: "information"` with an explicit `initial`, as in
`models/sd_functions.ir.json`. Variable-duration equivalence to other tools
remains unproven.

The hand-derived fixture combines a step-driven smooth with pulse and ramp
flows. Additional tests check exact/neighboring half-step boundaries, units,
arity, table composition, rejection diagnostics, first-/third-order smoothing
recurrences, analytic step-response convergence, and failed-step isolation.
The [XMILE importer](XMILE_IMPORT.md) maps graphical functions and SMTH1/SMTH3/DELAY1/DELAY3 with constant or variable durations to these explicit components. Source input dialects use separate `XMILE_*` names, described below.

### Grid-aligned XMILE inputs

`XMILE_STEP(height, first)` includes the first tick. `XMILE_RAMP(slope, first)`
has no end argument and returns slope times elapsed time after that tick.
`XMILE_PULSE(quantity, first, interval)` emits quantity/dt for one tick; zero
interval means one event, positive interval repeats. Native calls require all
three pulse arguments; the importer supplies zero when the source omits interval.
Pulse result units are quantity/time; step preserves units and ramp multiplies
by time. See [native example](../models/xmile_inputs.ir.json).

These pure functions require grid-aligned times, finite arguments and a
representable clock. Helpers bound relative offsets to ±2,000,000 ticks;
the importer applies the stricter schedule bounds in the [import contract](XMILE_IMPORT.md#source-input-functions).
Roundoff allowance includes absolute clock scale but cannot exceed 1e-6 tick.
Off-grid calls fail; no alternate half-step selection rule is inferred.
All three inject `t` and `dt` dependencies. RK4 flows and hybrid flows reject
them; standalone expression outputs can observe them at committed grid ticks.
Source imports conservatively reject every input-function call under RK4.

### Explicit next-tick XMILE inputs

Opt-in `--input-policy next_tick` lowers to `XMILE_NEXT_STEP(height,first,origin)`,
`XMILE_NEXT_RAMP(slope,first,origin)` and
`XMILE_NEXT_PULSE(quantity,first,interval,origin)`. Origin defines the sampling
grid; importer-generated expressions use the source start. STEP samples the
threshold and RAMP retains fractional elapsed time. PULSE counts all scheduled
events in the tick window open on the left and closed on the right, emitting
their combined quantity/dt. Multiple events per tick are supported.

Integer tick boundaries relative to origin make adjacent counts telescope.
The same rounding rule applies to each shared boundary. Pure observation,
unit checking and Euler snapshot semantics apply; RK4/hybrid flows reject these
functions. Final observations do not advance stocks. This is an explicit policy
with bounded clock precision and schedule counts, not implicit vendor emulation.
See [complete contract and rational oracle](OFFGRID_INPUTS.md).

## Signed SD flows

`sd::Model::add_flow(source, destination, rate, nonnegative)` accepts an optional
sign policy, defaulting to `true`. Native IR flows expose the same policy as
`non_negative`, also defaulting to true. With false, finite negative rates
subtract from the destination and add to the source. Endpoint names retain
their declared orientation; there is no separate reverse event or clipping.
Two internal endpoints still receive equal and opposite derivatives. This
policy applies consistently in standalone SD, hybrid DES/SD, ABM/SD, and
staffed-delivery SD runners.

Flow sign policy does not change stock bounds: a resulting forbidden negative
stock rejects the whole step. Nonfinite rates remain errors at every integration
stage, and failure leaves stock state unchanged. `models/signed_flow.ir.json`
checks a transfer that reverses direction while conserving total stock. The
XMILE importer explicitly selects signed flows for source flows without
clipping directives.

## Declarative variable-duration delays

Standalone SD `delay.duration` accepts either a positive number in model time
units or a pure expression whose inferred units equal the model time unit.
An expression can read parameters, stocks, `t`, `dt`, and declared lookup tables.
It cannot read any delay output. This restriction prevents an implicit algebraic
cycle when a material delay's output itself depends on duration. Hybrid modes
still exclude delay components.

Lint checks the expression, units, and duration at the declared start using default
parameters and declared stock initials. Before each run, scenario overrides
are applied and initial material pipelines are constructed with the resulting
initial durations. Each material stage initially stores
`initial_output * initial_duration / order`; information stages store the
initial signal. There is no pipeline rescaling when duration later changes.

For material/information delays, at each tick the runner evaluates every duration from the same committed stock
snapshot and time, validates it, then computes all delay outputs using those
durations. Material output is last-stage quantity divided by the current stage
duration, so shortening duration can increase the reported output immediately.
Information output is the last stage value, independent of current duration;
only its adjustment rate changes. Flows and all stage updates read this shared
pre-step snapshot. Stocks and delay stages commit together after successful
Euler updates. Source/component order does not affect results.

Every duration must be finite, positive, and satisfy `dt <= duration/order` at
every observation, including the horizon. No order reduction or duration clamp
occurs. Duration evaluation or bound errors report `IR_DELAY_RUNTIME` at the
component's `/duration`; override-induced initial pipeline errors point to
`/initial`. Input or stage-update errors retain `/input`. Failed CLI runs publish
no partial trajectory. Subsequent runs reinitialize from the model specification.

The hand fixture `models/variable_delay.ir.json` doubles duration at t=2. Material
output changes from 10 to 5 immediately, while information output stays 10.
The tests also cover third-order stage lag, stock-dependent durations, overrides,
late/horizon failures, and algebraic-dependency rejection. Variable-duration
SMTH1/SMTH3 and DELAY1/DELAY3 agree with pinned PySD on the documented Euler
fixtures. General equivalence to every vendor, adaptive solvers, arbitrary
orders beyond 255, signed material delays, and duration/output algebraic loops is not claimed.

## Fixed transport delays and general-order source functions

Native `kind: "delay", type: "fixed", order: 1` buffers sampled input values.
The duration is evaluated once from the initial snapshot after parameter overrides.
It must be 1–1,000,000 whole ticks; grid alignment allows only floating-point
roundoff (`8 * epsilon * nearest_tick_count`). Fractional lags are rejected,
not rounded or interpolated. Later changes to duration dependencies have no effect
and the expression is not reevaluated. `FixedDelay` C++ calls reject changes to
the initialized duration or dt. Inputs and initial output may be signed and must
be finite. This is a signal transport buffer, not a material-pipeline API.

For lag N ticks, `y[k] = initial` for k < N and `y[k] = input[k-N]` otherwise.
All fixed buffers, Euler stages, and stocks read the same committed snapshot.
Two cascaded fixed lags of N and M ticks therefore have lag N+M; source ordering
cannot shorten that lag. Failed steps preserve buffered history. The native
example is `models/extended_delay.ir.json`.

XMILE `SMTHN(input, duration, order[, initial])` maps to an information cascade;
`DELAYN` defaults to provably constant duration, with explicit
`--delayn-policy cascade` enabling current-duration material stages. Both
require constant integer order in 1–255 (constant auxiliary expressions allowed).
The cascade policy conserves stored quantities, uses current duration for all
stage rates, and has an independent explicit-stock PySD oracle. PySD's unexpanded
DELAYN duration-history semantics differ and are not claimed by this policy.
The explicit `history2` policy instead maps DELAYN of constant order 2 to
`sd::HistoryDelay2`, or IR `type: history2, order: 2`. Stage 0 uses current
duration; stage 1/output uses saved previous duration. Signed stage contents
and boundary outputs conserve quantity. Updates and saved duration commit
together; observations are pure, dt fixed, and duration must cover two dt.
Other history orders are rejected pending independent evidence. See the
[recurrence, source comparisons and failed-order diagnostics](DELAY_HISTORY.md).
`DELAY(input, duration[, initial])` freezes the initial
lag and requires the same whole-tick contract. No silent truncation, duration
stretching, or order reduction is performed. Source behavior is described by
[Stella's delay documentation](https://iseesystems.org/resources/help/v3/Content/08-Reference/07-Builtins/Delay_builtins.htm).

## Pure expression outputs

Standalone SD outputs may select exactly one of `stock`, `component`, or
`expr`. Expression outputs require a declared `unit` matching the inferred
expression dimension. They read parameters, stocks, committed delay outputs,
`t`, `dt`, and tables; output IDs do not become expression symbols. They do not
feed state transitions or acquire evaluation-order dependencies. Existing
stock/component selectors do not accept an extra unit override. Hybrid modes
retain their existing output contracts.

Standalone models may have an empty component list. They still require at
least one valid output and the normal fixed time grid. Expressions are observed
at every tick including the horizon, after current delay outputs are assembled
and before state updates. A runtime evaluation failure reports
`IR_OUTPUT_RUNTIME` at `/outputs/N/expr`; failed CLI runs publish no partial
trajectory. `models/expression_outputs.ir.json` illustrates units and
scenario-dependent observations without stocks.

## Standalone integrator selection

Native standalone SD accepts top-level `integrator: "euler" | "rk4"`; omission
retains Euler. Other modes reject this field. RK4 selects the existing classical
four-stage C++ integrator: derivatives use snapshots at t, t+dt/2, t+dt/2, and
t+dt with the corresponding trial stock values. Every flow in a stage reads
the same stage state. The expression symbol `dt` remains the full nominal step.
Tables are queried using the stage arguments. Outputs observe committed grid
values, never intermediate trials, including expression outputs at the horizon.
Only a successful full step commits; intermediate evaluation failure publishes
no partial CLI trajectory.

RK4 currently rejects all delay components at `/components/N/kind`, and flows
using STEP/PULSE/RAMP at `/components/N/expr`, with `IR_INTEGRATOR`. Those
functions have a separately tested Euler tick convention. Output-only functions
remain ordinary grid observations. RK4 does not add adaptive stepping or event/
table-knot localization; fourth-order accuracy is established for smooth
trajectories, not across arbitrary discontinuities. Native midpoint remains a
C++ API capability and is not yet a declarative/source mapping.

`models/rk4_growth.ir.json` demonstrates `y'=t*y` with strict units and an
expression output for the derivative. The importer also recognizes legacy
stock edges that name auxiliary rate equations; only explicitly stock-linked
auxiliaries acquire a native flow, with one source/destination and the existing
signed-rate contract. `promoted_aux_flows` in the metadata records those IDs.

## Bounded Euler clipping

Standalone SD exposes two opt-in controls. A flow with `clip_negative: true`
uses `max(requested_rate, 0)` after checking finiteness. A stock with
`clip_outflows: true` requires an explicit `outflow_order` listing each outgoing
flow exactly once. Both require `non_negative: true` (the native default).
Without the clipping controls, `non_negative` retains its rejection semantics.
See [the runnable fixture](../models/clipping.ir.json).

At each Euler tick:

1. Evaluate all requested rates from the same old stock/delay state and time.
2. Multiply by dt to obtain requested transfer amounts, after sign filtering.
3. Visit clipped stocks in upstream-to-downstream order. Available material is
   old stock plus the already-constrained amounts arriving in this interval.
4. Allocate `min(requested_amount, remaining_available)` to each outflow in
   explicit priority order. Keep the remainder as the stock's new value.
5. Apply each shared amount to both endpoints and validate all candidate state
   before committing. Never repair a negative stock after integration.

For example, a stock holding 2 with inflow 3/day, dt=0.5 day, and outflows
6/day then 8/day has 3.5 available. Its receivers get 3 and 0.5; reversing
priority gives 0 and 3.5. Their total equals initial material plus boundary
inflow. Floating-point conservation is tested with appropriate tolerances.

The allocation follows [Stella's documented current-inflow and priority rules](https://www.iseesystems.com/resources/help/v2-1/Content/08-Reference/05-Computational_Details/Flow_prioritization.htm).
The [XMILE specification](https://docs.oasis-open.org/xmile/xmile/v1.0/xmile-v1.0.html)
identifies stock outflow tag order as priority. XMILE supports optional filter
implementations; this contract does not claim universal filter equivalence.

All flows incident to a clipped stock must be nonnegative. Direct flow cycles
between clipped stocks, including self-loops and zero-rate cycles, are rejected
before evaluation. Native nonnegative flows may reject negative requested
rates or opt into filtering; the importer requires explicit source directives.
Unrelated signed flows retain their declared meaning. Negative initial clipped
stocks, stale/missing/duplicate priority entries, nonfinite arithmetic, RK4,
midpoint, and dt-free derivatives with clipping are rejected. Clipping controls
are currently standalone SD only. The importer rejects equations reading flows limited by source stocks.
Sign-filtered flows without source-stock limits have the pure reference semantics below.

Stock state is unchanged on a failed step; user callback side effects are not
rolled back. C++ tests cover conservation, competing priorities, same-tick
cascades, old-state evaluation, roundoff, sign policies, invalid graphs and
failure isolation. Native/XMILE tests cover source order, overrides, diagnostics,
and full trajectories. The pinned diagram teacup matches both historical exports,
but its limits never bind; active clipping evidence comes from the hand-derived
allocation tests, not that historical trajectory.

## Absolute standalone SD clocks and pure flow filtering

Native standalone SD accepts optional `time.start` (default 0). `time.horizon`
is elapsed duration; observations occur at `start + index * dt`, including both
endpoints. Expression `t`, RK4 stage clocks, and initial delay-duration evaluation
use absolute time. The source importer also evaluates stock and default delay
initializers at source start. Fixed lags count intervals from initialization,
not from calendar zero. See [the native fixture](../models/nonzero_start.ir.json).

Start may be any finite positive or negative number whose full tick grid is
finite and strictly increasing; RK4 half stages must also be distinguishable.
Invalid grids reject at lint. Other modes reject a `start` field, even zero,
until their event schedules and hybrid initialization support absolute origins.

`NONNEGATIVE(x)` is a pure unary native expression: `max(x, 0)` for finite x,
with the same dimension as x and no implicit t/dt symbols. Nonfinite arguments
and invalid arithmetic remain errors before filtering. It is allowed in pure
outputs and RK4 expressions, unlike tick-based STEP/PULSE/RAMP semantics.
It adds no solver guarantee for trajectories crossing the filter's nonsmooth
point; such crossings still need appropriate convergence checks.

The importer uses this primitive only for references to explicitly nonnegative
flows without a clipped source stock. Their applied rate equals the filtered
requested rate, so delay inputs and default initial values can use the pure
expression. Availability-limited outflows require a separate dependency scheme
and cannot be referenced or exported via the new `--outputs all` option.

### Scheduled typed population lifecycle

Port 3 accepts timestamped lifecycle inputs with unique per-timestamp sequences,
retirements, and full typed birth records. All lifecycle inputs in one timestamped input bag form one
transaction: already-due asynchronous timers run first, then all retirements,
then births ordered by input sequence and record position, then direct messages,
topics, same-time timer closure, and synchronous phases. Newborns participate in
that timestamp's tick and messages. Retiring an agent cancels its future timers;
IDs increase monotonically and are never reused. Duplicate retirements, invalid
records, occupied spatial cells, or retired endpoints in queued publications
reject the entire transition, including timer work and ID allocation.

An optional pure native birth initializer sees the post-retirement, raw-birth
snapshot and returns the newborn record and absolute wakeups. Declarative async
births enter their chart's initial state at birth time with generation zero and
arm timers using the newborn ID and run RNG context. Birth input generations
must be -1. Initialization does not emit publications.

Declarative `lifecycle` entries require `time`, `sequence`, `retire`, and `births`.
IDs follow chronological sequence order regardless of declaration order. A
retirement must refer to an initial agent or an agent live from an earlier birth timestamp; a newborn
cannot be retired at its own birth timestamp. Scheduled message endpoints must
be live after the lifecycle batch. Network membership follows live agents; newborns initially
have no edges and scheduled edge edits may connect them. Spatial and population queries use current live membership.
Agent output metric `alive` returns 0/1; field/query outputs require live agents
unless an explicit finite `inactive_value` is provided. Aggregates exclude dead
agents. Behavior-triggered phase/transition lifecycle is specified below; network editing is specified below.

### Behavior-triggered typed population lifecycle

Synchronous phases and selected statechart transitions may carry a `lifecycle`
object with optional dimensionless `retire` expression and `births` array. Each
birth has a full typed literal `record`, optional dimensionless `guard`, and
optional unit-checked `assign` expressions evaluated against the parent's
pre-action snapshot. A false birth guard skips its assignments. Lifecycle
expressions cannot mutate the snapshot. A true retirement suppresses the parent's
ordinary assignments, chart entry and publications, but permits replacement
births. Previously queued publications still require live endpoints; retirement
of one of those endpoints rejects the transaction.

A sync phase collects decisions from agents live at phase entry, in stable ID
order, then applies surviving updates, retirements and births as one spatially
validated batch. Birth IDs follow parent ID then birth-list order. Newborns do
not execute their birth phase; they participate in subsequent phases. Any later
phase failure restores the complete tick, including IDs and queued publications.
Async decisions run only for the selected, enabled, non-stale transition. Newborns
enter the configured initial chart state at the event timestamp with generation
zero and their own addressed timers. Parent retirement cancels pending timers and
does not enter the transition's target. Immediate newborn events join the same
timestamp closure and event budget. Initial chart entry has no lifecycle actions.

`agent_limit` is an optional positive integer, at most one million, default one
million, bounding total IDs allocated over a declarative run (including retired
agents). Exceeding it rejects the transaction. Scheduled births cannot coexist
with behavior birth declarations, because their predicted IDs would depend on
behavior. Scheduled retirements and input references to initial agents remain
supported, with runtime liveness validation after behavior changes. Dynamically
born agents are observed through aggregates/queries and addressed by self or
broadcast publications; the IR does not allow forward numeric references to
behavior-assigned IDs. Networks retain live-membership and scheduled-edit semantics.
Topic-handler lifecycle actions are not included in this increment.

### Population-owned mutable networks

Typed population snapshots own optional CSR network state alongside typed records.
When configured, vertices equal the live agent IDs: birth adds isolated vertices,
retirement removes the vertex and every incident edge, and IDs are never reused.
Record and graph copies are independent. Field updates preserve topology. Spatial
and network queries read the same staged population snapshot; graph state is never
held in a mutable external callback capture.

Population DEVS port 4 accepts timestamped edge edits with unique per-bag sequences
and namespaced live endpoint references. At a timestamp, already-due timers run
first, then scheduled lifecycle batches, then all edge edits as one batch, then
ordinary commands/topics and existing timer/tick closure. Same-bag edits sort by
sequence and combine removals/additions: duplicates, existing additions, missing
removals, self-loops and add/remove overlap reject. Undirected reversed pairs are
the same edge; directed edges retain orientation. Endpoints must be live after
scheduled lifecycle. Edges already removed by retirement cannot be explicitly
removed again. Newborns can be connected in their birth timestamp. Later same-time
microsteps are separate transactions. Graph failure restores earlier timer and
lifecycle work in the same transition. Result snapshots include committed topology,
and checked downstream publication retries do not repeat edits.

The IR accepts population `network_updates` entries with required `time`, `sequence`,
`add`, and `remove`. A network declaration is required. Endpoints may reference
initial or scheduled-born IDs; scheduled liveness is checked at lint, while actual
liveness and edge existence are checked at execution (behavior can change them).
Empty edit arrays are legal. Network queries see current live topology in phases,
chart guards/actions, lifecycle expressions, topic handlers and observations.
Behavior-born agents are isolated unless explicitly connected by a native input;
forward numeric references to their runtime-assigned IDs remain unsupported in IR.
Behavior-generated edge edits remain a later increment; declarative graph initialization is specified below.

### Declarative random graph initialization

A typed population's `network` declares either explicit `edges` (optional `directed`)
or an undirected `generator` object. Generators are `erdos_renyi` with `probability`,
`watts_strogatz` with `degree` and `probability`, or `barabasi_albert` with `m`.
Every generator requires an explicit 16-bit `stream`; numeric parameters are
nonempty, dimensionless parameter expressions evaluated once at run initialization.
They cannot read agent fields, queries, time or message context. Probability lies
in [0,1]; WS requires positive population size and even integer 0 <= degree < n;
BA requires integer 1 <= m < n. ER permits empty populations. Default parameter
validity is checked at lint without generating a graph; experiment overrides are
revalidated before simulation. Extra/mixed fields and directed generator requests
are rejected. Generator and statechart rate streams must be disjoint, even when
probability/rates are zero.

Generation uses sorted initial agent IDs and the run seed/scenario/replication plus
its explicit stream. Existing native generator draw traversal and unbiased integer
selection are unchanged. It happens before initial chart entry, time-zero lifecycle
or edge edits, and observations. Later births are isolated and retirements remove
incident edges; no regeneration occurs. Existing scheduled edge edits can change
the generated graph but must satisfy runtime edge-existence checks. Reordering
outputs/queries or changing observation density cannot consume extra graph draws.

### Boltzmann wealth reference model

The native `abm::models::WealthExchange` workload uses typed integer records and
transactional population updates. Initial wealth is nonnegative and its total must
fit signed 64-bit arithmetic. Membership is fixed. Each sweep visits every agent
once in a uniformly shuffled order. At its activation an agent with positive current
wealth transfers exactly one unit to a uniformly chosen recipient. With no network,
all agents including itself are eligible; with a network, only outgoing neighbors
are eligible. An isolated or penniless donor does nothing. A self-transfer changes
nothing. Transfers commit sequentially within a staged sweep, so a recipient may
spend received wealth later in the same sweep. The complete sweep commits atomically;
failure preserves records and the sweep counter.

Fisher–Yates shuffling starts from sorted stable IDs, descends from n−1 to 1, and
uses an explicit order stream with entity equal to the stable ID at that original
array index. Recipient draws use the donor ID and a distinct recipient stream.
Both address the current zero-based sweep in the 16-bit step field and use unbiased
integer rejection sampling, with rejection retries in the draw-index field. Seed,
scenario and replication are explicit. Address overflow is rejected. Observation
does not consume draws. A scripted sweep accepts a permutation of all IDs and one
eligible recipient per activation (ignored for penniless/isolated donors); it uses
the same transfer/commit path and advances the sweep counter without random draws.

This is a sequential random-activation reference model, not synchronous Jacobi
wealth exchange or a continuous-time process. Its native API is separate from the
typed ABM IR; no declarative shuffled activation is implied.

### Schelling segregation reference model

The native `abm::models::Schelling` workload stores fixed binary-group agents and
integer x/y coordinates in a typed population. Initial positions must be canonical
in-bounds cells even on a periodic grid. Grid area is bounded to one million cells.
Single occupancy, stable membership and each agent's group are invariants. Radius-one
Moore or von Neumann neighborhoods exclude self and deduplicate periodic cells.
An agent is satisfied when `same * denominator >= neighbors * numerator`, with
0 <= numerator <= denominator <= 1,000,000 and denominator positive. An agent with
no neighbors is satisfied. This exact rational comparison includes the boundary.

Each sweep shuffles all stable IDs, then evaluates satisfaction sequentially against
the current staged population. A dissatisfied agent moves once to a uniformly chosen
currently vacant cell, without testing whether the destination is satisfactory.
Its current cell is excluded from that choice and becomes available to subsequent
agents after the move. On a full grid it stays in place. Vacancies are enumerated
in row-major order (y then x). A satisfied or blocked agent consumes no relocation
draw. The entire sweep commits records, last-sweep move count and sweep counter
atomically. Invalid late choices preserve the original state and counters.

Addressed Fisher–Yates order draws use the stable ID at each original array index;
relocation draws use the moving agent's ID. Distinct explicit streams address the
same zero-based 16-bit sweep, with seed/scenario/replication and unbiased integer
rejection retries in the draw-index field. Exhaustion fails rather than wraps.
A scripted sweep supplies an ID permutation and a destination per activation,
ignored when the agent is satisfied or blocked. Observations consume no draws.
This native reference API does not add shuffled activation to typed ABM IR or
establish synchronous/continuous-time Schelling equivalence.

### Boids flocking reference model

The native `abm::models::Boids` workload uses synchronous typed-population phases
in a two-dimensional box. Every agent stores real x/y/vx/vy. One tick reads a common
snapshot, excluding self from radius-inclusive vision neighbors but retaining
colocated peers. Neighbor accumulation follows stable ID order. Periodic displacement
uses the minimum image; an exact half-box displacement retains its original sign.

Alignment is mean neighbor velocity minus own velocity. Cohesion is mean neighbor
displacement. Separation is the mean `-displacement/(distance²+softening²)` over
neighbors within the inclusive separation radius. Colocated peers contribute zero
separation; no neighbors means zero for the corresponding steering term. Positive
softening regularizes separation. The weighted sum is capped by maximum acceleration;
then velocity becomes `cap(v + dt*a, max_speed)` and position advances by `dt*v_new`
(semi-implicit Euler). These are explicit model rules, not a universal Boids variant.

Periodic positions wrap into [0,L). Reflecting positions fold modulo 2L, reversing
the corresponding velocity component after an odd number of wall crossings. At an
exact lower/upper wall, velocity points inward; the upper wall is stored as the next
representable value below L to preserve the spatial index's half-open domain. Both
axes are handled independently, including multiple crossings in one tick.

Membership, finite state, canonical coordinates and speed caps are validated before
commit. Invalid intermediate arithmetic fails the whole tick without changing records
or the tick counter. Width, height, dt, max speed, softening and bin width are finite
positive values; vision and steering weights/max acceleration are finite nonnegative
values, and 0 <= separation radius <= vision. The spatial index imposes its existing
bin-count bounds, and twice each box extent must remain finite. Population allocation
is bounded to one million agents. Initial coordinates must already be canonical.
Zero velocity and empty populations are permitted; there is no minimum-speed or random-heading rule.

The optional seeded initializer uses separate 16-bit position/velocity streams,
explicit seed/scenario/replication and stable agent IDs. Draw indices 0/1 select the
two coordinates at step zero. Positions use open uniforms scaled by the box; velocity
components are independently uniform in (-max_speed/2,max_speed/2). Initialization
rounding at the upper position boundary is clamped one representable value inward.
Evolution consumes no random draws. This is a native workload API, not new typed-IR
force or boundary syntax. Its finite-time comparisons do not establish long-time
trajectory equivalence for chaotic configurations.

### Sugarscape-lite reference model

The native `abm::models::Sugarscape` model activates a uniformly shuffled permutation
of current live stable IDs once per sweep. Each agent selects from its own cell and
unoccupied axial-vision cells (cardinal rays, without line-of-sight blocking), maximizing
land sugar, minimizing shortest axial distance, and then drawing uniformly from
row-major ties. Periodic cells are deduplicated. It moves, harvests the whole cell,
consumes `min(reserve, metabolism)`, and retires immediately at zero reserve. Later
agents read the updated population and land. After all activations, each cell regrows
by `min(regrowth, capacity - sugar)`, including occupied cells and empty populations.

Initial reserves and metabolism must be positive, and vision is nonnegative and
bounded. Metabolism and vision remain immutable; reserves and positions evolve. No births, trade, negative reserves or ID reuse occur.
The landscape and complete population/ledger/clock stage together and roll back on
any failure. Exact balances are initial resources + cumulative regrowth = current
land + live reserves + cumulative consumption, and initial population = live agents
+ cumulative deaths. Both resource totals and cumulative counters must fit signed
64-bit. Initial coordinates are canonical even in periodic space; area is limited
to one million cells.

Order and movement use distinct Philox streams with scenario/replication/sweep fields;
Fisher–Yates rank addresses are `first_id + remaining - 1`, and movement addresses
are stable agent IDs. Each choice uses rejection sampling starting at draw index
zero. The stochastic and scripted APIs both reject sweeps beyond 65,536. Scripted
activation must be a live-ID permutation and each destination must satisfy both
resource and distance priorities. Read-only queries consume no randomness. See
[Sugarscape contract and evidence](ABM_SUGARSCAPE.md); this native reference does not
extend declarative ABM scheduling or claim equivalence to other Sugarscape variants.

### Synchronous network SIR reference model

The native `abm::models::SIR` workload runs one typed population phase per tick on a
fixed simple undirected CSR graph whose vertices equal all live stable IDs. Integer
states are susceptible=0, infected=1, recovered=2. A susceptible agent with k infected
pre-tick neighbors transitions with probability `-expm1(-(beta * dt) * k)`; an infected
agent recovers with probability `-expm1(-gamma * dt)`. Beta is per edge, without degree
normalization. Decisions use strictly `u < p` with independent addressed streams.
Recovered agents are absorbing. Membership is fixed and conserved exactly.

All rules read the same pre-tick snapshot, so recovering agents still contribute
infection pressure and newly infected agents do not transmit or recover in the same
tick. Maximum one transition per agent per tick is validated. The full phase and
clock commit together; invalid scripted variates or failed validation restore all
state. Fresh phases capture values, preserving copy independence.

Rates must be finite/nonnegative and dt finite/positive; maximum-degree integrated
hazards must fit finite double. Time is integer tick count times dt and must advance
finitely. There are 65,536 addressable updates using separate infection/recovery
streams and draw index zero. The scripted API accepts one finite [0,1) variate per
agent in stable-ID order. Observations consume no draws. This frozen-neighbor process
is a synchronous discretization, with continuous-time and mean-field convergence
tracked separately. See [SIR contract/evidence](ABM_SIR.md).

### Continuous-time network SIR reference model

`abm::models::AsyncSIR` runs the fixed undirected contact process through
`AsyncPopulation`. Susceptible agents have hazard beta × current infected-neighbor
count; infected agents have hazard gamma; recovered agents have zero hazard. One
direct Gillespie race sums rates in stable-ID order, draws an exponential wait and
selects one agent proportionally to its hazard. Following that single S→I or I→R
transition, all hazards are recomputed and one successor is scheduled. Zero total
hazard leaves no pending event. The graph, IDs and population count stay fixed.

Event generations 0..65535 use independent waiting/selection streams, population
first ID as entity address, and draw index zero. All uniforms are open (0,1); weighted
selection is strict at cumulative boundaries and clamps a rounded-to-total target
to the preceding representable double. Rates, total hazards and deadlines must be
finite and deadlines strictly advance. The selected event plus successor schedule
are one transaction; `run_until` additionally commits the entire requested horizon
atomically. It includes events at the horizon and preserves the pending event when
only observation time advances. Copies own callback/calendar data.

This reference model's rate recomputation does not extend the general statechart
rate-expression contract. The synchronous model still freezes neighbors for each
tick. [Independent event and finite-state distribution evidence](ABM_SIR_ASYNC.md)
validates their shared continuous-time limit on the declared small graphs, separately
from the M5 well-mixed ABM/SD convergence requirement.

### Well-mixed SIR and the SD mean-field limit

`AsyncSIR::well_mixed` selects implicit complete mixing instead of a CSR graph.
Each susceptible has hazard `(beta/N)*I`, where N is the fixed total population,
including recovered agents, and I is the current infected count. Infected hazard
is gamma and recovered hazard is zero. The graph constructor's per-edge beta and
the factory's mass-action beta are distinct: complete-graph equivalence uses
per-edge `beta/N`. Empty populations have zero hazard.

The factory retains individual typed records, stable-ID event selection and all
event/horizon transaction and draw-address rules above. No network is stored, and
the shared susceptible hazard is computed once per race. Population counts are
derived from individual records, never advanced by a separate count-only kernel.
There are no new declarative fields or general changing-rate statechart semantics.

For fractions, the matching SD flows are `beta*s*i` from susceptible to infected
and `gamma*i` from infected to recovered. The [M5 convergence suite](HYBRID_SIR_MEAN_FIELD.md)
separates SD step-halving error, stochastic population spread and Monte Carlo mean
error, comparing N=40/160/640 against independent count-CTMC and SciPy references.
Its stored figures and numeric gaps cover four declared cases and finite horizons.

### Synchronous Bass adoption and the SD limit

`abm::models::Bass` stores fixed-membership integer adoption flags in the native
typed synchronous population. Every unadopted agent uses the common committed
fraction a and linear probability `dt*(p+q*a)`; adopted agents stay adopted. The
next tick sees all newly adopted agents together. N includes all fixed members.
Parameters require finite nonnegative rates, positive finite dt and
`dt*(p+q) <= 1`; probabilities are never clipped. Empty populations are valid.

Stable-ID addressed Philox draws use ticks 0..65535 and one named stream. Every
scripted variate must be finite [0,1), including variates for adopted agents;
comparison is strictly `u < probability`. Tick time is integer ticks times dt,
with finite strict advancement. State and tick commit as one transaction; copied
populations own their phases/state, and observations consume no draws. Membership,
0/1 state and irreversible adoption invariants are checked.

At fixed dt, increasing N approaches the Euler recurrence
`a_next = a + dt*(p+q*a)*(1-a)`. Reducing dt then approaches continuous Bass SD.
The [Bass convergence suite](HYBRID_BASS_MEAN_FIELD.md) checks both limits with
an independent binomial chain, closed-form SD, pinned SciPy and separate population,
mean, sampling and numerical-error measures. This native model does not add a new
IR component kind or alter the existing homogeneous adoption mode.

### Queue backlog pulses and the reflected SD fluid limit

`hybrid::models::QueueBacklog` composes a scheduled source, FIFO single server,
arrival/completion pulse bridges and clocked SD through the native DEVS kernel.
Arrivals add one and completions subtract one from a nonnegative whole-job stock.
Initial queued jobs are a time-zero source batch; the stock starts at zero to
avoid double counting. The backlog includes the active service job. The source
preserves batch order and the server preserves that order for simultaneous arrivals.

`observe(horizon)` drains all events and zero-time publications through the horizon
before returning. It requires exact equality between source emissions and server
admissions, and between the SD stock, admissions minus completions, and waiting
jobs plus the busy indicator. Intermediate microsteps can contain pending pulses
and are not observable snapshots. All kernel steps are transactional; an exception
restores that step, while prior successful steps in the request remain committed.
A step-budget interruption can be retried at the same horizon. Invalid/backward
horizons and zero budgets are rejected. Component lookup after rollback uses IDs,
so replaced atomic instances cannot leave stale observation pointers.

The [fluid-limit suite](HYBRID_DES_FLUID_LIMIT.md) scales arrival and exponential
service rates by N, with N*b0 initial jobs, and compares stock/N to
`max(0, b0 + (lambda-mu)*t)`. The separate native SD model uses constant inflow and
explicitly ordered, conservatively limited service outflow. Integer DES accounting,
stochastic variation, Monte Carlo error and numerical SD agreement are checked
separately. Empty-boundary finite-N mean gaps are not treated as integration errors.
Scope is a scheduled FIFO single-server composition with a fixed topology; no
general typed bridge graph or time-varying fluid-limit claim is added.

### Typed agent continuous stocks (native Euler contract)

`hybrid::AgentStocks<Tag>` owns a typed population store and global SD stocks.
Selected real-valued fields are the authoritative per-agent continuous state;
other fields, stable IDs, membership and topology are preserved. Membership is
fixed after construction in this first native bridge. Retired rows are excluded.
Each flow binds a boundary, a field on its current agent, or a global stock.
Per-agent flows may transfer to/from global stocks; global flows can read a
last-committed population sum. Internal transfers use one flow at both endpoints.

A step from t to t+dt evaluates every requested rate against the same committed
population and global-stock snapshot at t, then uses the existing SD Euler engine.
All agents and global stocks integrate simultaneously. Newly committed aggregates
at t+dt first influence the interval beginning at t+dt, never the interval just
finished. Callbacks must be pure, with value-owned captures for independent copies;
external callback side effects cannot be rolled back. The bridge rejects recursive
mutation. Its flow configuration freezes on the first successful step.

There is no clipping: finite rates must obey their declared sign policy and all
resulting stocks must obey their individual sign policy. Nonfinite arithmetic,
domain errors or callback failures leave population, global state and time intact.
Time must advance strictly by a finite positive dt. Deterministic evaluation and
summation follow ascending stable IDs and declared flow/field order; floating-point
conservation is checked with a scale-aware tolerance, not claimed bit-exact.
Empty populations contribute a zero sum; global flows still run. Snapshots and
copies own their state and observation consumes no RNG.

This is a synchronous, value-owned C++ bridge. It does not yet expose declarative
expressions, DEVS event/pulse phases, lifecycle changes, clipping priorities or
higher-order integration. Coupled RK stages require simultaneous population/global
stage snapshots and must not be approximated by frozen-aggregate RK4.

### Typed snapshot aggregates and scalar-driven SD

`PopulationAggregate<Tag>` accepts one value-owned `PopulationSnapshot<Tag>` per
input bag: committed population, absolute timestamp and strictly increasing
revision. Its configured store namespace, first ID and field schema must match.
One upstream source owns the stream after the first accepted snapshot. The producer
is responsible for publishing only committed population state. The bridge does not
borrow a population pointer or infer lifecycle actions from successive snapshots.

Each named reducer reads live records in ascending stable-ID order, with an
optional pure selection predicate. Sum and count return zero for an empty selection.
Mean, minimum and maximum reject empty selections unless a finite explicit fallback
was configured. Count has no projection; other reducers require a finite-valued
projection. Sum rejects overflow; mean uses an overflow-safe running interpolation
so finite means remain possible when the corresponding sum would overflow. All
reducers commit together as one ordered scalar vector, published one zero-time
microstep after acceptance, including unchanged values. Names are unique and define
stable vector positions. Revisions never wrap; stale/duplicate revisions, multiple
writers and ambiguous multi-snapshot bags are rejected.

`SignalSD` owns SD stocks and a scalar input vector. Flow callbacks read its stock
state, latched vector and integration time. It integrates to a publication timestamp
using the previous vector, then replaces the vector atomically. A publication at a
tick, including one received in a later microstep at that timestamp, affects only
the following interval. Off-grid publications split the Euler interval; publishing
an unchanged vector can therefore change Euler truncation error for state-dependent
flows. Observation alone does not split an interval or force a step.

The SD clock uses integer-indexed absolute ticks. Inputs must have the configured
vector width, finite values, an exact matching timestamp and increasing revision,
and belong to one upstream source. Initial scalar values are explicit. Euler flows
and stocks have explicit sign policies, without clipping. All direct transitions
stage their state before committing; checked DEVS steps additionally roll back
publishers, consumers and pending messages together after downstream failure.
Confluence publishes a pending old aggregate before accepting a newer same-time
snapshot; no positive time elapses between those publications. Callbacks must be
pure, with value-owned captures for independent copies.

These are native typed DEVS endpoints and C++ expression callbacks. Declarative
unit checking, general graph bindings, a typed ABM publisher adapter, and pulse or
lifecycle coupling into this SD consumer remain separate work.

### Agent-stock DEVS snapshot publisher

`AgentStocksAtomic<Tag>` owns one native agent-stock core, starting at time zero,
and publishes its initial committed population at time zero. At each integer-indexed
absolute tick it first integrates the entire core to that tick, then publishes a
value-owned population snapshot one zero-time microstep later. Snapshot revisions
are tick indexes, with zero for the initial state. A publication step performs no
integration. Observations must drain all same-time microsteps to see settled values.

Connecting this publisher to `PopulationAggregate` and `SignalSD` gives an explicit
continuous-state -> committed snapshot -> scalar publication -> held SD input path.
If all components are due at the same timestamp, the consumer integrates its ending
interval with the preceding aggregate. The newly integrated agent values fund only
the following interval, independent of component declaration order. Different SD
and population tick sizes retain these same held-input semantics; no interpolation
of the population's continuous fields between its committed ticks is implied.

Direct internal transitions stage integration, revision and next deadline together.
Checked DEVS steps roll back all components when any transition in that step fails.
A downstream publication failure preserves the already committed core at that tick
and retries the pending message, rather than integrating the core again. Initial
or tick publication can be retried after a zero-step-budget interruption.

The initial adapter is autonomous and fixed-membership: all external inputs and
confluent input bags are rejected. A core that has already advanced is rejected at
construction. Pulse/lifecycle actions and synchronous behavioral phases require
their own staged ordering contract before this adapter accepts them.

### Typed ABM result-to-snapshot adapter

`PopulationResultPublisher<Tag>` adapts the existing `PopulationAtomic` committed
result to the shared population-snapshot interface. It starts from an explicit
time-zero population copy that must match the producer's initialized store. It
publishes this initial snapshot as revision zero, then increments its own revision
for every accepted result, including equal-valued or same-time results. Publication
is one zero-time microstep after acceptance; it never reruns ABM phases or timers.

Each input bag must contain exactly one committed result from the same producer.
Timestamps must be finite and nondecreasing, with exact elapsed-time agreement;
the store namespace, first ID and field schema must match. Timers, topic deliveries
and input-command metadata stay with the producer's result; the adapter publishes
only its immutable population snapshot. A malformed result or exhausted revision
counter leaves the old snapshot and pending publication intact.

The adapter inherits the producer's committed lifecycle/statechart semantics. For
synchronous populations, same-time lifecycle and input commands precede the due
phase; for asynchronous populations, due timers precede lifecycle/commands. The
adapter observes only the resulting committed store. It adds no new behavior,
membership mutation, model time advancement or inference about causal history.

### Declarative continuous-agent / aggregate / SD subset

The `agent_stock_sd` mode declares one `population` with `execution: continuous`,
typed fields, explicit initial records and an optional population `dt` (defaulting
to the model dt). `agent_stock` components bind unique real fields of that population
to optional nonnegative `inflow_expr` and `outflow_expr`; at least one expression is
required. A field's `non_negative` policy defaults to true. Rates read the common
committed agent record and parameters at the start of its Euler interval. No
behavioral phases, changing membership or SD-to-agent feedback are implied.

Named `aggregate` components bind the same population to sum/mean/count/min/max,
an optional numeric-boolean filter and, where applicable, a projection expression.
Filters must evaluate to exactly zero or one. Mean/min/max can declare a finite
`empty_value`; sum/count use zero. Aggregate projections and filters read fields
and parameters only. Numeric fields include real, exactly representable integer,
and boolean (zero/one); string fields are not numeric expression symbols.

Standard `stock` and `flow` components form the consuming SD model. Their flow
expressions can read stock IDs, aggregate IDs, parameters, t and dt. Outputs are
explicit expressions with declared units over that same scalar scope. Every ID is
unique in its component/parameter/output namespace; agent field names cannot
shadow parameters or built-in time/function names. All dimensions are checked:
agent flux = field/time, aggregate projection = aggregate unit, count and filters
are dimensionless, SD flux = endpoint stock/time, output = declared output unit.

The runtime composes the validated native agent-stock publisher, grouped aggregate
publisher and scalar SD consumer using checked DEVS steps. Initial snapshots drain
at time zero before outputs. Output requests do not interpolate agent fields or
force an off-grid SD step. Publications split SD intervals under the held-input
rules above; different agent and SD steps are permitted. In agent rate expressions,
dt is the nominal population step; in SD flow/output expressions it is the model
step. Actual Euler integration uses the elapsed interval supplied by the clock.

This first declarative path is deterministic, fixed-membership and Euler-only.
Only arithmetic and NONNEGATIVE expression functions are supported. Tables, delays,
clipping, pulses, statecharts, lifecycle, multiple populations and general cyclic
hybrid graphs remain outside this mode. Parameter overrides retain their declared
dimensions and must be finite; invalid runtime rates, filters or stocks fail with
a structured model error rather than being clipped or silently converted.

### Typed event pulses and scalar SD

`StockPulse` carries an absolute `time`, nonempty named `channel`, monotonically
increasing `revision`, and a dense finite signed amount vector in destination stock
order. Each channel has one producer, pinned on its first accepted pulse. The
consumer explicitly declares allowed channel names. Width, channel, revision,
owner and timestamp mismatches reject the entire transition. Scalar publications
remain on port 0; stock pulses use port 2. A bag can contain at most one scalar
publication and at most one pulse per channel. Scalar-only consumers retain their
existing contract.

`SignalSD` integrates the ending interval with its old signal vector. It then
combines the bag's pulses in lexicographic channel order, sums a net amount for
each stock, and applies the net vector simultaneously. Nonnegative-stock policy
is checked on the resulting vector, not on each individual channel's intermediate
withdrawal. Nonfinite partial sums or final states reject rather than saturating.
The scalar vector, if present, becomes the input for the following interval.
No integration occurs between same-time publication microsteps. Different
microsteps are separate transactions: same-time deposits in a later microstep
cannot rescue an earlier invalid withdrawal. On tick confluence, integration,
pulse/scalar acceptance and next-deadline advancement commit together. All direct
transitions stage state as well as supporting checked kernel rollback.

`EventToStockPulse<Event, Message>` maps typed event payloads into this format.
A pure key callback gives each event a globally unique uint64 key for this bridge;
a pure amount callback reads its typed payload and event time and returns one
finite signed vector of the declared stock width. Events in a bag are evaluated
in ascending key order, independent of input/source order. Duplicate keys, including
replays from earlier committed batches, reject. The bridge retains keys for its
lifetime; repeated business entities must use distinct event keys for distinct
triggers. Payload references that can mutate outside the atomic are outside the
value-owned callback contract.

Each accepted nonempty bag produces one revisioned pulse in a following zero-time
publication step. Every event counts even when its projected amount is zero. A
later same-time bag can arrive confluent with the preceding publication; the old
pulse is output first, then the new bag stages the next revision. Seen keys,
amounts, clock, pending publication and revision commit together; callback failure,
overflow or downstream checked-step failure is retryable without duplicate stock
application. Channels can combine multiple event producers if their keys share
one unique namespace. No unit inference is performed by this native callback API;
unit-checked declarative event bindings remain a separate gate.

### Typed event-to-population lifecycle publication

`EventToLifecycle<Event, Tag, Message>` projects typed event payloads to birth
records and retirement references for one population namespace/schema. It owns
no authoritative population state and never predicts or allocates a newborn ID.
The existing `PopulationAtomic` owns allocation, liveness, network membership,
newborn initialization and timer cancellation. Population results expose the
committed IDs after application.

A pure event-key callback supplies a globally unique uint64 lifecycle sequence.
The bridge retains seen keys, rejects duplicates/replays, sorts a bag by key and
publishes one `PopulationLifecycleInput` per event in the next zero-time step.
Distinct producers targeting one population must share a unique sequence namespace.
Bridge publications carry the target store namespace and schema; the receiving
population rejects mismatches and retains bound event sequences to reject replay
across bridge producers. Legacy direct lifecycle inputs can omit this target and
retain their existing per-bag sequence contract. Birth records are validated
against the captured schema. Retirement namespaces,
48-bit IDs, duplicate retirements within the batch, and configured per-batch birth
and retirement limits are checked before publication. Live/dead reference checks
and allocation exhaustion remain authoritative at the receiving population.
Empty effects are permitted as explicit no-op events and still consume their key.

Only the receiving population may retire its agents. A successful retirement
cancels owned timers and removes incident network edges; it does not retire any
separate DES entity record. DES event identity and population identity are separate
unless an explicit entity-agent owner joins them. No mutable store pointer or
implicit synchronization is introduced by this bridge.

DEVS publication latency is visible. If the upstream event is delivered to the
bridge at a population tick, that tick can commit before the bridge publishes the
lifecycle change. Newborns then first join the next synchronous phase. An already
committed phase or async timer is never undone or rerun because a later microstep
at the same timestamp changes membership. Direct lifecycle/tick confluence still
uses the existing population contract. All bridge transitions stage the pending
batch, seen keys and clock. Failure during checked delivery restores the population
and pending publication for retry, preserving earlier successful microsteps.

### Shared entity-agent ownership

`EntityAgentAtomic<Tag, Message>` owns one typed `PopulationAtomic` and a process
ledger. DES tokens use the same typed `(store, id)` references as the population;
there is no second independently mutable DES record. The wrapper accepts the
population's existing command/topic/lifecycle/network inputs and one process-return
port. Population callbacks/statecharts are the sole authority for record mutation.

A pure eligibility callback selects live agents that have not yet launched. Each
selected agent receives one process token with its shared reference, a configured
priority and a value-owned snapshot of its dispatch-time schema/record/time. The
snapshot is immutable process input: DES duration/selection callbacks can inspect
it without borrowing the live store. It is not a competing authoritative state;
a return action reads the population's current record. Existing reference DES
blocks carry this optional snapshot unchanged. Legacy tokens have no snapshot.

Each agent makes at most one process journey in this initial contract. The ledger
tracks waiting, in-flight and returned references and retains terminal identities
for replay rejection. Returned tokens must identify a live in-flight agent, have
no outstanding leases, and preserve the issued priority and snapshot. A return is
translated into a configured population command at the exact return time. The
command can update fields, transition a statechart or retire the returning agent.
The wrapper reserves command sequence UINT64_MAX for completion; ordinary commands
for the same agent in the same bag run before its return action. Explicit external
commands using that reserved sequence are rejected.
Unrelated timer/phase/lifecycle actions may not retire an in-flight agent: such a
transition rejects and rolls back instead of leaving a dangling process reference.
Waiting or already-returned agents may retire normally. Newborn allocation remains
owned by the population and is discovered from committed live records.

The wrapper stages population, ledger, dispatch snapshots, clock and pending token
publication together. Initial zero-time population timers settle before initial
eligibility is evaluated. Committed population result messages retain port 1;
process dispatch uses port 5 and returns use port 6. Launches publish after the
population commit in a following zero-time step. Confluent process returns use the
existing population timer-before-command or lifecycle-before-sync-phase policy at
that microstep. A previously committed tick is never replayed. Failed downstream
publication remains pending for checked-step retry. Eligibility/priority callbacks
must be pure and own their captured data; no external mutable store pointer is used.

This provides shared identity and statechart/process lifecycle ownership for a
single journey per agent. Re-entry/revisits, process cancellation/preemption and
mutation of dispatch snapshots are outside the initial contract. Retiring an
in-flight agent requires completion through the process return path first.

### Scalar SD publication and typed rate/routing consumers

`PublishingSignalSD<Message>` wraps the existing Euler `SignalSD` by value and
publishes a pure projection of committed stocks, held inputs and time as a
`ScalarPublication` on port 1. Initial projection is revision zero at time zero.
Each committed tick or accepted input computes the next projection, then publishes
in a following zero-time step. Projection width is fixed and values must be finite.
Projection, underlying SD state and revision commit together. Same-time feedback
must be causally bounded by the model; unchanged values still publish, and the
kernel's existing microstep budget detects zero-time loops.

`SignalRateSource<Tag, Message>` reads one finite scalar vector on port 0 and uses
a pure projection to obtain a nonnegative piecewise-constant Poisson rate. It owns
an immutable typed entity record store and emits reference tokens with dispatch
snapshots on port 1. A pure record factory reads the new reference, arrival time
and held scalar vector. Schema validation happens before output/commit; generated
IDs remain monotonically allocated in the declared 48-bit namespace.

Each entity's unit-exponential hazard uses its stable ID with the declared Philox
seed/scenario/replication/stream. Rate updates consume the elapsed hazard under the
old rate and preserve the remainder. An unchanged projected rate preserves the
already scheduled absolute arrival deadline. Zero rates pause it; resuming does not redraw.
Rate projection is evaluated initially and on scalar publication, not continuously
or at unrelated observation horizons. At exact arrival/update confluence, the old
arrival and old factory inputs apply first; the new vector determines following
arrivals. Scalar width, timestamp, revision and single-producer checks are strict.
Invalid rates, records, exhausted addresses and unrepresentable future deadlines
reject transactionally. An off-deadline update whose residual hazard rounds to a
nonpositive value also rejects instead of inventing an early zero-time arrival.
The source is bounded by its configured count (0–1,000,000) and can remain passive
after exhaustion while accepting valid later scalar revisions.

`SignalSelect<Tag, Message>` routes typed process tokens using a categorical
probability vector projected from held scalars. Tokens enter port 0; scalar updates
enter port 2; branch outputs use ports 1..N. Branch count is fixed. The existing
categorical probability validation and addressed Philox mapping apply. Tokens in
a bag are handled in stable reference-ID order with the preceding committed
probabilities; a simultaneous scalar update applies to subsequent input bags.
This rule is independent of bag order. Different publication microsteps at the same
timestamp remain distinct; upstream control must reach the selector before a token
bag to affect that bag. Repeated references reject, and tokens preserve snapshots,
priorities and leases. Probability changes do not consume random draws.

All three endpoints own callback captures and state, preserve absolute timestamps,
use explicit zero-time publication phases and support checked retry. Native
callbacks do not infer physical units. General declarative auxiliary/source/routing
bindings require their own dimensional validation.

### Typed agent-backed pool and broker writeback

`TypedAgentPool<Tag>` owns one typed population store, one integer resource pool,
an assignment ledger and bounded assignment/unassignment broker topics. A pure
capacity callback computes each live agent's total capacity from the committed
record; it must not depend on the reserved integer allocation field. The owner
writes that field from its ledger. Initial and newborn allocations must be zero.
Capacity contributions fit nonnegative int64; aggregate capacity and resource
counts are checked size_t integers.

A transaction applies releases first, writes all released allocations, and delivers
canonical unassignment notifications. Then it applies explicit record updates,
retirements and births, followed by optional registered synchronous Jacobi phases.
Explicit updates are full post-release records, including any handler changes
the caller intends to preserve. Each update/phase preserves the reserved allocation field; each phase must leave
capacity sufficient for every active assignment. A busy agent cannot retire or
become unavailable unless its allocations are released in this same transaction.
The owner then resizes the pool, processes new requests and maps grants to available
agents in stable ID order. Requests/releases at one transaction time are ordered
by request ID; the underlying FIFO/LIFO/priority discipline applies to its queue.

Each request can span several agents. The sum of each request's shares equals its
grant; per-agent shares equal its reserved allocation field; the sum equals the
resource pool's allocated units. No assignment may exceed the projected capacity.
All assigned fields are written before delivering the assignment topic batch.
Broker notifications use request ID as sender, agent ID as sequence/receiver, and
carry full typed references, units and direction. Optional handlers run in canonical
request/agent order, can update other fields, and see prior handlers' committed
candidate records. They cannot change allocation or any projected capacities.
A bounded-topic overflow or handler failure rolls back the whole transaction.

Capacity-time, allocated-unit-time, waiting-request-time and live-agent-time are
integrated from the preceding committed levels. Read-only horizon statistics do
not advance the owner. Population, pool, ledger, broker, statistics, IDs and time
commit together; no other component holds a mutable population pointer.

`TypedAgentPoolAtomic<Tag, Message>` accepts typed resource seize/release messages
and at most one revisioned workforce control per bag. The first workforce producer
is pinned; revisions must increase. Controls and resource messages merge into the
same transaction. Control time must match the DEVS timestamp. Results publish in a
following zero-time step, alongside routed grants (port = 1 + request's high 16-bit
block ID), result port 65537, committed population-result port 65538 and notification
port 65539. Consumers can feed that population result into the existing aggregate
adapter. The atomic is event-driven; registered ABM phases run only when requested
by an explicit control. Publication confluence and checked retry preserve the
already committed transaction without duplicate assignment or allocation.

This native bridge has integer units and one authoritative pool per population.
Multi-pool allocation fields, preemption and automatic behavior clocks are outside
this contract. Callback purity/value ownership and unique lifetime request IDs
follow the existing resource and population contracts.

### Continuous agent stock lifecycle and pulse transactions

`AgentStocks` can apply a discrete change at its committed clock. Bound real stock
fields remain owned by the integrator: ordinary field updates may only change
other schema fields and each agent/field pair occurs at most once per change.
Pulses carry signed finite amounts to a bound field on a live typed reference or
to a global stock. Net pulses for each endpoint are accumulated in declared order
and applied simultaneously before domain checks. No clipping or intermediate
nonnegativity test is used. Pulse references must exist before the change; callers
cannot predict or pulse a newborn ID in the same transaction.

After field updates and net pulses, retirements remove agents, then births allocate
fresh records in declared order. Retirement references are unique; retirement may
follow an update/pulse to that agent. Birth stock fields obey their domains. Network
membership follows PopulationStore lifecycle rules. A receipt records born and
retired references, birth/retirement amounts per bound stock field and net pulse
amounts per agent field/global stock. Retired stock leaves the modeled population;
this is an explicit boundary removal recorded by the receipt, not a transfer to a
global stock. A model needing a transfer supplies the corresponding signed pulse
before retirement. All records, global stocks, topology, IDs and receipts stage
together. Successful discrete execution freezes flow registration, like integration.

`DynamicAgentStocksAtomic` integrates the old committed population and rates to an
input timestamp, applies one merged discrete change, and publishes the resulting
population snapshot and global/receipt commit in a following zero-time step. A
newborn accrues stock only after its birth. Retiring agents accrue through their
retirement timestamp. Off-grid inputs split the Euler interval; original absolute
tick deadlines remain on the configured dt grid. Merely observing between events
does not split integration. Explicit `step_to` preserves the exact target timestamp;
ordinary `step(dt)` retains its existing requested-dt arithmetic.

Revisioned `AgentStockInput` messages enter port 0 through configured nonempty
channel names. Each channel pins its first producer and requires increasing
revisions; at most one message per channel per bag is accepted. Channels merge in
lexical order, preserving each declared vector order. Bound `PopulationLifecycleInput`
messages enter port 3, with matching namespace/schema and unique lifetime sequence
keys; they merge after the stock channels in sequence order. Duplicate retirements
or field writes across merged inputs reject. Canonical merge order determines birth
ID allocation and floating-point pulse summation, independent of bag order.

At exact tick/input confluence, the old population integrates once before the
change. A control arriving during a pending publication at the same time starts a
new transaction without reintegration. Population snapshot output is port 1; global
state and optional discrete receipt output is port 2. Revisions increase on every
successful tick or input commit, independently of the integer tick counter.
Checked failure restores integration, lifecycle, pulses, input revisions and pending
publication together. Higher-order integrators and ordinary ABM behavior clocks
remain outside this Euler contract.

### Ordered experiment execution

The CPU experiment runner accepts 1–256 requested worker threads and at most
1,000,000 trajectories per invocation. Scenario IDs and replication indices retain
the existing 16-bit addressed-RNG limits. Tasks have canonical scenario-major,
replication-minor indices. Each trajectory receives its own copy of the pure run
callback and produces a private observation vector. Shared mutable callback captures
and external side effects are outside this contract.

Workers may finish in any order. The coordinator validates/merges results in task
index order, preserving each trajectory's declared observation order. Numeric
observations must have finite times and values, nonempty output names and unique
time/output pairs. Negative source-clock times from imported SD models are valid
observations; this does not relax elapsed-time constraints in model schedulers.
Only a completely successful invocation returns trajectories.
If several trajectories fail, the earliest canonical task's exception is rethrown,
independent of worker completion order. No partial result is returned.

Optional progress callbacks run only on the calling thread, initially at zero and
then once per successfully merged trajectory. The completed count is the ordered
validated prefix, not a count of out-of-order worker completions. Returning false
requests cancellation. An external stop token can also cancel. Cancellation stops
new task acquisition, joins in-flight work, and raises `ExperimentCancelled` with
the merged prefix count and total; callbacks are not forcibly interrupted. Progress
exceptions also stop acquisition and join workers before propagating. No worker
outlives the runner, including thread-creation failure. Cancellation takes precedence
when observed before a task result is consumed.

The serial default uses the same validation and merge contract. Thread count does
not enter random draw addresses or scenario parameters. Ordered summary accumulation
therefore retains identical numeric results for identical trajectory vectors on the
same build. This is a correctness/isolation contract; no throughput claim is implied.

### CLI ensemble controls and result publication

`fathom run` accepts strict decimal `--threads` (1–256) and `--seed` (uint64) values.
Duplicate, signed, fractional and overflowing options reject. A seed override
replaces the experiment seed when an experiment is supplied and the default
single-model seed (zero) otherwise. A model without an experiment has one effective trajectory regardless
of the requested worker count. Existing single-model and ensemble CSV column
contracts remain unchanged.

The CLI completes and validates trajectories before formatting/publishing results.
For file outputs on the supported POSIX hosts, a private sibling temporary file is
written, flushed, closed and atomically renamed to the destination. Simulation or
formatting failure cannot truncate a previous result. Write/rename failure removes
the temporary file and preserves the previous destination. Input alias protection
uses filesystem identity, including existing symlink/hard-link aliases, rather
than only comparing path strings. Standard output is published after simulation
success, but a terminal/pipe failure cannot undo already written bytes. This is
atomic file visibility, not a claim of directory-entry durability after power loss.

### Scenario design expansion

An experiment contains exactly one explicit `scenarios` list or `design` object.
Design axes/bounds are sorted lexically by parameter name. Generated scenario IDs
increase from `first_id` (default zero), fit the existing 16-bit address, and do not
change the sample coordinates. Every generated name must be a declared model
parameter. Values/bounds are finite; continuous bounds strictly increase. Designs
have 1–32 parameters and generate at most 65,536 scenarios, further limited by the
chosen first ID and the runner's total-trajectory bound.

A grid preserves the declared values within each axis and enumerates the Cartesian
product with the last lexical axis varying fastest. Duplicate parameter vectors are
permitted as separate scenario IDs. LHS uses one independent Fisher–Yates permutation
per parameter and one open-unit Philox jitter per sample/parameter, putting exactly
one point in every one-dimensional stratum. Permutation choices use unbiased
rejection sampling. Bounds map normalized coordinates with fixed-policy `std::lerp`.
Rounded physical values remain inside the closed declared interval.

LHS has an explicit uint64 `design_seed`, which must differ from the effective
execution seed in a loaded experiment, including CLI seed overrides. Design Philox
uses key = the design seed and counter words `(index, dimension, retry, purpose)`.
Purpose is 0x4c485350 for permutation and 0x4c48534a for jitter; word zero is used.
Indices/dimensions follow canonical axis ordering, and retry is bounded to 65,536
attempts per permutation choice. This separates design randomization from trajectory
addresses using distinct keys. No mutable RNG cursor or thread count enters design
expansion.

Sobol uses 32-bit directions derived from the first 32 Joe–Kuo D(6) polynomial and
initial-direction rows, pinned from SciPy 1.18.1's dataset with provenance/license.
Sample count must be a power of two in [1, 65,536]. The native implementation uses
direct Gray-code XOR evaluation, starts at point zero, and has no skipping,
thinning, optimization or scrambling. Integer direction arithmetic is exact; unit
coordinates divide by 2^32 and then use the same bounds mapping. SciPy's independent
unscrambled engine is the comparison oracle. A Sobol design is deterministic and
has no design seed. Replications still use the independent execution seed/addresses.

### Observation output publication

The runtime output adapter preserves scenario/replication order and each
trajectory's observation order. It rejects duplicate addresses, out-of-range
addresses, invalid observations and duplicate time/output pairs. Arrow observation
schema 0.1 is `(scenario:uint32, replication:uint32, time:float64,
output_id:utf8, value:float64)`, all non-null; single runs use address `(0,0)`.
Parquet stores the Arrow schema, and IPC uses file format. Finite float64 bits,
including signed zero and subnormals, survive serialization. Writer bytes are
not the reproducibility contract. CSV preserves its existing single/ensemble
layouts with locale-independent numeric formatting and escaped native names.

Files are published only after complete simulation, validation and serialization.
A private sibling temporary is flushed, closed and atomically renamed. Any
failure before rename leaves an existing destination intact and removes the
temporary. This POSIX contract does not guarantee directory metadata durability
after power loss. Binary output requires an Arrow-enabled build and a file path;
standard output remains CSV. See [output contracts](RUNTIME_OUTPUTS.md) for schema,
build options and evidence. Run manifests and parameter lineage remain later M6
work; schema metadata alone is not a provenance record.

### Data snapshot identity and row order

Local data readers hash and decode the same owned input bytes. They require an
exact schema and a declared key; no columns, nulls or invalid values are silently
dropped/coerced. Loaded records own their values independently of the source file.
Columns sort by name and rows sort by the typed key, with unsigned UTF-8 byte order
for strings. Duplicate keys fail before publication of the table. Float keys are
finite and treat signed zeros as equal for uniqueness; float values retain their
bits for content identity. Dictionary encoding does not change logical values.

The raw file hash and the canonical table hash serve different purposes. The
canonical version-1 encoding includes normalized schema, unit/domain declarations,
key selection and all typed values, with fixed-width little-endian scalars and
length-prefixed UTF-8 strings. It excludes physical row/column order, writer
metadata, compression and chunk layout. See [data contracts](RUNTIME_DATA.md) for
the complete byte format and independent reference checks. These identities are
foundations for provenance; run manifests and model binding adapters remain open.

### Population initialization from data

The native population data adapter initializes only a never-populated typed store.
Every target field has one explicit source-column mapping and expected unit. Unit
strings must match exactly. i32 widens to i64; u64 must fit i64; all other supported
field transfers preserve their type/value. No implicit real/integer conversion
occurs. Mappings validate even for empty input; empty initialization consumes no IDs.

Canonical source-key order determines row assignment, while the store allocates
its own 48-bit agent IDs. Full source keys are retained in an owned receipt with
file/canonical hashes. Mappings, conversions, records, IDs, network membership and
receipt allocation are staged; only a nonthrowing final move commits. Invalid rows
or exhausted identity space leave the target unchanged. A retired population cannot
reuse initialization to reset its IDs. Native schemas do not carry units, so the
caller supplies expected target units; declarative binding and dimensional checks
remain later work. See [population binding](POPULATION_DATA_BINDING.md).

### Native exogenous series and keyed parameters

Series require a nonempty snapshot whose unique key is its f64 time column.
Hold is right-continuous. Linear queries use the containing interval and fixed-policy
`std::lerp`; exact knots and held endpoints retain source bits. Both policies hold
the nearest endpoint outside the source range. Sampling computes every time as
`start + double(index) * step`, validates strict advancement and finiteness, and
returns a complete owned grid or fails. No samples are silently dropped or clipped.
Native time/value columns and parameter-value columns require f64 with exact
expected unit labels; no automatic numeric or unit conversion occurs.

Parameter selection uses exact typed keys and returns an owned, lexically ordered
map plus source identity. Missing keys fail instead of supplying defaults. Native
adapters can be shared across private trajectories; samples are indexed on the
caller's chosen simulation grid. Declarative registration, scenario override
precedence, higher-order integration stage sampling and full provenance records
remain later contracts. See [input bindings](DATA_INPUT_BINDINGS.md).


## Declarative parameter-table inputs

Standalone SD model files may declare required `data` entries with
`use.kind = parameter_table`; see [the binding contract](PARAMETER_DATA_IR.md).
Paths resolve relative to the model file, types and unique keys use the native
table rules, mapped values must be f64, and declared dimensions must match model
parameter dimensions. No scale conversion is inferred. Multiple bindings cannot
write the same parameter. Literal values are superseded by selected table values
before component validation; explicit scenario overrides supersede both at run
time. Unbound literals retain their values.

Loading snapshots effective values and source/hash/key/mapping receipts into the
model. Run workers never reopen bound data; reloading is an explicit new snapshot.
CLI output paths cannot alias a bound input at preflight. The JSON schema and
loader reject `data` in other modes and nested hybrid submodels. Optional fallbacks,
scenario-dependent table selection, declarative series/population inputs and
serialized manifests/replay are not part of this increment.


## Declarative exogenous-series inputs

Standalone SD `data` may also declare `use.kind=exogenous_series`, exposing the
binding ID as a unit-checked unary function. Its argument has the model time
dimension and its return has the declared value-column dimension. Time must be
a unique f64 key; values must be f64. Required interpolation selects linear or
right-continuous hold; required extrapolation selects endpoint hold. Singleton
series are constant. Source times can precede model start.

Evaluation queries the immutable source snapshot at the actual expression
argument, including RK4 stage times and explicit shifts. It does not pre-hold
observation-grid samples. RK4 retains its fixed-step quadrature across knots;
no automatic step splitting or discontinuity convergence claim is implied.
Outputs use observation time and delays retain their established Euler tick
semantics. Series function IDs cannot collide with parameters, components or
reserved built-ins. Models retain source hashes, mapping/policy and owned values;
CLI outputs protect bound source aliases at preflight. See
[SERIES_DATA_IR.md](SERIES_DATA_IR.md) for the complete contract.


## Declarative population-table inputs

Typed ABM model files support exactly one required `population_init` data binding
instead of inline agents. Every target field maps once with matching dimensions;
source types follow the native initializer, with the additional existing IR exact
integer range ±(2^53−1). External keys retain their complete declared type/range.
Canonical source-key order assigns engine IDs starting at zero; keys never become
simulation IDs directly. The loaded Model owns initial records and a source-key,
agent-reference and file/canonical-hash receipt.

Records pass through the existing sync/async, chart, lifecycle, spatial and network
validation before load succeeds. Runtime workers use the snapshot and existing
population engine. Empty input is allowed only when the model's references remain
valid. Inline agents plus a binding reject; no append or fallback is implicit.
Only initial membership is captured in the receipt. Adding/removing source keys
may change assigned IDs. See [POPULATION_DATA_IR.md](POPULATION_DATA_IR.md).


## Run manifests and strict local replay

`run --out ... --manifest ...` records captured model/experiment identities,
loaded binding hashes, expanded scenarios/effective scalar parameters, effective
seed, precision/build policy and a SHA-256 of ordered numeric observations.
Input JSON hashes and parsing consume the same captured bytes. Result identity
encodes trajectory addresses, counts, time/value IEEE-754 bits and UTF-8 output
IDs with explicit little-endian lengths; it is independent of output format.

`replay` reloads and validates the original local inputs, verifies raw/canonical
identity and build policy, then recomputes and compares numeric identity before
publishing results. Thread count and output format/path may change; model, data,
experiment and seed may not. A verification-only replay prints a JSON verdict.
Version 0.2 manifest-backed results carry scenario/replication, manifest ID and
effective scenario parameters on each observation. Arrow/Parquet also embed the
complete manifest; empty tables retain that metadata. Numeric identity excludes
lineage. The writer checks identity and complete trajectory/scenario association
before publication. Replay retains the original manifest and its recorded run
settings even when its destination or workers change. Version 0.1 replay and runs
without manifests retain their original schemas. Replay does not restore missing
inputs or verify the old result file. Manifest publication
follows result publication as a completion marker; two-file atomicity and
cross-platform/build replay are not promised. See [RUN_MANIFESTS.md](RUN_MANIFESTS.md).

`verify-results` is read-only schema 0.2 artifact verification against an explicit
manifest. It checks schema, canonical lineage and the ordered numeric digest,
reconstructing empty trajectories from the manifest catalog. Arrow/Parquet also
require matching embedded provenance. It neither loads source inputs nor replays
the model, and does not require the original execution build. Its verdict is
scoped to decoded artifact integrity; it does not assess accuracy or authenticity.
Verification supports at most one million trajectories and currently materializes
decoded rows. With `--embedded`, an Arrow/Parquet result supplies its own reference
manifest from the same decoded table; the CLI also applies the sidecar manifest's
envelope validation. Verdicts distinguish `manifest_source: embedded|sidecar`.
Canonical metadata, duplicate-key rejection and row/numeric checks still apply,
including for empty tables. This tests internal consistency; a consistently
resealed artifact can pass while differing from a separately retained sidecar.
CSV requires a sidecar. Legacy verification and embedded-manifest replay remain open.

## C ABI 1 foundation

`libankurafathom` exposes opaque model/results handles through a C11 header.
JSON and file entry points share the existing loaders; experiment JSON shares
explicit/grid/LHS/Sobol validation. Raw NUL bytes in input JSON reject on both
file and memory paths. Memory models retain input hashes with an empty source path.
Bound-data JSON requires an explicit absolute base directory. Inputs are copied;
results own trajectories independently of model lifetime. Observation getters
preserve ordered addresses and binary64 bits; IDs/digests are borrowed until result
release. Failed calls preserve output arguments, and failed runs publish no handle.

Status-returning functions contain C++ exceptions and use thread-local fixed-size
diagnostics with explicit truncation flags. Successful calls clear that thread's
error; queries and frees preserve it. Immutable handles allow concurrent reads/runs;
release must wait for all uses to finish. Stale or incorrectly typed handles are
outside the contract. See [C_API.md](C_API.md) for ownership, options and evidence.
Arrow C Stream export materializes an independently owned CPU observation table
with schema 0.1 and exact numeric bits. Model/results may be freed after export;
returned schemas/arrays also outlive the stream. Consumers release each parent
object through its Arrow callback, serialize callbacks per stream and keep the
shared library loaded while any exported objects remain live. Callback failures
use Arrow errno/per-stream error semantics. CSV-only builds return
`FATHOM_UNAVAILABLE` without modifying the destination. Callbacks for execution
progress/cancellation remain open. Provenance is now available through a borrowed
manifest getter and an optional schema-0.2 stream export.

The optional nanobind Python interface uses only this C boundary for loading,
execution and Arrow export. `Model.from_json` snapshots JSON/data; `Experiment`
snapshots its JSON and validates it against the model at execution. `run` releases
the GIL during native work, then imports an owned Arrow capsule and returns a
schema 0.1 PyArrow table by default. `provenance=True` returns schema 0.2 lineage
and an embedded version-0.3 manifest instead. Results and slices outlive models
and readers. Structured
exceptions copy status/code/pointer/truncation out of thread-local diagnostics.
`lint` runs the shared loader without executing trajectories; M7 behavioral checks
are not implied. See [PYTHON_API.md](PYTHON_API.md). Local wheels use relative loader
paths from the extension to its private C library and the pinned sibling PyArrow
package. Native SDK/CLI installation exports a relocatable CMake target; native
Arrow-enabled installs still require their external SDK. See [PACKAGING.md](PACKAGING.md).
This does not extend replay portability or scientific acceptance across platforms.


API result receipts are captured once per successful run. They retain input/data
snapshots, effective scenarios/seed, build policy and the unchanged ordered numeric
identity after freeing the model. Version 0.3 uses empty paths for memory JSON and
an exact empty-path/memory-format output declaration. Versions 0.1/0.2 retain their
file-path requirements. Artifact verification accepts 0.3 with explicit result
paths or embedded metadata; replay rejects it before input loading/output mutation.
No fake input/output files are created. See [API_PROVENANCE.md](API_PROVENANCE.md).

## C/Python execution callbacks

The public run callback executes on the calling thread at canonical trajectory
merge boundaries (zero through total inclusive). Cancelling at any boundary joins
workers and publishes no result, including at total. No intra-trajectory checks
are added. Python callback exceptions are captured with the GIL held and re-raised
after native cleanup; successful C calls clear diagnostics left by nested calls.
See [API_EXECUTION_CALLBACKS.md](API_EXECUTION_CALLBACKS.md).

## Default CLI file provenance

`run --out PATH` derives a new sidecar at `PATH.manifest.json`, preserving the full
filename. File output uses schema-0.2 lineage. `--manifest` selects another new
sidecar; `--no-manifest` explicitly selects legacy output and is mutually exclusive
with that option. Stdout emits legacy CSV without a receipt; replay retains its
original manifest without creating a new sidecar. Existing sidecar entries,
including dangling symlinks, reject before execution. Result and sidecar publication
remain separate renames with the receipt last; no two-file atomicity is claimed.

## Portable input resolution

File-backed bundles preserve original manifest and raw input identities. Replay
resolves fixed local members, loads immutable data snapshots through the normal
validators, then compares the original identities, scenario catalog, build policy
and numeric digest. Original paths remain historical metadata and are never used
as fallback reads. Packing validates captured inputs without asserting current-build
replay success; replay always enforces the unmodified strict build policy. Bundled
replay publishes no new receipt and cannot overwrite any member or create outputs
inside the input bundle. See [PORTABLE_REPLAY.md](PORTABLE_REPLAY.md).
