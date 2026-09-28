# Explicit next-tick input policy

Importer 0.11 accepts `--input-policy next_tick` for Euler STEP/RAMP/PULSE
schedules between simulation ticks. The default `grid` policy retains its strict
alignment checks. Metadata records `euler_next_tick_quantity_pulse` when the new
policy is used by input functions.

The [XMILE input definitions](https://docs.oasis-open.org/xmile/xmile/v1.0/os/xmile-v1.0-os.html)
specify arguments and pulse quantity but do not prescribe a grid-assignment rule
for off-grid events. This option is an explicit Fathom sampling contract; it is
not a claim that every Stella, Vensim or PySD version makes the same assignment.
Native functions and existing strict-grid imports keep their original semantics.

## Rules

For simulation origin `a`, dt `h` and tick `k`, observation time is `a + k*h`.

| Source expression | Sampled behavior |
|---|---|
| STEP(height, first) | Zero before first; height at the first tick on or after first and thereafter |
| RAMP(slope, first) | `slope * max(0, time - first)`; retains fractional elapsed time |
| PULSE(quantity, first) | One pulse, activated at the first tick on or after first |
| PULSE(quantity, first, interval) | Zero interval means once; positive interval schedules events at `first + n*interval`, n ≥ 0 |

At a pulse activation tick, output is `event_count * quantity / dt`. Multiple
events assigned to the same tick each contribute their quantity, including when
interval is shorter than dt. The quantity expression is evaluated once from
the committed tick state, not separately at the nominal event times.

Pulses activate for events in `(time-dt, time]`. This interval includes its right
boundary and excludes its left. A schedule may begin before model start: events
in that first window activate at tick zero, but older events are not replayed.
For example, with start=10, dt=.5, first=10.125 and interval=.25, the two events
at 10.125 and 10.375 activate together at 10.5.

Euler integrates the sampled rate over the following step. In that example,
their quantity is visible in the receiving stock at 11.0. A rate visible only at
the final observation does not integrate beyond the horizon. This is a sampled
SD input, not a DES event delivered at its original timestamp.

STEP heights, RAMP slopes and PULSE quantities may depend on committed state.
Source first times and intervals must remain constant expressions, including
constant auxiliary/table expressions. Stock and default delay initialization
use the same policy at the declared source start. Delays continue to update from
one shared snapshot. RK4 source calls, RK4 flows and hybrid flows reject these
tick-dependent functions.

## Native expressions and numerical bounds

The importer lowers to pure functions with an explicit schedule origin:

```text
XMILE_NEXT_STEP(height, first, origin)
XMILE_NEXT_RAMP(slope, first, origin)
XMILE_NEXT_PULSE(quantity, first, interval, origin)
```

The origin must align with observation times. The importer supplies the model
start; native callers may provide a time-valued parameter. All functions inject
the usual `t` and `dt` dependencies. STEP preserves height units, RAMP multiplies
slope units by time, and PULSE divides quantity units by time. Every schedule
argument has model-time dimensions. See [native fixture](../models/offgrid_inputs.ir.json).

Adjacent pulse windows are computed from integer ticks relative to the same
origin. Counts are differences of cumulative event counts at those boundaries,
so clock subtraction cannot duplicate or lose an event between adjacent windows.
No growing list of pending events is needed.

Source first times are bounded to ±1,000,000 ticks from model start. Positive
interval/dt must lie in [1e-6, 1,000,000]; native relative offsets and cumulative
event-index arguments must remain within ±2,000,000. Import checks the endpoints
of the entire run. Runtime calls check their current window, including after
parameter overrides. These bounds reject unresolved schedules rather than
silently skipping events.

Values within eight machine epsilons of an integer, scaled by relative count
and the fixed origin/first clock scale, snap to that integer. The allowance may
not exceed 1e-6 in the normalized quantity. Clocks with indistinguishable ticks,
nonfinite arguments, excessive indices, negative intervals and output overflow
fail. First-time normalization and boundary-count rounding use the same scale
for a shared boundary in both adjacent windows.

## Evidence and reproduction

`generate_inputs.py` independently enumerates each scheduled event using exact
Python fractions, assigns it with rational ceiling, and integrates Euler stocks
with rational arithmetic. It neither imports Fathom nor uses the runtime's
cumulative-floor counting algorithm. The saved reference contains source XML,
source and generator hashes, and complete rate/stock trajectories.

There are 288 cases: dt .1/.125/.5/1, origins 0/10/−2, starts a quarter tick
before or .25/.5/.75 tick after origin, and one-shot/.5/1.5/2.25-tick pulse
intervals. All 17 observations of rates and stocks pass in both source orders:
**19,584 comparisons**, at 2e-12 relative or absolute tolerance. These are
contract-oracle comparisons, not historical commercial-tool matches.

Additional native and source tests cover exact and neighboring boundaries,
documented roundoff snapping, long decimal-clock schedules, repeated reads,
dynamic quantities, signed values, initialization, delay composition, scenario
overrides, units, reserved identifiers, failure diagnostics and the final tick.

```sh
python3 tests/oracles/xmile/generate_inputs.py --verify
python3 tests/xmile_input_tests.py build/fathom
./build/fathom run models/offgrid_inputs.ir.json
```

The original 67-file XMILE corpus remains unchanged. This optional timing policy
does not change its coverage denominator or close the outstanding general-history
DELAY N acceptance gate.
