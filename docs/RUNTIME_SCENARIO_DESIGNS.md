# Reproducible scenario designs

Experiment files now accept exactly one explicit `scenarios` list or a `design`
object. The new [experiment schema](../ir/schema/ankurafathom-experiment.schema.json)
checks structure; the loader checks declared parameter names, increasing finite
bounds, scenario address capacity and separation of LHS/execution seeds.

```json
{
  "seed": 42,
  "replications": 4,
  "design": {
    "kind": "lhs",
    "count": 1000,
    "design_seed": 73,
    "bounds": {"decay_rate": [0.1, 0.3]}
  }
}
```

All designs sort parameter names lexically and assign increasing scenario IDs
from `first_id` (default zero). Changing first ID changes trajectory addresses but
not design coordinates. There are 1–32 parameters and at most 65,536 scenarios;
the existing runner also bounds total trajectories to 1,000,000.

- `grid`: `axes` maps names to nonempty finite-value arrays. The last lexical axis
  varies fastest. Repeated values are allowed as distinct scenarios.
- `lhs`: `bounds`, `count` and `design_seed` define independent Fisher–Yates
  permutations and addressed open-unit jitter. Every dimension has exactly one
  point in every stratum. Design and effective execution seeds must differ, including
  after `--seed` overrides. Draw counters and unbiased rejection behavior are frozen
  in the semantics document.
- `sobol`: `bounds` and a power-of-two `count` use the first 32 Joe–Kuo D(6)
  dimensions, starting at point zero. There is no scrambling, skipping or thinning
  in this contract. The power-of-two restriction preserves the intended balance
  properties; see [SciPy's Sobol documentation](https://docs.scipy.org/doc/scipy/reference/generated/scipy.stats.qmc.Sobol.html).

The native Sobol implementation derives 32-bit directions from pinned polynomial
and initial-direction rows, then evaluates coordinates using Gray-code XOR. The
[Joe–Kuo notes](https://web.maths.unsw.edu.au/~fkuo/sobol/joe-kuo-notes.pdf) specify the
recurrence. The numeric subset, upstream dataset/subset hashes and BSD license are
stored in `third_party/sobol/`; data came from installed SciPy 1.18.1. Bounds map
normalized values with `std::lerp` under the fixed floating-point policy.

The frozen reference contains 46 cases, 141,916 scenarios and 255,482 coordinates.
Running both axis declaration orders gives **92 configurations and 510,964 native
coordinate comparisons**. Maximum absolute gap is **2.22e-16**. All **1,156 LHS
stratum/mean gates** pass. Sobol is compared to pinned SciPy; LHS uses an independent
integer Philox/permutation implementation, and grids use an independent Cartesian
product. Twelve deliberately corrupted reports reject. Reference artifact hashes
are checked, and pinned regeneration reproduces the data.

Six CLI designs add **15,570 analytic observations**, thread-count agreement,
17 invalid cases (six requiring semantic validation) and a seed-override guard.
The optional schema check confirms each structural/semantic classification.
Normal/sanitizer native reports and trajectories match exactly. Trajectory SHA:
`57b83e2491140b69241b89cb44afe0fef08844d504a1a06cb144697cde20eb95`.

```sh
./build/fathom run models/decay.ir.json --experiment models/decay.lhs.experiment.json --threads 7
ctest --test-dir build -R '^experiment_design' --output-on-failure
python tests/experiment_design_cli.py ./build/fathom --schema
```

This completes the declared bounded design generators. Arrow/Parquet, data
bindings, manifests/replay, C/Python interfaces and general hybrid graph loading
remain M6 work. Runtime dependencies are staged in `.venv-runtime` (PyArrow 25.0.1,
nanobind 3.1.0); no Arrow or Python-binding implementation is claimed yet.
