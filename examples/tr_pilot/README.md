# Synthetic T&R monthly pilot

This example extends the earlier [AI mechanics fixture](../ai_decision/README.md)
with a **150-person initial practice: 135 billable employees and 15 support staff**.
Seven billable levels have different rates, pay, task requirements and AI exposure.
The distribution and all commercial assumptions are illustrative, not Ankura data.
The model is a bounded implementation of the [pilot specification](../../docs/TR_PILOT_SPEC.md).

[Recorded results and verification](RESULTS.md) cover the September 30 run.

## Initial synthetic assumptions

| Level | Employees | Standard hourly rate | Annual base pay | AI-exposed task share |
|---|---:|---:|---:|---:|
| Analyst | 18 | $250 | $85,000 | 80% |
| Consultant | 27 | $350 | $110,000 | 70% |
| Senior consultant | 30 | $450 | $145,000 | 60% |
| Manager | 24 | $600 | $185,000 | 40% |
| Director | 18 | $800 | $240,000 | 20% |
| Senior director | 12 | $1,100 | $320,000 | 10% |
| Managing director | 6 | $1,400 | $450,000 | 0% |
| Practice support | 15 | — | $80,000 | 0% |

Employer load is 25% of base and bonus accrual 20%. Fixed allocated overhead is
$500,000/month plus 5% of revenue. Annual attrition is 10%; the hold policy replaces
exits immediately. Paid hours are 160 per FTE/month. These values are test inputs;
the rates have not been extracted from Chapter 11 fee applications. Full settings
and task-hour requirements are in [config.py](config.py).

## Run and inspect

From the repository root, with the Arrow-enabled native executable built:

```sh
.venv-runtime/bin/python examples/tr_pilot/run.py \
  --out artifacts/tr-pilot-new
ctest --test-dir build-arrow -R '^tr_pilot_' --output-on-failure
```

The default runs 34 five-year scenarios, plus a separate three-month observed
bankruptcy-input demonstration. Scenarios cover 20%/40% eligible-task savings,
three workforce policies, price pass-through, constrained new fixed-fee work,
business development, market shocks and pipeline uplift. `--executable` selects
the native binary; `--threads` defaults to 8 within each batch; `--workers` defaults
to 3 concurrent batches. Batches contain at most three scenarios to bound memory
used by repeated native result provenance. Scenario IDs are local to each batch;
the root `scenarios.json` and batch order define the global reporting order.

Use `--resume` with the same directory after an interruption. It compares newly
generated model/input bytes and scenario definitions before reusing completed
results, verifies saved results, and repeats the independent comparison and replay.
The program refuses changed inputs; it does not silently accept an old run.

Each output directory retains:

- `operational-data/`: seeded Parquet exports, plus a separate validation-only truth file.
- `calibration.json`: estimates as of January 1, 2025, uncertainty, diagnostics and source hashes.
- `config.json`, `scenarios.json`, `sources.json`: effective assumptions and provenance.
- `batch-*/`: typed native inputs, full results, declared-check receipts, manifest,
  independent comparison receipt, and a portable input/replay bundle.
- `annual-summary.json` and `summary.txt`: practice and level metrics.
- `validation.json`: combined evidence and script hashes.

## What runs in the engine

Python generates inputs and native SD equations. **AnkuraFathom's C++ CPU engine
calculates every monthly trajectory** using a one-month Euler step with explicit
next-state equations. The example does not use a Python forward simulation as
its production engine. A separate imperative Decimal ledger implements the same
published ordering for independent comparison of every public output at every
month, including the initial state.

The initial roster contains integer employees. Subsequent workforce quantities
are **expected FTE**, not simulated fractional people or stochastic employee
histories. Delivery is likewise a fluid number of equivalent engagements/service
units with a fixed required skill mix. A missing required senior skill can block
all delivery. There is no substitution of junior time for protected senior work.

The monthly order is: read delayed inputs; receive prospects and expected wins;
apply exits/promotions and previously queued hires; account for paid time,
non-delivery time, AI training and onboarding; allocate eligible surplus time to
BD; deliver skill-constrained work; post earned fees and costs; advance approval,
holdback and collection queues; commit staffing requests for future months.
New demand may be served in its arrival month; no engagement-duration model is
implied. Queue lag 2 sends a month-1 input to month 3.

### Costs and workforce

Salary, employer load and bonus accrual apply to billable and support staff.
Support compensation is included once and reported separately. Allocated overhead
has a fixed component and a revenue-linked component. Variable delivery costs,
recruiting, AI licenses and AI training cash costs are separate. Training and
onboarding also consume time, without subtracting their salaried time twice.

Annual attrition is converted with `1-(1-p)^(1/12)`. Policies are:

- `hold`: immediate replacement of exits, with recruiting expense; an explicit
  constant-workforce control without replacement onboarding delay.
- `freeze`: no new hires; exits reduce payroll and delivery capacity.
- `responsive`: capped hiring requests based on forecast won work, backlog and
  target utilization, after accounting for queued starts. Hires incur a lag and
  first-month onboarding loss. There are no involuntary layoffs.

Promotions transfer expected FTE to the adjacent billable level; the default
promotion rate is zero. Frozen requisitions, offer acceptance/cancellation,
individual tenure, differential attrition after AI and employee part-time/FTE
changes are outside this subset.

### AI, contracts and cash

Each level has a share of task hours exposed to AI. A six-month adoption ramp
multiplies scenario savings; rework adds back a fraction of removed hours. MD
exposure is zero in the synthetic configuration. "20% AI" means **20% savings on
eligible tasks**, not a uniform 20% cut in the practice's total delivery hours.

