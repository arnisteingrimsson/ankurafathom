# Synthetic T&R AI decision case

This example generates three years of operational exports, estimates parameters
without reading planted truth, and runs a five-year decision experiment through
the existing Arrow-enabled native SD runtime. The first practice is **synthetic
T&R**, following the user's practice context. None of the rates, fee mixes,
headcounts, conversion rates or intervention effects are estimates about Ankura.
No engine changes are required.

The [T&R pilot specification](../../docs/TR_PILOT_SPEC.md) reviews the remaining
economic limitations and defines staged acceptance cases for level-specific
staffing/costs, workforce policy, task-specific AI, contract/cash mechanics,
business development, external demand and imperfect data. These are proposed
extensions, not features implemented by the current example. The current 76%
fixed-fee result is an unconstrained mechanics sensitivity, not an actionable
T&R pricing recommendation; the reported contribution margin is not EBITDA.

The current user-specified T&R case has **150 employees**, represented as 150
full-time paid staff. Run it with the checked-in configuration below. Potential
pipeline is scaled to 30,000 baseline hours/month to preserve the previous
workload per employee; 50% win rate gives 15,000 won hours/month. The acquisition
still adds 15% workforce (22.5 FTE of capacity, not an integer employee count) and
20% pipeline, with purchase/integration costs scaled 7.5-fold to $15 million and
$150,000/month. These are explicit proportional
scaling assumptions, not FTI or Ankura datapoints. Per-person pay, rates, AI costs,
fee mix and seasonality retain their prior synthetic values.

The completed [150-person results](../../artifacts/ai-decision-tr-150-20260930/results.txt)
and [validation](../../artifacts/ai-decision-tr-150-20260930/validation.json) pass all
983 scenarios, 719,556 independent forecast comparisons, historical reconciliation
and exact 1/8-thread and portable-bundle replay. The synthetic baseline has
$41.22 million annual revenue and 62.5% utilization. All previously reported
percentage thresholds are unchanged. The
[size comparison](../../artifacts/ai-decision-tr-150-20260930/scaling-validation.json)
checks 36,371 annual/summary values against proportional scaling of the 20-person
reference. NPV scaling agrees within $0.001; its comparison uses a one-cent
absolute tolerance because net NPV subtracts large earnings totals. Engine/oracle
tolerances remain unchanged. The intervening 200-person attempt was interrupted
when the user corrected the count; it is not a completed result.

```sh
.venv-runtime/bin/python examples/ai_decision/run.py artifacts/my-tr-150 \
  --config examples/ai_decision/tr-150.json \
  --assumptions examples/ai_decision/tr-150-assumptions.json \
  --executable build-arrow/fathom --threads 8
```

For the original 20-person reference fixture, run from the repository root;
the result directory must be new:

```sh
.venv-runtime/bin/python examples/ai_decision/test_data.py
.venv-runtime/bin/python examples/ai_decision/run.py artifacts/my-ai-decision \
  --executable build-arrow/fathom --threads 8
```

Read `results.txt`, `thresholds.json`, `fee-thresholds.json`, `calibration.json`, `assumptions.json` and
`validation.json`. `annual.parquet` contains all annual results; `forecast.parquet`
contains native monthly stock observations and provenance. `summary.json` includes
the five-year operating-contribution NPV proxy and sustained payback. The output
directory is intentionally under the existing ignored `artifacts/` convention.

The September 30 local [completed run](../../artifacts/ai-decision-tr-validated-20260930/results.txt)
passes 983 scenarios and 719,556 independent forecast comparisons, 444 historical
stock comparisons and 72 historical transaction checks. Its
[validation receipt](../../artifacts/ai-decision-tr-validated-20260930/validation.json)
also records 55 recovered planted values, 773,621 native forecast rule evaluations,
the wrong-T&M negative control, and exact 1/8-thread and portable-bundle replay.
Nine adapter tests and three generated-model JSON Schema checks pass. This is
focused normal-build evidence, not a new full regression or sanitizer claim.

## Decision and baseline

For a 10%, 20% or 30% reduction in actual delivery hours at full adoption, what
increase in opportunity-flow baseline hours, or change in fixed-fee work share,
preserves **both annual revenue and operating-contribution margin**? The scenario
grid tests 0–60% pipeline uplift in 1 percentage point increments, at fixed-fee
shares of 20%, 40%, 60% and 100%. Thresholds are the first passing tested point,
not a continuous optimization result. They are computed separately for years
1, 2 and 5, including rollout/training in year 1. A separate zero-growth sweep
tests fixed-fee work shares from the starting mix to 100% in 1 point increments.
A missing threshold means no
tested point passes, not universal infeasibility.

Default history is 2023–2025, with 20 FTE, 160 paid hours/person/month and an 80%
delivery ceiling. Each level has ten people; illustrative bill rates are $200 and
$300, and salaries $90,000 and $150,000. Equal delivery allocations give a $250
gross blended rate. Planted T&M realization is 90% ($225/hour), fixed-fee value is
98% ($245/baseline hour), and fixed-fee work accounts for 20% of baseline hours.
Annual closed-cohort win rates increase from 30% to 40% to 50%. July/August
opportunity hours are 75% of the annual monthly average; other months are 105%.
Latest annual average prospective work is 4,000 baseline hours/month, yielding
2,000 won hours/month. No historical work is capacity constrained or backlogged.

