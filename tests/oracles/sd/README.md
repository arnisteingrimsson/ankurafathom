# Independent SD references

`generate.py` implements ODE right-hand sides independently from the C++ stock/flow
models. It generates `reference.json` with pinned NumPy 2.4.3 and SciPy 1.17.1;
no C++ execution or native trajectory is used to produce the oracle.

The primary solver is `solve_ivp` with DOP853, relative tolerance `2e-13`, absolute
tolerance `2e-15`, and maximum step `1/16`. Each trajectory is checked against:

- DOP853 with tighter tolerances (`4e-14`, `4e-16`) and maximum step `1/32`.
- Radau with tolerances (`2e-13`, `2e-15`) and maximum step `1/32`.
- Closed forms for Bass, logistic, the oscillator, and time-dependent growth.
- Population conservation/positivity for SIR and the first integral for Lotka–Volterra.

All cross-solver and analytic differences must remain below `2e-11` in absolute
state units. Solver agreement is a numerical cross-check, not a rigorous error
bound. DOP853 and Radau use different integration methods but share the Python
right-hand-side definition; analytic solutions and invariants provide additional
checks of that definition. See the [SciPy solve_ivp documentation](https://docs.scipy.org/doc/scipy/reference/generated/scipy.integrate.solve_ivp.html)
for solver and tolerance semantics.

The artifact stores model parameters, initial states, stock order, observation
times, scaling constants, solver settings, package versions, generator hash, and
observed reference discrepancies. Ordinary CTest consumes this file without
requiring Python or network access. Both configured CPU CI platforms additionally
regenerate and compare references, allowing `2e-12` absolute difference for
platform rounding. The generator hash, specifications, solver settings, and
package versions must match. It does not overwrite data in `--verify` mode.

From the project root:

```sh
python3 -m venv .venv-sd-oracle
.venv-sd-oracle/bin/python -m pip install -r tests/oracles/sd/requirements.txt
.venv-sd-oracle/bin/python tests/oracles/sd/generate.py --verify
```

To deliberately replace the reference after a reviewed generator/specification
change, omit `--verify`, review the artifact diff, and run the convergence and
full correctness suites. There is no silent reference update in CI.

Model equations, native acceptance gates, and the current evidence are in
[SD_VALIDATION.md](../../../docs/SD_VALIDATION.md).
