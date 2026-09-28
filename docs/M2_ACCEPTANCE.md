# M2 acceptance review

M2 remains **in progress**. The review of §7.3 now records **three validation
areas passing and one partially satisfied**. These are validation areas within
M2, not Phase 1 milestones or a percentage of implementation effort.

| Requirement from the implementation plan | Status | Evidence and scope |
|---|---|---|
| Analytic growth/decay, logistic, damped oscillator and first-/third-order delays: discrete solutions and continuous convergence | Pass for the documented cases | `sd_analytic`: 56 new convergence trajectories, 16 delay boundary trajectories, 45 logistic discrete observations. `sd_nonlinear`: logistic and oscillator continuous convergence plus oscillator Euler closed form. |
| SIR, Lotka–Volterra and Bass against independent high-accuracy references, including Euler/RK4 orders | Pass | `sd_nonlinear`: pinned direct-ODE SciPy references, independently checked with tighter DOP853/Radau and available closed forms; 72 trajectories across six models and three methods. |
| At least 80% of supported, assessable pinned corpus within tolerance | Pass under eligibility policy v2 | 8/8 eligible files import; 7/8 (87.5%) have compatible complete historical comparisons. Adding STEP/RAMP/PULSE to the reviewed function list leaves these counts unchanged. One RK4/Euler conflict remains excluded; 49 malformed files remain unassessable. This is not 87.5% of all 67 files. |
| Named function semantics and initialization, including the exact-function requirement in §6.3 | Partial | Cascade imports have 1,386 independent explicit-stock observations. Second-order history adds 101 historical Vensim observations and 1,386 PySD comparisons with conservation checks. Grid-aligned inputs have complete hand trajectories; an explicit off-grid next-tick policy adds 19,584 rational-oracle comparisons. General history orders remain unresolved, and universal vendor off-grid timing is not claimed. |

No acceptance criterion has been removed to close M2. The numerical analytic
and corpus gates are now satisfied, but passing a bounded function subset does
not establish the complete source-function contract requested in §6.3.

## Function contract still to resolve

The explicit `--delayn-policy cascade` contract is implemented and checked
against an independently expanded stock-and-flow PySD model. Tests cover abrupt
and gradual duration changes, shared old stages, material conservation,
initialization, feedback, nested calls, aliases, parameter overrides and failure
isolation. Default imports still require constant duration. The saved unexpanded
PySD DELAYN trajectory demonstrates the dialect difference; it is diagnostic
evidence, not a matching reference.

Source STEP/RAMP/PULSE mappings are implemented for constant, grid-aligned
schedules under Euler. Their native names, argument meanings and units are
explicitly separate from the existing native functions. Tests cover signed
quantities, four dt values, three absolute start times, initialization, final
observations and invalid schedules. See the [import contract](XMILE_IMPORT.md).

The bounded second-order history contract is now implemented as native
`history2` and explicit `--delayn-policy history2`. A separate historical Vensim
component projection, PySD source trajectories and independent quantity balance
support it. The two stages and saved duration commit together; signed inputs,
initialization, overrides and failure isolation are covered.

General history orders remain rejected. The pinned PySD implementation fails
12 of 24 conservation probes: changing-duration cases for orders 1/3/5. Its
second-order and constant-duration controls pass. There is also a recorded
integer-duration truncation diagnostic. These failures are not production
kernel passes and do not establish a Vensim defect. See the
[history evidence and reproduction commands](DELAY_HISTORY.md).

Off-grid inputs are now implemented under explicit `--input-policy next_tick`,
with multiple-pulse quantity conservation, initialization and shared-state
tests. This is a declared sampling policy, not inferred vendor behavior;
see [contract and evidence](OFFGRID_INPUTS.md).

General history support still requires authoritative changing-duration references
for other orders and reconciliation with conservation. M2 remains open; §6.3 has
not been narrowed to close it. Independent Phase 1 work can advance to DES finite
queues and deterministic priorities while that evidence gap remains explicit.

## Follow-on work that does not reopen the completed analytic gate

Applied references to stock-limited outflows, cyclic clipping dependencies,
delay-output-dependent durations, fractional fixed lags, RK4 with delays/tick
inputs, and hybrid clipping remain explicit unsupported cases. They are not
claimed by the new analytic suite. Broader source dialects and malformed-corpus
repair require a separate scope decision and provenance; original upstream
files have not been rewritten to increase coverage.

## Reproduction

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
python3 tests/oracles/xmile/audit.py --verify
```

Local normal and ASan/UBSan suites pass **74/74 tests**. Schema checks pass 28 valid
fixtures, 95 structural and 55 semantic invalid mutations. The analytic test writes
`build/sd-analytic.csv`; [saved results](../tests/sd/analytic-baseline.csv) and
[metadata](../tests/sd/analytic-baseline.metadata.json) record the observed errors,
compiler flags and source hashes. The numerical gates, not baseline byte
identity, apply on other platforms. CI includes the new target through CTest;
no remote CI run is claimed.