Future win rate is held at the latest observed year; seasonality repeats. There
is no extrapolated upward win-rate trend. Pipeline uplift is a step increase from
month 1, **not compound annual growth**, and costs an assumed $2 per extra
prospective baseline hour. Fixed-fee share is a work-hour fraction, not revenue
share. Each fee type retains its own estimated realized rate. AI ramps from no
effect in month 1 to its full effect in month 7, with assumed $40/FTE/month licenses
and $300/FTE initial training. These costs are held fixed across AI effect levels.
Paid payroll does not decline when hours are saved.

Acquisition is compared alone and with each AI effect. It adds 15% headcount and
20% opportunity flow in month 13, costs $2 million then, and incurs $20,000/month
integration expense for 18 months. These assumptions are inherited from the
synthetic preview, not a valuation of an actual target. The 10% discounted NPV
uses incremental operating contribution, includes those costs, and excludes
terminal value, financing, working capital, tax and cash-collection effects.

## Operational exports and calibration

`data/` contains Parquet tables for accounts, a roster, opportunities, stage
history, projects, timesheets and paid/delivery availability. IDs and explicit
foreign keys connect the tables. Opportunity fields resemble Salesforce exports
(`Id`, `AccountId`, `StageName`, `Amount`, `CloseDate`, `Probability` in percent), with
fixture-specific `Practice`, `FeeType` and `BaselineHours` fields. This is an
adapter contract, not a promise that an unmodified Salesforce export will load.

Weekly timesheets use month-contained service periods beginning on days
1/8/15/22/29, not ISO weeks. Hours reconcile to paid availability. All employees
work across won projects; nonbillable time fills the remaining paid hours. Dates
are ISO strings. Empty project IDs mark nonbillable entries, and an empty
termination date marks an active employee. `RecognizedAmount` allocates project
revenue to delivery rows; it is not a cash receipt. Fixed-fee realization here is
contract value divided by gross baseline labor value, not a time billing rule.
This convention is valid for the deliberately all-completed synthetic history.

There are 100 terminal opportunities per historical month plus 20 open
opportunities at the export date. Open records are excluded from closed-cohort
win-rate and stage-transition estimates, not counted as losses. Stage histories
also yield empirical transition frequencies and mean days to the next stage.
They are reported but **not used by a stochastic stage-transition simulator**.
Pipeline flow is measured from resolved monthly cohorts; open pipeline inventory
is a separate concept. The exact stratified fixture is unsuitable for estimating
statistical uncertainty, causal effects or survival-adjusted conversion hazards.

`data.py:calibrate` reads exports, validates identities/ledgers and estimates
fee-specific realization, win rates, seasonality, utilization, capacity and pay.
It does not read `truth.json`. `run.py` separately compares those estimates to the
planted truth, then converts them to file-bound model parameters and series.
The Python adapter handles the raw operational joins; the native engine consumes
validated aggregate Parquet inputs. Source hashes link the adapter estimates to
the raw exports; native manifests cover the derived model inputs and results.

Generator configuration can be passed with `--config path.json`. Supported keys
are listed in `data.py:DEFAULTS`. Exact stratification requires integer win counts
within both fee groups; workforce must be positive/even, and historical demand
must fit capacity. Other constraints are explicitly rejected. Seeds randomize
record order and opportunity identities/outcomes within fixed cohort counts;
they do not create independent uncertainty draws.

## Evidence and its limits

The runner checks all twelve stock outputs at every monthly observation against
an independent Decimal ledger. It also reconstructs 36 historical months through
the native runtime and compares revenue/hours to the raw transaction aggregation.
This is **in-sample accounting reconstruction**, not an out-of-sample prediction
test. It cannot validate the assumed intervention effects.

Native declared checks must pass and share input/result identities with each
saved run. A deliberately wrong T&M formula passes internal accounting but must
fail the independent revenue comparison. Exact same-build numeric replay at one
thread checks the original multithreaded result, and a portable input bundle is
replayed. `test_data.py` adds nondefault calibration, truth-file independence,
row-order invariance and malformed-export rejection controls.

This work does not change the full platform regression or sanitizer status.
No hiring/attrition policy, freed-hours-to-sales feedback, random opportunities,
skill matching, AI quality/rework, success fees, retainers or court fee-approval
and collection processes are modeled. The user must agree those mechanisms and
assumptions before this becomes a T&R business pilot. Real data would require
field mapping, historical censoring rules and richer revenue reconciliation.

## Public practice context and peer-data follow-up

Reviewed September 30, 2026:

- [Ankura T&R](https://ankura.com/solutions/turnaround-and-restructuring) includes
  company/lender advisory, CRO, interim management and bankruptcy-related services.
  The time-based test here covers only a bounded part of that service context.
- [Ankura disputes](https://ankura.com/focus-areas/distress) includes expert
  testimony for complex litigation. Its operating assumptions should be separately
  agreed rather than copied from T&R.
- [Construction and infrastructure](https://ankura.com/industries/construction-and-infrastructure)
  includes project advisory as well as disputes; [construction disputes](https://ankura.com/services/construction-disputes)
  includes cost/quantum analysis and expert testimony. Construction and disputes
  therefore overlap in the public service taxonomy; these are not presumed to be
  mutually exclusive financial reporting segments.
- [FTI Q2 2026 results](https://ir.fticonsulting.com/news-releases/news-release-details/fti-consulting-reports-second-quarter-2026-financial-results/)
  still report segment operating KPIs, but definitions and applicability differ.
  Headcount is period-end; utilization/rates are not meaningful for all segments.
  Revenue includes effects such as success fees and pass-through amounts. A
  residual from headcount × hours × utilization × rate is not independently
  identified realization. Reconciling those bases and holding out periods must
  precede a credible peer backtest. No public peer data calibrates this run.

No ten-year peer backtest, CRM/IBM external dataset ingestion, or macro-series
calibration is claimed by this example.
