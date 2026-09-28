# Native declared validation

`fathom check model.json [--experiment experiment.json] [--threads N] [--seed S]
[--out validation.json]` loads the model with the normal parser, units, references
and immutable data bindings, then checks every requested standalone SD trajectory.
It emits a JSON report on stdout; `--out` also saves it atomically to a new path.
A failure exits 1; pass or warning exits 0. Invalid CLI options use the existing
stderr diagnostic contract. Model/load/runtime failures appear in the report.

This is the first bounded M7 validation increment. A **pass** means the declared
checks hold at the sampled output times for this exact input and experiment. It
is not scientific acceptance of the model or the complete M7 gate. A model with
no declarations receives **warn**, never an implicit behavioral pass. Reports
name unassessed work: automatic extremes, integration error estimates, Monte
Carlo standard errors, fit, sensitivity, hybrid behavior and broader structural
inventories. Deterministic scenarios do not establish confidence intervals.

## Declarations

Standalone SD models may include a nonempty `checks` array (up to 1,024 entries):

```json
"checks": [
  {"id": "mass", "kind": "conserved", "stocks": ["a", "b"]},
  {"id": "range", "kind": "bounds", "output": "b_ts", "min": 0, "max": 10},
  {"id": "growth", "kind": "monotone", "output": "b_ts", "direction": "increasing"},
  {"id": "balance", "kind": "assert", "expr": "a+b == total", "when": "always"}
]
```

- Conservation compares the sum of distinct, equally dimensioned stocks with
  that trajectory's initial sum. It tests the declared balance; it does not infer
  whether the flow graph is closed. Each scenario/replication has its own baseline.
- Bounds use declared output IDs and at least one of `min`/`max`, interpreted in
  the output's units. Endpoints are inclusive.
- Monotonicity uses consecutive output samples and `increasing` or `decreasing`,
  both nonstrict. The initial sample establishes the reference, not a comparison.
- Assertions contain one `<=`, `>=` or `==` comparison between arithmetic
  expressions. They can read model parameters, stocks, auxiliaries, delays,
  `t`, `dt`, tables and exogenous series. Output aliases and flow IDs are not
  expression symbols. Both sides must have equal dimensions: write `a/total >= 0`
  to compare a dimensional stock with zero, or use an output bound. Compound
  predicates and strict inequalities are not implemented. `when` defaults to
  `always`; `end` evaluates only at the final sample, including lazy expression
  branches. All expressions remain statically checked, even unused branches.

Each declaration may have an `id` (defaults to `check_N`, unique within checks),
`absolute_tolerance` and `relative_tolerance` (finite, nonnegative, both default
zero). Equality accepts `abs(a-b) <= atol + rtol*max(abs(a),abs(b))`; inclusive
comparisons accept exact ordering or that equality tolerance. Absolute tolerance
is in the comparison's units. Relative tolerance is dimensionless. Conservation
uses an ordered extended-precision sum converted to float64 before comparison.
Tolerance arithmetic is normalized by the largest operand/absolute-tolerance
magnitude before subtraction and multiplication, preventing finite large values
from producing an infinity-versus-infinity false pass. No hidden tolerance is applied. This remains local CPU qualification.

Checks run on the model's output grid, including initial/final times; they do not
claim to inspect between steps or at every RK4 stage. Auxiliary values use the
same runtime evaluator as ordinary outputs. Observation helpers are private:
they do not change the saved output set, declared IR identity or result hash.
An end-only assertion is not evaluated at earlier times.

## Reports and run gating

The [report schema](../ir/schema/validation_report.schema.json) records the scope,
sampling policy, per-check code/pointer, sample and failure counts, and first
failure's scenario, replication, time, actual and reference values. Arithmetic
errors have `CHECK_EVALUATION` and a `first_error` with the sample location.
Execution failures stop behavioral checking and explicitly mark it incomplete;
partial trajectories are not published as a successful result. The earliest
reported trajectory is stable in scenario/replication order across worker counts.

For successful execution, `inputs` and `result` use the same native provenance
identities as `fathom run`. The pilot runner requires equality of both before
presenting its report. A structural load failure has no completed input snapshot;
its report has a failure code/pointer and `result: null`.

`fathom run model.json --require-check ...` recomputes checks using the same loaded
model, captured data, scenario overrides and seed before emitting observations
or a manifest. It requires **pass** (both fail and warn prevent publication).
It accepts no stale cached verdict and does not apply to replay. The gate does
not yet embed a validation receipt in the run manifest; use `check --out` to
retain one and compare identities, as the pilot runner does. This implementation
executes the ensemble twice; correctness currently takes priority over speed.

Ordinary `run` preserves its existing behavior; declaration syntax is validated
at load, but behavioral rules execute only for `check` or `--require-check`.
C/Python loading accepts the same declarations; a public C/Python check function
is not included yet. Python-only pilot runs explicitly say validation was not run.

See [the executable transfer example](../models/validation_transfer.ir.json),
[validation tests](../tests/validation/declared_checks.py), and
[pilot workflow](agent/README.md). M7/M8 acceptance remains open.
