# M6 runtime acceptance audit

M6 remains **in progress**. This audit separates implemented, locally tested
contracts from unresolved milestone requirements. It does not close M2's source
history gaps or M5's general declarative hybrid graph work.

| Gate in the implementation plan | Current evidence and scope | Status |
|---|---|---|
| 1,000 scenarios, identical ordered results across thread counts | Local ensemble tests; decoded Arrow/Parquet comparisons at 1/8/64 workers; ordered cancellation/progress contracts | Local pass |
| Cross-platform numeric policy and comparison | Linux/macOS CI jobs exist, but do not exchange results for a numerical comparison; remote CI has not run | Open |
| Reproduce recorded numerical results | Strict same-build file manifests 0.1/0.2, relocated bundles, input hashes and ordered result digest; C/Python memory manifests 0.3 are recorded but not replayed | Bounded local pass |
| Data binding identities | Raw-file SHA-256 changes with bytes; canonical hash reflects validated typed values in canonical key order. Row reordering can change raw hash while leaving canonical hash unchanged | Local pass |
| `population_init`, `exogenous_series`, `parameter_table` | Three native adapters and bounded declarative integration; composition matrix below | Partial |
| CLI, C ABI, Python | Installed C11 consumer, Python/C parity, Arrow ownership, callbacks, provenance and native prefix relocation on macOS/arm64 | Local pass; other platforms open |
| Editable Python installation | Root `pip install -e .` in the prepared local environment; isolated Python invocation outside the checkout verifies source redirect, 11 analytic values, three thread counts and five expression-limit controls | Local pass; remote qualification open |
| ASan/UBSan and nightly TSan | Instrumented focused regressions and earlier full-suite checkpoints; TSan ensemble test; CI now has a daily schedule | Local evidence; remote schedule unverified |
| IR-loader and expression fuzzing, ten minutes each nightly | Loader completed 3,882,096 executions in 602.067 seconds. Expression campaign exposed a harness omission for documented arithmetic-overflow exceptions; corrected and seeded. A fresh full expression campaign is still required | Partial; nightly CI unverified |
| Property/invariant coverage | Existing generated conservation, non-negativity, ordering and lifecycle tests; independent oracle contracts | Implemented for documented model subsets |
| Full event-trace CLI from §6.7 | Native conformance tests have event traces; a general `fathom trace` interface is not implemented | Open |
| `fathom xmile2ir` CLI from §6.7 | XMILE source translator/reference workflow exists separately; named CLI subcommand is not implemented | Open |

## Data-binding composition

| Model path | Supported bindings | Remaining scope |
|---|---|---|
| Standalone SD | Multiple keyed parameter tables and multiple hold/linear series together; unit checks, duplicate-target rejection, scenario override precedence, immutable snapshots | Broader model modes |
| Typed ABM | One population table combined with multiple parameter tables, or parameter tables with inline agents; sync/async, order-independent defaults, overrides and source receipts | Multiple populations; series alongside population data |
| DES and hybrid modes | Native data adapters can be used by C++ callers | Declarative bindings and shared graph resolution |
| Memory JSON loading | File bindings require an explicit absolute base directory | URI providers and decoded-memory budgets |

Series functions interpolate the owned snapshot at the requested time, including
RK4 stage times. A change to grid-only presampling must preserve those semantics;
the plan's original presampling wording is not an acceptance claim for the current
implementation. `entity_replay`, `calibration_target` and `check --fit` belong to M7.

The next acceptance work should establish cross-platform comparison policy and
evidence, qualify installation on other platforms, and extend binding composition with
explicit schema/unit/provenance contracts. General graph work and missing CLI
interfaces must either be completed or explicitly scoped before declaring M6 done.
The [synthetic economics preview](../examples/ankura_pilot/README.md) now exercises
the platform end to end while these gates and M7 validation/reporting remain open.
See [status](STATUS.md) and [the session evidence](SESSION_2026_09_27.md) for measured
test counts; no new full-suite result is implied by this audit.
