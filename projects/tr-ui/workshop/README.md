# T&R operating-model workspace

A project-owned, assumption-first hybrid model. Root `/` starts with an editable
practice definition and no simulation output. `/monthly` preserves the previous
monthly financial model, and `/analysis` preserves its evidence tools. These are
**different models** with different scope; their outputs should not be presented
as interchangeable forecasts.

## Build and start

From the repository root:

```sh
.venv-runtime/bin/python projects/tr-ui/workshop/build.py
.venv-runtime/bin/python projects/tr-ui/server.py
```

The host still needs the existing monthly model's packaged artifacts, described
in the parent README. The new model itself requires no business input files.
Build uses C++20 and the platform's native header-only ABM, DES, SD and Philox
components; no platform library or engine source changes are needed for this increment.

## Working definition

1. State the business question and document the assumptions.
2. Set the workforce, skills, project effort, demand, economics, AI and training.
3. Play, pause or step the native model. Inspect employees, queues, observations,
   accounting checks and recent events. Reset before changing the definition.
4. Compare one parameter across 2–8 values and 1–10 shared seeds. Multiple
   experiments can explore other variables. This release has no factorial or
   multi-parameter uncertainty-design UI.
5. Import named observations when available and compare against explicitly
   selected, already-computed simulation times and tolerances.

The initial workforce is 150 people: seven billable levels and support. The
pyramid, costs, rates and skill distribution are synthetic. Counts are editable.
The fixed population does not hire, retire or promote during this version.

## Model semantics

- **ABM:** `SyncPopulation<Employee>` stores individual roles, primary/secondary
  skills, productivity, licenses, proficiency, shifts and assignments. Project
  event handlers update agent behavior using native population replacement.
  This is an event-updated population, not a synchronous daily behavior phase.
- **DES:** `devs::Simulator<int>` schedules a project `Atomic` model. Its internal
  calendar contains daily setup, shift starts/ends, training/administration ends,
  prospects and service completions. Stable event priorities and insertion order
  resolve ties. An employee handles one stage at a time. Compatible FIFO work
  respects role bands and skills; a paused assignment resumes before new work.
- **SD:** `sd::Model` integrates recognized-revenue and operating-cost stocks with
  Euler. Hourly service changes the revenue rate, training changes the cost rate,
  and fixed-price completion pulses revenue. Rates are constant between events,
  so integration substeps do not improve these already-linear intervals.
- **Hybrid:** employee availability and skills govern DES work; AI changes service
  durations; service affects SD financial flows; delayed senior BD activity changes
  future demand. Coupling is native project code, not a claim that arbitrary
  hybrid configurations are supported declaratively by the platform.

Time is hours from Monday 00:00. Working days are Monday–Friday. Arrival/departure
ranges are uniform per employee/day. Administration blocks the first configured
fraction of the shift; daily training then uses up to weekly hours / 5. Support
staff are costed and tracked but cannot deliver work. Work cannot run off shift.
Projects start with no backlog. Long stages plus no opening work create a startup
transient; short runs are not representative annualized practice forecasts.

Prospects arrive as a daily Poisson process during 08:00–18:00. Win draws are
independent. Stage efforts share a mean-one lognormal project-size multiplier.
Skill and project mixes are sampled from configurable liquidity/restructuring/
operations shares. All projects have analysis, planning and review stages, with
one person working on each stage, sequentially. No teams or parallel workstreams.

License coverage is an exact rounded fraction of billable employees selected by
stable random ranking. An assignee makes a stable per-project/stage usage draw.
Effective saving = maximum saving × stage exposure × current proficiency.
Effective work speed = employee productivity / (1 − effective saving).
A stage can acquire a new speed after overnight interruption as proficiency grows.
Review effort additionally scales with the earlier-stage share of work using AI.
Licensed users learn exponentially from training or AI work. Training-method
multipliers are hypothetical, not claims of relative real-world efficacy.

Senior idle time (director and above) can be partially allocated to BD. Delivery
preempts it. BD hours add prospects after a calendar-day lag; an arrival increase
is used on working days, with weekend-maturing BD carried into Monday.

Hourly revenue accrues as actual delivery hours × employee rate × realization.
Fixed fees are size-scaled and recognized on completion. Costs include loaded
salary, allocated overhead, licenses and training; salary/overhead/licenses accrue
over all calendar hours using a 365-day year. Contribution = revenue − modeled
operating costs. No cash, tax, financing, holdbacks, court approvals or acquisition
valuation is modeled here. The monthly financial model retains separate richer
accounting rules.

## Experiments and reproducibility

Philox addresses randomness by entity/day/stream. Matched seeds preserve source
random draws; demand feedback and resource competition can change resulting paths.
Do not interpret coupled runs as matching every realized event or assignment.
Experiments run sequentially in a background worker with progress and cancellation
between observation steps. Reported means and min–max ranges are descriptive;
they are not confidence intervals. The first listed value is the reference for
paired contribution differences. A cancelled experiment retains completed runs.

Sessions block on stdin between requests. Observations may advance 0.25–24 hours;
all intervening native events execute. The financial integration maximum step is
separate from observation interval and browser pace. No future output is computed
on load. Failure stops the worker; reset begins a new process. Four interactive
sessions plus one sequential experiment are supported locally. Paused sessions
expire on a later creation request after 15 minutes. Browser hiding pauses live
advancement; background experiments continue until completed or cancelled.

Definitions and native frames are retained in
`artifacts/tr-ui-state/operating-sessions/`. Each frame is hash-chained; the run
receipt also includes definition, runner and source hashes. These are project
receipts, not the general platform's batch-run manifest format. Historical agent
LLM authorship/reviewer identities are not available and are not invented.

## Evidence contract

Import JSON with `source`, `kind` (`observed` or `synthetic`), and `rows`. Each row
requires `time` in elapsed hours, `metric`, `unit`, `actual`, and an absolute
`tolerance`. The UI template deliberately leaves `actual` null; users must fill it.
Only exact recorded times can be compared. Duplicate points, wrong units,
nonfinite values, unknown metrics and missing observations are rejected.

Example shape (illustrative observation, not actual Ankura evidence):

```json
{"source":"Synthetic roster fixture","kind":"synthetic","rows":[
  {"time":24,"metric":"headcount_level0","unit":"person","actual":18,"tolerance":0}
]}
```

Units are published in `/workshop/catalog` under `metric_units`. Counts and
financial totals are cumulative except headcount, busy/on-shift and backlog,
which are current state. Utilization is cumulative delivery hours / cumulative
shift hours of billable roles, including administration/training/idle in the
denominator. Cycle and wait means refer to completed engagements. Proficiency is
mean among license holders. `headcount_level0` through `headcount_level7` map to
the ordered eight role rows. Point comparisons do not perform fitting, prove
independent historical holdout, or validate causal assumptions. Raw Salesforce,
HR and timesheet mapping remains a separate adapter task.

## Verification

```sh
.venv-runtime/bin/python projects/tr-ui/workshop/test_workshop.py
.venv-runtime/bin/python projects/tr-ui/workshop/build.py --sanitize
.venv-runtime/bin/python projects/tr-ui/workshop/record_checks.py
```

Tests cover independent hand schedules and fee arithmetic; skill bottlenecks;
no-demand cost accounting; zero-effect AI negative controls; observation and SD
step independence; repeatability; paused process barriers; idempotent retries;
strict definitions/evidence; paired seeds and cancellation. Native checks at each
observation reconcile workforce time, skills/ownership, engagement conservation,
and financial totals against employee/project ledgers.
