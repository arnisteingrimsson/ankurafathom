# Workforce and parameter data together

Typed ABM models now accept up to 1,024 bindings: at most one `population_init`
plus any number of `parameter_table` bindings with unique IDs and disjoint
parameter targets. Parameter-only binding is also supported with inline agents.
Binding declaration order does not change the initialized records or defaults.
Scenario overrides take precedence over table values, which take precedence over
inline defaults. Tables resolve before synchronous rules, async transitions and
network-generator parameters are validated. Inputs remain owned snapshots.

Population mapping diagnostics retain the original `/data/N` position even when
parameter tables precede it. Duplicate binding IDs, repeated parameter targets,
multiple population initializers, incompatible units, mixed inline/file records,
missing keys and unavailable readers reject. Exogenous series, multiple typed
populations and general hybrid bindings remain outside this ABM increment.

Every source contributes its raw/canonical identities to manifests. Mixed binding
models support strict replay and portable bundles with no original input files.
The existing manifest/ABI versions are unchanged.

`models/data/workforce_capacity.ir.json` is a small five-year preparation example:
two synthetic workforce rows plus practice assumptions, with baseline/productivity
scenarios in `workforce_capacity.experiment.json`. It reports potential work
capacity, payroll and headcount; demand and revenue are represented separately in
the [economics preview](../examples/ankura_pilot/README.md).

The composition contract covers 62 declaration/format/thread and sync/async cases,
4,320 exact observations, eight corruption controls, three relocated bundles and
366 analytic example values. A native test verifies copy/move snapshot ownership
after both source files change, plus 192 scenario/replication trajectories at
1/8/32 workers. CSV-only builds reject file bindings explicitly.
