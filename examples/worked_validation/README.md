# Worked examples: calculations before platform output

Reporting expansion is deferred. These examples exercise the existing pilot
equations and staffing bridge with small inputs whose answers can be calculated
independently. They use synthetic assumptions, not measured Ankura effects.

Run from the repository root (new destination required):

```sh
.venv-runtime/bin/python examples/worked_validation/validate.py \
  build-arrow/fathom artifacts/worked-examples
```

The runner writes native observations, model/input files, manifests and a simple
`validation.json`. It does not invoke visualization, explanation pages or the
pilot reporting runner. Arrow is needed for the production pilot's Parquet inputs.
A CSV-only build can run the staffing example with `--staffing-only`.
The saved [validation evidence](../../artifacts/worked-examples/validation.json)
and [input/result directory](../../artifacts/worked-examples/) are available.

## 1–4. Where do saved delivery hours go?

One person has 160 delivery hours/month. Rate: $200/hour. Payroll: $10,000/month.
Variable delivery cost: $10/actual hour. Incoming won work is either 100 baseline
hours/month (spare capacity) or 200 (capacity constrained). Work is either all T&M
or all fixed-fee. Fixed-fee value is baseline hours × $200. No starting backlog,
demand growth, hires or other interventions. The numbers are chosen for arithmetic.

Copilot costs $40/month plus $300 training in month 1. Adoption is zero in month 1,
then increases by 1/6 each month through month 7. Full adoption reduces actual
hours per baseline hour by 8%, so the labor factor in month 7 is **0.92**.
Headcount/payroll stay fixed; saved hours do not themselves reduce salary cost.

All figures below are month 7, independently calculated and matched by the platform:

| Case | Revenue: baseline → Copilot | Profit: baseline → Copilot | Copilot change in year-one profit |
|---|---:|---:|---:|
| T&M, 100 hours of incoming work | $20,000 → $18,400 | $9,000 → $7,440 | −$13,700 |
| T&M, 200 hours of incoming work | $32,000 → $32,000 | $20,400 → $20,360 | −$780 |
| Fixed-fee, 100 hours of incoming work | $20,000 → $20,000 | $9,000 → $9,040 | −$100 |
| Fixed-fee, 200 hours of incoming work | $32,000 → $34,782.61 | $20,400 → $23,142.61 | +$22,646.66 |

**T&M with spare capacity:** 100 × 0.92 = 92 billable hours. Revenue is
92 × $200 = $18,400. Cost is $10,000 + 92 × $10 + $40 = $10,960.
Profit is $7,440. Utilization falls from 100/160 = 62.5% to 92/160 = 57.5%.
There is no extra work to absorb the saved hours.

**T&M with full capacity:** billable hours remain 160 and revenue remains $32,000.
Baseline-equivalent work completed increases to 160/0.92 = 173.913043 hours.
Variable cost is still $1,600; the $40 license reduces monthly profit by $40.
Higher throughput is not automatically higher T&M revenue.

**Fixed-fee with spare capacity:** the same 100 baseline hours still earn $20,000.
Saving 8 actual hours saves $80 in variable cost; after the $40 license, monthly
profit improves by $40. Across the full year, adoption sums to 8.5 fully adopted
months, saving 100 × 0.08 × 8.5 = 68 hours, or $680. Licenses and training cost
$480 + $300 = $780. **Year-one profit falls $100 despite the steady-state gain.**

**Fixed-fee with full capacity:** revenue becomes (160/0.92) × $200 = $34,782.61.
Actual hours remain 160, so cost is $10,000 + $1,600 + $40 = $11,640.
This case monetizes the extra completed work because demand and contracted pricing
both support it. Backlogs equal cumulative won work minus completed baseline work.

The oracle uses exact rational arithmetic for every month, both scenarios and all
12 outputs: **1,248 value comparisons**, including costs, backlog and headcount.
It does not import the pilot's expressions or use the C++ economics reference.
The production generator and original accounting declarations remain in use;
only horizon and experiment selection are reduced to one year/baseline/Copilot.

## 5. Staffing, queueing and revenue at the right time

Two consultants start with one slot each. Four engagements arrive on day 0.
One consultant leaves at day 1 after completing work; another joins at day 1.5.
A fifth engagement arrives at day 2. A second departure occurs at day 3 after
completion. Strict FIFO and non-preemptive delivery give this hand-scheduled ledger:

| Engagement | Arrival | Start | Finish | Slots |
|---|---:|---:|---:|---:|
| 10 | 0 | 0 | 1 | 1 |
| 11 | 0 | 0 | 2 | 1 |
| 12 | 0 | 1.5 | 2 | 1 |
| 13 | 0 | 2 | 3 | 2 |
| 14 | 2 | 3 | 3.25 | 1 |

Completions free capacity before same-time departures and new grants. The two-slot
engagement blocks the last job until day 3. Each completion recognizes $100; a
continuous cost flow is $20/day. At day 4:

- Revenue = 5 × $100 = **$500**; cost = 4 × $20 = **$80**; profit = **$420**.
- Occupied capacity-time = 1 + 2 + 0.5 + 2×1 + 0.25 = **5.75 slot-days**.
- Available total capacity-time = 2×1 + 1×0.5 + 2×1.5 + 1×1 = **6.5 slot-days**.
  Utilization = 5.75/6.5 = **23/26 ≈ 88.4615%**.
- Total waiting time = 0 + 0 + 1.5 + 2 + 1 = **4.5 days**.
- Total completed cycle time = 1 + 2 + 2 + 3 + 1.25 = **9.25 days**.
- Integrated revenue = $100 × [(4−1) + (4−2) + (4−2) + (4−3) + (4−3.25)]
  = **$875·days**. This checks when revenue entered SD, not just its final value.

All 16 outputs match the hand ledger at quarter-day, half-day and daily sampling:
**496 comparisons**, with identical values at shared observation times. This is
exact for these constant flows and event pulses, not a general Euler convergence
claim. The `granted` selector means *ever granted* and stays true after completion;
`in_service` and `allocated` describe current occupancy.

## Deliberately wrong cases

The runner also proves the independent checks catch two plausible mistakes:

1. Bill T&M using baseline work hours after AI savings. All native accounting
   declarations still pass, but the independent oracle rejects 22 values, starting
   with month-2 cumulative revenue. Internal consistency alone is insufficient.
2. Delay the final completion from day 3.25 to 3.5. Day-4 revenue remains $500, but
   integrated revenue becomes $850·days and the oracle rejects six values.

Normal and ASan/UBSan builds pass all **1,744** expected-value comparisons and both
negative controls. CSV-only passes the 496 staffing comparisons and timing control;
its pricing cases are explicitly not run. Maximum normal pricing gap is about
5.82e−11; staffing gaps are zero. No simulation-engine changes were needed.
These validate specified mechanics, not the realism of adoption, demand or pricing
assumptions. Next, extend worked cases around hiring lags and uncertain deal arrivals
before adding those mechanisms to the pilot.
