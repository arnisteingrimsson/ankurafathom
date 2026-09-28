# History-based DELAY N acceptance

The implemented history contract is **second order only**. Native IR uses
`type: "history2", order: 2`; XMILE opts in with `--delayn-policy history2`.
Default imports still require constant DELAYN duration. The existing `cascade`
policy is unchanged.

## Timing, state and conservation

For duration `tau`, initial output `y0` and fixed dt, initialize two signed
stage quantities to `y0 * tau / 2` and the saved duration to `tau`.
Every tick reads the same committed snapshot:

```text
output   = q1 / (saved_duration / 2)
transfer = q0 / (current_duration / 2)
next_q0  = q0 + dt * (input - transfer)
next_q1  = q1 + dt * (transfer - output)
next_saved_duration = current_duration
```

Observing output never advances history. A duration change does not immediately
change visible output. The same output drives the receiving stock and pipeline
balance: `next_pipeline = pipeline + dt * (input - output)`. Both stages and
saved duration commit together; failure preserves all three. Inputs and initial
outputs may be signed, consistent with the historical source. This differs from
the nonnegative-input contract of `type: material`.

Duration must be finite, positive and at least two dt, including the final
observation. dt is fixed. No automatic order reduction occurs. Source order must
be provably constant and exactly 2; all other history orders fail. Duration may
depend on time, stocks and tables, but not delay outputs. Standalone Euler
restrictions, unit checks and scenario initialization apply. See the
[native fixture](../models/history_delay.ir.json).

## Independent evidence

The pinned SDXorg/test-models `tests/delays` model changes duration from 4 to 6
at time 15, changes input from −1 to 4 at time 5, and initializes delay order to
2. Its published Vensim output agrees with a native projection of that DELAY N
component at all **101 observations**, using 1e-5 relative or 5e-6 absolute
tolerance for rounded historical values. Original source, output and README
are vendored and hash-pinned. This is a component projection, not an MDL importer
or a new Vensim run. The README attributes the model/export to Vensim DSS 7.1a;
it labels the export `output.csv`, while the actual pinned artifact is `output.tab`.

A separate authored XMILE fixture has eight second-order calls and a shared
alias. Direct PySD 3.14.3 trajectories provide **1,386 comparisons** across both
source orders (660 stock, 726 auxiliary observations), at 2e-12 relative or
absolute tolerance. Cases include abrupt/gradual changes, stock-driven duration,
feedback, nesting, aliases and default/explicit initialization. Hand calculations
independently check contents, signed balance, boundary timing, overrides,
observation purity and rollback.

## Why other history orders remain rejected

[Vensim's DELAY N documentation](https://www.vensim.com/documentation/fn_delay_n.html)
requires previous-duration output timing and quantity conservation. PySD's
history implementation passes the pinned second-order trajectory but fails
conservation for some other orders. With initial output 2, initial duration 4,
zero input, dt=.125, and duration reduced to 2 at time 1, initial quantity is 8.
Integrating until time 256 gives:

| Order | Quantity emitted | Initial minus emitted (remaining tail negligible) |
|---|---:|---:|
| 1 | 7.8060750301 | +0.1939249699 |
| 2 | 8.0000000000 | approximately 0 |
| 3 | 8.2387619634 | −0.2387619634 |
| 5 | 8.7406697814 | −0.7406697814 |

The report covers two dt values, four orders and three duration schedules:
**12/24 conservation probes pass**. Constant-duration controls and second-order
cases pass; 12 changing-duration cases for orders 1/3/5 fail. This is evidence
about pinned PySD, not a claim that Vensim has the same defect. Matching PySD
alone cannot justify accepting a general material-history kernel.

Another diagnostic records PySD truncating requested duration 4.5 to 4 when
its history array initializes from an integer scalar. The authored conformance
fixture uses `3.0` for its duration-driving stock initialization; native history
always stores double precision. Original upstream files remain unchanged.

## Reproduce and interpret results

```sh
python3 tests/xmile_history_tests.py build/fathom
.venv-sd-oracle/bin/python tests/oracles/xmile/generate_history.py --verify
.venv-sd-oracle/bin/python tests/oracles/xmile/generate_history.py --check-all-orders
```

`--verify` confirms that the pinned report regenerates, including failed cases;
CI uses that command. `--check-all-orders` exits **1** while any conservation
case fails and does not overwrite the report. Passing the regression test means
the bounded kernel and evidence safeguards passed, not all-order acceptance.

Generalization requires authoritative changing-duration reference cases for
orders 1/3/5 and reconciliation with conservation. Off-grid source inputs now
have an explicit [next-tick policy](OFFGRID_INPUTS.md); full history support
remains an open gate.
