# T&R pilot: economics specification and acceptance gates

Status: implementation specification with a bounded monthly example, September
30, 2026. The user has specified **150 employees**. The original synthetic run
remains a mechanics fixture. The new [T&R monthly pilot](../examples/tr_pilot/README.md)
implements the subset below; this does not mark every acceptance gate in this
specification complete. No FTI operating datapoints calibrate either run.

## Implemented subset and open gates

| Phase | Implemented in the monthly example | Still required for the full specification |
|---|---|---|
| Level costs | Seven billable levels plus support inside 150; salary, employer load, bonus, support and allocated overhead; level skill bottlenecks | Agreed roster, verified rates, real cost allocation, explicit leave/calendar and part-time histories |
| Workforce and AI | Expected-FTE exits, adjacent promotions, full replacement/freeze/responsive hiring, fixed start lag, onboarding, exposed/protected level task time, rework and training | Integer-person stochastic populations, tenure/cohort behavior, offer/cancellation stages, detailed task taxonomy and quality acceptance |
| Fees and cash | Hourly, fixed, earned retainer service units and expected success fees; old contract values preserved; new-price erosion, approval/disallowance/holdback/collection/deposits | Contract-level mixed terms and amendments, actual contingent success events, engagement duration, court-order and accounting-policy calibration |
| Growth | Capacity-constrained senior BD, lagged prospect conversion, bounded synthetic demand shock, separate observed US Courts input demonstration | Estimated causal/forecast relationships, actual opportunity/service dynamics and historical holdout validation |
| Imperfect data | Seeded variable arrivals/outcomes, permanently open cases, slipped dates, missing fee types, stale probabilities, dated calibration cutoff, malformed/leakage controls and a later synthetic cohort | General real-export mapping, changing rosters, broad missingness/selection sensitivity, repeated coverage studies and real-outcome validation |

The example reports level workforce/utilization, rate/realization, loaded
compensation, EBITDA, collections, receivables, deposit liability and backlog.
It does not yet provide a calibrated open-pipeline model, acquisition comparison,
integer-person forecasts or cash valuation. The original acquisition test remains
separate. See the example's receipts for exact evidence; requirements below remain
the acceptance contract for subsequent extensions.

## What the existing evidence establishes

The current model passes its independent monthly ledger comparisons, native
declarations, malformed-export controls and exact replay. This supports those
particular implementations and inputs. It does not establish that the entire
engine is correct, that the synthetic economics resemble Ankura, or that the
model can predict real outcomes. Historical reconstruction is in-sample.

Its reported roughly 52% margin is a contribution ratio after base salary,
variable delivery expense and intervention costs. It excludes material costs
needed for practice EBITDA. The 76% fixed-fee threshold is an unconstrained
sensitivity result, not an actionable recommendation for a T&R practice.

The new pilot must distinguish mechanics verification, parameter calibration,
historical holdout validation and sponsor acceptance. Passing one is not passing
the others. Adding realistic-looking complexity alone does not close these gates.

## Corrections to the external critique

