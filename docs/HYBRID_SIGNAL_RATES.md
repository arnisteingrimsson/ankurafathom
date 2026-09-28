# SD scalars driving typed arrivals and routing

`PublishingSignalSD` publishes a pure projection of committed SD stocks, held
inputs and time. `SignalRateSource` reads those scalars as a piecewise-constant
Poisson rate and creates typed records with stable addressed identities.
`SignalSelect` reads projected branch probabilities and routes the resulting
reference tokens, preserving dispatch snapshots, priorities and resource leases.

Rate changes retain the pending exponential hazard. Zero rates pause it; resuming
does not redraw. An unchanged rate preserves the scheduled absolute deadline.
An arrival coincident with a scalar change uses the old rate and record-factory
inputs. Routing similarly uses preceding probabilities for a simultaneous token
bag; the new probabilities apply to following bags. All state and publications
are value-owned and support checked rollback.

The [example](../examples/signal_rate.cpp) uses a rising SD arrival-rate stock and
a derived branch probability. Run:

```sh
cmake -S . -B build
cmake --build build --target hybrid_signal_rate_tests hybrid_signal_rate_oracle_tests fathom_signal_rate
ctest --test-dir build -R '^hybrid_signal_rate' --output-on-failure
./build/fathom_signal_rate
python3 tests/oracles/hybrid/signal_rate_oracle.py --verify --contract
```

## Evidence

The frozen [plan](../tests/oracles/hybrid/signal-rate-plan.json) covers constant,
rising, pause/resume, off-grid, zero, count-limited and high-address sources across
all six SD/source/router declaration orders. The independent
[oracle](../tests/oracles/hybrid/signal_rate_oracle.py) uses global cumulative-hazard
inversion, a separate integer Philox implementation and analytic Poisson moments.
The high-address case exercises the final eight 48-bit IDs, maximum seed/scenario/
replication values and the last two stream addresses.

- **9,408 configurations and 41,418 complete event comparisons pass.**
- IDs, branch decisions, typed marks and event counts agree exactly; maximum
  arrival-time/scalar discrepancy is **1.33e-15**.
- **14 predeclared mean/variance gates** pass, including the appropriate truncated
  count distributions. Six cases use 256 replications; high addresses use 32.
- Thirteen corruption controls reject invalid evidence.
- **Eleven distinct focused tests pass in normal and ASan/UBSan builds**: five new
  checks plus existing rate, RNG, SD, aggregate, pulse and reference-routing tests.
- Both trajectory files and scored reports are byte-identical. Example output also
  agrees. Final overflow/projection-width hand checks were rebuilt and passed.
- Hand evidence includes arrival/update confluence, pause/resume, unchanged-rate
  deadline preservation, last-ID and count bounds, schema/projection failures,
  state/record/publication rollback, duplicate routing rejection, immutable snapshots,
  downstream retry and a deliberately unbounded zero-time feedback loop caught by
  the existing kernel budget.

Trajectory SHA-256:
`fe918edfa349778236a0d68ef5d0a6209aee4ce4a17438f20220e045d03dec20`.

## Scope

Rates and probabilities change on explicit scalar publications. They are not
continuously interpolated. Source records are immutable once generated; source
count is bounded at configuration time. Callbacks must be pure and own captured
data. Unrepresentable deadlines or residual hazards fail explicitly. Native APIs
do not infer physical units; general declarative bindings remain separate work.
See [semantics](SEMANTICS.md#scalar-sd-publication-and-typed-raterouting-consumers)
and [M5 tracking](M5_ACCEPTANCE.md).
