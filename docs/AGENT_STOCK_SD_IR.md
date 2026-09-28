# Declarative continuous agents and SD

The `agent_stock_sd` mode composes continuously integrated typed agent fields,
filtered population aggregates and scalar-driven SD. The runnable
[model](../models/agent_stock_sd.ir.json) represents heterogeneous work remaining
and completed, feeding a global delivered-work stock. Its
[experiment](../models/agent_stock_sd.experiment.json) compares three productivity
settings with two replications each.

```sh
./build/fathom lint models/agent_stock_sd.ir.json
./build/fathom run models/agent_stock_sd.ir.json --out /tmp/agent-stock-sd.csv
./build/fathom run models/agent_stock_sd.ir.json --experiment models/agent_stock_sd.experiment.json --out /tmp/agent-stock-experiment.csv
```

One `population` with `execution: continuous` owns typed real, integer, boolean and
string fields. Each `agent_stock` binds a real field to inflow/outflow expressions.
Rates read a common agent snapshot; all fields commit together using Euler.
Named `aggregate` components support sum, mean, count, minimum and maximum with
an optional filter. Their scalars feed ordinary SD stock/flow expressions.

The loader checks dimensions throughout: agent flux has field/time units, count
and filters are dimensionless, aggregate projections match declared aggregate
units, and SD flows match their endpoint stock/time units. Numeric fields and
parameters are available to agent expressions; SD expressions read stocks,
aggregates and parameters. Only arithmetic and `NONNEGATIVE` functions are in
this subset. Full contracts and rejected combinations are in
[semantics](SEMANTICS.md#declarative-continuous-agent--aggregate--sd-subset).

The population can have its own `dt`. The consumer holds each aggregate until the
next committed population publication. It integrates the ending interval with the
old signal before accepting a new one. Output observations neither interpolate
agent values nor force off-grid integration. Consequently a global accumulated
quantity that mirrors per-agent progress conserves with that progress at shared
committed agent times; at intermediate consumer ticks the agent snapshot is still
held. This is explicit sampled coupling, not continuous interpolation.

## Validation

The frozen [plan](../tests/oracles/hybrid/agent-stock-ir-plan.json) combines four
agent steps, three SD steps and both component declaration orders. The independent
[oracle](../tests/oracles/hybrid/agent_stock_ir_oracle.py) uses exact rational
recurrences on the union of clocks, filtered typed reductions, and stock-dependent
SD transfers. It checks empty populations, parameter overrides and experiments.

- 24 models, **2,728 scalar comparisons**, maximum absolute error **1.78e-15**.
- Six experiment trajectories, **300 additional comparisons**.
- Fifteen corruption controls reject invalid reference evidence.
- Normal and sanitizer reports and every native CSV hash agree exactly.
- **48/48 relevant regression tests** pass in both session builds, including all
  new IR tests, existing IR modes, XMILE and related DES/agent-pool checks.
- Schema/loader validation in both builds passes 52 valid fixtures, 13 interaction
  cases and six generator cases; 412 structural and 248 semantic invalid cases.

```sh
cmake -S . -B build
cmake --build build --target fathom ir_agent_stock_sd_tests
ctest --test-dir build -R '^ir_agent_stock_sd' --output-on-failure
python3 tests/oracles/hybrid/agent_stock_ir_oracle.py --verify --contract
```

This is a fixed-membership, deterministic Euler path. General hybrid graphs,
pulses, lifecycle changes, statecharts and feedback into agent rates remain
separate work. M5 remains open. See [status](STATUS.md) and
[acceptance tracking](M5_ACCEPTANCE.md) for the complete platform scope.