- Court supervision does not imply an absolute hourly-fee restriction.
  [11 USC 328(a)](https://uscode.house.gov/view.xhtml?edition=prelim&num=0&req=granuleid%3AUSC-prelim-title11-section328)
  permits several compensation bases with court approval. Represent the actual
  engagement agreement and applicable approval order. Do not infer that existing
  matters can be repriced, or that 76% fixed fee is commercially available.
- Approval, holdback and collection timing are separate from the fee basis and
  from the model's revenue-recognition policy. A delayed payment is not itself a
  revenue haircut. Disallowance and write-offs need separate events.
- Natural attrition of 15–20% is not a verified Ankura T&R input. FTI reports
  **14% company-wide voluntary turnover in 2025**, which is useful context but
  neither a T&R-specific rate nor a total-turnover measure.
  [FTI 2025 annual report, printed page 16](https://ir.fticonsulting.com/static-files/f95b2344-3d05-4972-9800-48dcc1faca75)
- A higher blended hourly rate after junior hours disappear is a composition
  effect; it does not prove revenue growth. Track numerator and denominator.
- Fixed headcount can overstate the long-run cost of surplus capacity, but
  attrition does not guarantee a positive AI NPV. Departures can remove scarce
  skills or relationships, and replacement/retention costs may be material.
- Fixed overhead reduces reported margins but does not automatically change
  the absolute revenue loss from one fewer billed hour. Specify which costs
  vary, which are avoidable, and when reductions actually become effective.

## Public evidence and parameter provenance

| Evidence | Verified context | Permitted use |
|---|---|---|
| [FTI Q2 2026 results](https://ir.fticonsulting.com/news-releases/news-release-details/fti-consulting-reports-second-quarter-2026-financial-results/) | Corporate Finance: $553 average billable rate, 59% utilization, 2,358 period-end billable headcount and 20.9% adjusted segment EBITDA margin | External reasonableness comparisons after reconciling definitions; not a pure T&R or Ankura calibration target |
| [FTI 2025 annual report](https://ir.fticonsulting.com/static-files/f95b2344-3d05-4972-9800-48dcc1faca75) | Company-wide voluntary turnover was 14% | Sensitivity context; do not substitute for level-specific Ankura exits |
| [US Courts bankruptcy tables](https://www.uscourts.gov/data-table-topics/bankruptcy?order=name&sort=asc) | F-2 offers business/nonbusiness and chapter distinctions, including monthly/quarterly periods | Candidate demand covariate; no assumed causal elasticity |
| Chapter 11 retention orders and fee applications | Extraction and reconciliation for relevant restructuring engagements remain outstanding | Case-specific level rates, billed hours, discounts, fee terms and payment procedures; not employee pay or universal practice mix |

FTI's Corporate Finance scope is broader than the modeled practice, and its
reported adjusted segment EBITDA is not interchangeable with fully allocated
Ankura practice EBITDA. Annual/quarterly utilization denominators, realized versus
standard rates, average versus closing workforce, non-hourly fees, currency and
pass-through revenue all require reconciliation. Do not solve a revenue residual
and then treat agreement with that same revenue as independent validation.

Every input must carry value, unit, effective period, practice/level/engagement
scope, source URL or export hash, extraction location, definition, and status:
`observed`, `peer_proxy`, `synthetic`, or `policy`. Preserve original and normalized
values. Calibration must not read planted truth. No universal rate pyramid or
Ankura compensation range has been established by this review. Case-specific
rates must be extracted from the relevant practice and period before use.

## Phase 1 — level structure and fully loaded costs

Implement a new versioned example alongside `examples/ai_decision/`; preserve
the existing fixtures and receipts. Keep native SD execution as the initial
monthly reference. Represent each workforce level separately, with explicit
capacity, task demand and financial flows. Python may prepare inputs and reports;
it must not become the production simulator.

### Inputs and semantics

- Separate employee headcount, paid FTE, billable roles, practice support staff
  and allocated central support. Initial practice employee counts sum to 150.
  Do not add support employees on top of 150 without changing that definition.
  Fractional FTE is allowed only as declared work capacity, not fractional people.
- Support six or seven configurable billable levels and a nonbillable category.
  Names and the actual distribution require an Ankura roster. A test-only vector
  `[18,27,30,24,18,12,6,15]` sums to 150, with the last bucket support; it is not a
  proposed Ankura staffing pyramid.
- For each level: standard rate, contractual discount, annual base pay,
  employer benefits/taxes, bonus basis, paid calendar, leave, training,
  administration, business development and delivery capacity. Distinguish a
  utilization target from physically available delivery hours.
- Hours reconcile as delivery + BD + administration/training/leave + idle =
  paid hours. All buckets are exclusive. Support roles cannot bill unless an
  explicit job rule allows it.
- Model engagement/task staffing requirements. At first, use fixed skill shares
  and a declared allocation rule; work cannot complete if a required level is
  unavailable. Reallocation and substitution must be explicit and auditable.
- Monthly cost = base pay + employer load + bonus accrual + support compensation
  + allocated overhead + other operating expense + AI expense. Define the tax/
  benefits base and bonus policy. Do not count support payroll in both roster
  cost and overhead. Keep fixed-dollar and revenue-variable SG&A distinct.
- Publish a bridge from the existing direct contribution to modeled practice
  EBITDA before depreciation/amortization, interest and tax. This is a specified
  management-model measure, not an audited financial metric.

### Independent acceptance cases

1. **Aggregation:** collapse identical levels to one level; every monthly
   capacity, revenue, backlog and direct-cost value equals the old fixture.
2. **Cost ledger:** monthly base $10,000; load 20% of base ($2,000); bonus 10% of
   base ($1,000); support $1,000; overhead $3,000. With $30,000 revenue and no
   other expense, EBITDA is $13,000, not $20,000. Zero delivery still incurs
   $17,000 cost while the workforce is retained.
3. **Workforce definition:** each employee belongs to one category; headcount
   sums to 150, FTE reconciles separately, and support hours create no revenue.
4. **Skill bottleneck:** 100 junior hours plus 10 senior-review hours are required
   per engagement. Unlimited junior availability and zero senior availability
   must produce zero completed engagements under that completion rule.
5. **Reporting:** weighted rates and utilization are computed from summed
   numerators/denominators, never an unweighted average of level ratios.

Deliverable: baseline and demand-shock cases with a transparent EBITDA cost
bridge, source-status table, independent oracle and replay evidence. Do not
publish new AI investment recommendations from this phase alone.

## Phase 2 — workforce policy and AI by level/task

These should be developed together: the hiring response depends on which skills
AI frees. Track vacancies, recruitment cohorts, offers, starts, onboarding,
promotion, voluntary exits and involuntary reductions separately. Use one owner
of each population/stock; do not maintain contradictory headcount copies.

Monthly transition order must be fixed: due starts and previously scheduled
exits, opening payroll/capacity snapshot, task allocation and delivery, revenue/
cost posting, then policy decisions creating future vacancies and starts.
Document any within-month proration or use explicit event timing. Do not let a
new decision supply same-month capacity without a zero-lag policy being declared.

Convert annual exit probability to monthly probability with
`q_month = 1 - (1 - q_year)^(1/12)` under a constant monthly conditional probability.
A continuous hazard uses `lambda = -ln(1-q_year)/year`; these conventions are not
interchangeable with dividing an annual probability by twelve. Show expected-FTE
and integer-person stochastic results separately. Initial headcount remains 150;
subsequent workforce is a modeled outcome.

Compare constant workforce/full replacement, hiring freeze, and demand-responsive
replacement under the same demand realizations and exit draw addresses. Surplus
capacity does not cancel payroll until someone exits or a staffing policy changes
paid employment. A freeze stops new requisitions; already committed hires obey a
separate cancellation rule and cost. Attrition can worsen delivery bottlenecks.

Task inputs separate exposure, adoption, time saved, review effort and rework.
Apply savings to affected level/task hours, retain minimum senior review, and
charge training/implementation time to capacity as well as cash expense.
Commercial pass-through of savings belongs to the fee model, not the labor factor.

Acceptance cases:

- Zero attrition and no hires reproduce constant workforce; full replacement
  with zero lag preserves population under an explicitly paired exit/start rule.
- At 20% annual attrition and no hires, expected FTE after twelve months is
  120 from an initial 150, using the exact monthly-probability convention. Do not
  require one stochastic trajectory to end at exactly 120 people.
- Two hires approved in month 1 with a two-month start lag contribute nothing
  in months 1–2 and enter at the start of month 3 under the declared convention.
- Promotion reduces one level and increases another by the same headcount;
  costs/rates change at the effective date and total headcount is conserved.
- **Composition example:** 100 junior hours at $200 and 20 senior hours at
  $1,000 yield $40,000 and a $333.33 blended rate. Remove 50 junior hours while
  retaining senior work: revenue is $30,000 and blended rate is $428.57. The
  rising blended rate must not be reported as higher revenue.
- Fixed-fee work with unchanged price and delivery quality retains its fee
  when junior hours fall; any rework and retained payroll remain explicit.

## Phase 3 — contract fees, approval, price erosion and cash

Define fee basis and approval/payment process as independent fields. An engagement
may combine hourly fees, earned periodic service fees and a success component.
Distinguish an advance deposit/retainer from a monthly earned retainer fee; cash
receipt does not make both revenue. Contractual credits between components must
prevent duplicate charges. Use contract-specific eligibility for future pricing
changes, and respect existing commitments and effective dates.

Track standard work value, earned revenue under a declared model recognition
policy, amounts submitted, approved, disallowed, invoiced, held back, collected
and written off. Payment timing must not be disguised as lost revenue. Model
success-fee conditions, accrual/recognition rule, probability and cash date
explicitly; this is not an assertion of accounting treatment for actual contracts.

Price erosion scenarios specify fraction of savings passed through, timing
(new engagements versus renewals versus permitted amendments) and eligible work.
No global fixed-fee switch may reprice all existing matters. Show the feasible
fee-mix frontier after eligibility constraints, and report infeasibility instead
of extrapolating an unavailable lever.

Acceptance cases:

- A $100 approved fee with 20% holdback and no disallowance produces $100 earned
  revenue under the fixture's policy, $80 collected initially and $20 retained
  receivable; later release moves $20 to cash without earning another $20.
- Disallowing $10 and delaying $10 have different effects on earned value and
  eventual cash. Reconcile each contract's ledger at every event, not just at end.
- A $30,000 earned monthly service fee remains $30,000 when work hours change
  inside the agreed service scope. An advance deposit instead creates a
  liability/credit until the specified earning/application event.
- A conditional success fee is earned only under the declared trigger; a fee
  already credited against a retainer cannot be charged twice.
- A $100 fixed fee with a 20% labor saving and 50% contractual pass-through
  becomes $90 for eligible new work; an ineligible existing contract stays $100.
- Zero erosion reproduces the protected-price case. A requested fee share above
  the eligible share must reject or return an explicit infeasible verdict.

## Phase 4 — growth feedback and externally driven demand

Introduce separate, testable channels rather than a single pipeline multiplier:

1. Eligible senior capacity allocated to BD reduces availability elsewhere.
   Convert BD hours into opportunities with a bounded productivity parameter and
   a delay distribution. Add saturation, qualification, win/loss, sales-cycle and
   delivery lags. Extra idle junior hours do not become senior BD by default.
2. Exogenous market demand uses dated business Chapter 11 or another defined
   relevant series. Do not substitute total consumer bankruptcy filings, count
   affiliated legal-entity petitions as independent clients without qualification,
   or apply an identical macro response to disputes and construction.

Use an additive specification with documented interaction terms, so a market
upswing is not counted again as BD success. Elasticities and lags need estimation
or labeled sensitivity assumptions. A countercyclical story alone does not
identify Ankura's opportunity demand. Use data releases available at each forecast
origin, preserve vintage/revisions and reserve chronological holdout periods.

Acceptance: zero BD effectiveness gives no incremental opportunities; zero
eligible hours gives no BD; no new opportunity arrives before its delay; a one-off
market shock affects only its declared lag window; full-capacity senior staff
cannot supply free BD. Compare a fixed scenario grid with an endogenous response
using matched random streams, and test zero-feedback reduction to Phase 3.

## Phase 5 — imperfect operational data and stochastic validation

Retain clean deterministic fixtures as arithmetic controls. Add separate exports
with latent outcomes and an observation layer: stale probabilities, shifted
expected close dates, missing fee types, duplicates, partial histories, late
timesheets, aging open deals and terminal outcomes not yet observable at cutoff.

Do not replace these with independent numeric jitter. Preserve underlying IDs,
event times, relationships and ledger constraints. Raw snapshots are immutable;
corrections form a versioned cleaning receipt. Missing fee type is unknown, not
silently T&M. Close-date history and as-of dates distinguish forecast changes
from realized closes. Open deals are censored, not automatically wins or losses;
removing them also cannot justify an unbiased overall conversion-hazard estimate.

Predeclare noise levels, replication counts, estimation intervals and tolerances
before checking results. Exact recovery is appropriate for clean fixtures;
statistical recovery requires bias/coverage checks across independently addressed
replications. Train/holdout division is by time and engagement, with no duplicated
records or future terminal statuses leaking across the boundary. Preserve some
private latent truth for the evaluator, unavailable to calibration code.

## Initial sensitivity designs, not market estimates

Until sourced or supplied, these are deliberate stress-test grids, not realistic
Ankura parameter ranges or recommended operating targets:

| Parameter | Initial test grid | Required qualification |
|---|---|---|
| Annual voluntary attrition | 0%, 5%, 10%, 15%, 20% | Estimate by level; zero is a control, FTI's 14% is company-wide context |
| Start lag | 0, 1, 3, 6 months | Treat approved vacancies and accepted offers separately |
| Employer load on base | 0%, 20%, 35% | Explicit compensation base; zero is a reduction control |
| Bonus accrual | 0%, 10%, 30% of base | Profit/revenue-linked pool is a separate policy, not the same parameter |
| Eligible task time saved | 0%, 10%, 20%, 40% | Level/task-specific; maintain review and quality constraints |
| Price pass-through | 0%, 25%, 50%, 100% of savings | Only eligible contracts at their permitted effective dates |
| Illustrative holdback | 0%, 10%, 20% | Contract/order input; no assertion of universal court procedure |
| Approval/collection lag | 0, 1, 3, 6 months | Approval and collection are distinct processes |
| Fixed-fee eligibility | 0%, 25%, 50%, 100% of eligible incoming work | Sensitivity boundaries only; no inferred observed mix |

Rates, workforce composition, salaries and overhead dollars require source-based
or sponsor-specified scenarios. Do not choose them to force a particular EBITDA
margin or reproduce an attractive AI outcome. BD conversion and macro elasticity
have zero-effect controls and otherwise remain explicitly uncalibrated until a
justified range is specified.

## Required outputs and denominator definitions

| Area | Definition |
|---|---|
| Hourly blended rate | Hourly earned revenue / actual hourly-billed service hours; non-hourly fees reported separately |
| Realization | Billed value / standard value, approval loss and collection realization as separate ratios; no double counting of discounts |
| Revenue per FTE / MD | Period revenue / time-weighted paid FTE or declared MD-equivalent headcount; no invented attribution of all revenue to specific MDs |
| Leverage | Declared junior/delivery FTE divided by MD-equivalent FTE, with the definition shown |
| Utilization | Delivery hours / declared paid or available hours, by level and firm; show both denominator conventions if needed for peer comparison |
| Workforce | Opening/closing people and FTE, hires, exits, promotions, vacancies, time to hire and productive onboarding lag |
| Pipeline | Gross and probability-weighted open pipeline, period-qualified win rate, close-date slips, aging and average deal size |
| Coverage/backlog | Eligible expected work or remaining contracted work / explicitly chosen monthly delivery capacity, with skill bottlenecks shown; zero denominator yields N/A |
| Compensation ratio | Fully loaded defined compensation / revenue; reconcile practice support allocation |
| Economics | Direct contribution, practice EBITDA cost bridge, AI cash cost and training hours per paid FTE, recognized revenue, receivables, cash collections and liquidity need |
| Value comparison | Operating-contribution NPV kept separate from a newly specified cash-flow NPV; show horizon and terminal-value assumption |

Undefined ratios are N/A with a reason, not zero or infinity. Explain any change
in blended rate using hours/rates/mix decomposition, not a single productivity
label. Thresholds must state constraints, horizon, scenario assumptions, search
resolution and whether the compared outcome is annual, cumulative or cash-based.

## Implementation and review sequence

1. Add source/parameter receipts and Phase 1 level/cost fixtures. Implement the
   native monthly baseline and independent hand/Decimal oracle. Preserve old run.
2. Add workforce policy and task-specific AI, with independent event schedules,
   exact zero-effect reductions and paired scenario comparisons.
3. Add contract/cash mechanics and feasibility constraints. Review engagement
   samples and assumptions before calling the output a business pilot.
4. Add endogenous BD and a dated external demand input, then historical holdout
   and imperfect-data/stochastic estimation tests.

Use supported native SD, DES and workforce components only within their documented
contracts. General hybrid wiring is not assumed available. If a discrete fee or
staffing event needs a new bridge, implement and validate that bridge explicitly
before claiming end-to-end support. Freeze monthly ordering before optimization.

Each phase records the executable/version, source/input hashes, comparison
counts, tolerances, failures, rejected controls, exact thread replay and retained
input bundle. Run focused native sanitizer tests for any changed native code;
keep historical full-suite counts separate. Never mark the whole platform or
business pilot accepted because a set of arithmetic checks passes.

The source changes and this specification are local. This review did not commit,
push, or publish them; the quoted suggestion to share code is not recorded here
as a completed GitHub action.