Hourly revenue uses actual delivered hours at level-specific rates and
realization. Non-hourly contracts have separate backlog/value balances. Fixed-fee
price erosion applies only when new work is booked: old backlog keeps its value.
The erosion factor passes through a fraction of the reduction in the job's
hourly-equivalent billed value, using its level rates and realization.
The default fixed-fee share is 15%, with a 25% ceiling on incoming work. This
ceiling is a policy assumption, not a claim about legal or commercial availability.

Earned monthly CRO/retainer work is represented by a fee per delivered service
unit, separate from advance deposits. Success work earns a probability-weighted
fee per completed unit. The success gate can eliminate new success fees; it is
**not a stochastic success-event or contingent revenue-recognition implementation**.
Approval and payment terms are defined by fee category; mixed contracts and
case-specific court orders require further work.

Revenue is earned on service delivery, with disallowance recognized when approval
occurs. Approval delay, holdback release and collection delay have separate
registers. Delay alone does not reduce revenue. Fixed advances increase cash and
a deposit liability; service delivery releases that liability. Initial fixed
backlog is assumed funded by its configured advance before the simulation.
This is an explicit simulation convention, not accounting advice or an assertion
of Ankura's recognition policy.

EBITDA subtracts all modeled operating costs from net revenue. Operating cash
subtracts those same costs (assumed paid currently) from collections. Financing,
tax, capex, collections default, terminal receivables recovery and acquisition
funding are absent. **No new valuation or acquisition recommendation is produced.**
The earlier acquisition fixture remains available, with its original limitations.

### Growth feedback and external demand

Eligible senior surplus hours can go into BD; those hours cannot simultaneously
serve clients. A capped conversion produces additional prospects after a lag.
Conversion, eligible shares and lags are synthetic; existing surplus can generate
BD even without AI. Matched no-AI BD controls separate that effect from AI.

The five-year market path is a synthetic shock scenario. Separately, observed US
Courts business Chapter 11 starts of **727, 760 and 895** in April–June 2026 are
normalized to April and passed through the native demand input. The source table,
pages and vintage are in [sources.json](sources.json). The elasticity is uncalibrated;
this demonstrates an external-data connection, not a demand forecast or historical
validation against a consulting practice. Filing counts include case composition
that may not match addressable T&R demand.

## Operational calibration

The seeded generator emits Accounts, Opportunities, dated opportunity history,
a 150-person roster and weekly timesheets for three years. Arrival counts vary,
amounts vary, outcomes are sampled, 10% of opportunities never close, some close
dates slip, CRM probabilities are stale and some fee types are missing.

Calibration uses only records available before January 1, 2025. It estimates
opportunity arrivals, mature-cohort wins, hourly realization, level counts, pay and
standard rates. These estimates feed native model parameters. It ignores current
snapshot stages/probabilities when reconstructing historical outcomes. Missing
fees are counted; fee-mix estimation is withheld. AI, BD, workforce and commercial
policies remain explicit assumptions, not estimates from the synthetic data.
CRM and timesheets are separate adapter fixtures; they are not a reconciled
engagement-level operating history. The new example makes no historical revenue
reconstruction claim. The earlier clean fixture retains that narrower accounting
test separately.

"Win rate" here is wins divided by all opportunities in cohorts at least 183 days
old, including permanently open cases. It differs from a closed-only CRM win rate.
A Wilson interval expresses sampling uncertainty under the fixture's independent,
stationary assumptions. Historical utilization is reported as a diagnostic; task
requirements are not fitted to force the forecast to reproduce it.

The adapter rejects duplicate IDs/timesheets, absent stage history, unknown joins,
invalid time quantities and unsupported changing-roster/part-time histories. The
fixture uses one paid-hours record per employee/week/project; it is not a general
multi-project payroll reconciliation adapter. Data quality checks do not establish
that a real Salesforce or HR export is compatible without mapping and validation.

## Evidence and boundaries

Native fixtures exercise fully loaded costs, skill bottlenecks, junior-hour
composition, attrition, hiring/onboarding lags, promotions, deposits, price erosion,
holdbacks, disallowance, retainer/success semantics, BD/macro lags and zero-task AI.
A deliberately wrong hourly formula passes accounting identities but fails the
independent oracle. Resume refuses changed inputs. Data tests cover noisy recovery,
future/truth leakage, malformed exports, reproducible generation and a later
synthetic cohort checked with a declared sampling tolerance.

Each experiment compares all public monthly quantities with the Decimal ledger
at `abs_tol=2e-6`, `rel_tol=2e-10`. Native declared checks reconcile exclusive time,
workforce, backlog, fees, costs and cash/receivables/deposits. Native validation and
saved-result identities must match. For the five-year scenarios, replay at one and eight threads is exact;
bundles retain file-bound parameters and external time series.

FTI Q2 2026 Corporate Finance KPIs are **peer comparisons only**: $553 average
billable rate, 59% utilization and 20.9% adjusted segment EBITDA. Segment scope,
utilization denominator and cost allocation differ. They did not set synthetic
rates or force a target margin. No Ankura business parameter is calibrated from
real data, and no real-outcome backtest or sponsor acceptance is claimed.

Remaining gates include real fee-application extraction, agreed staffing/cost
inputs, integer-person stochastic dynamics, contract-level success events and
service durations, estimated BD/macro relationships, acquisition economics, and
historical validation against actual practice outcomes. See the specification
for those acceptance requirements.
