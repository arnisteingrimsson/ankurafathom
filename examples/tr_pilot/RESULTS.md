# Recorded synthetic T&R results — September 30, 2026

The initial practice has **150 employees: 135 billable and 15 support**. All business inputs are synthetic. FTI is an external comparison only. See the [model assumptions and limitations](README.md).

## What was run

34 five-year scenarios ran through the native C++ CPU SD runtime, using estimates from imperfect synthetic operational exports. A separate three-month, two-scenario test consumed observed US Courts filing counts with an assumed demand elasticity. This is not real-practice calibration or outcome validation.

## Selected year-five results

| Scenario | Revenue | EBITDA | EBITDA margin | Ending expected FTE |
|---|---:|---:|---:|---:|
| Constant workforce, no AI | $61.98M | $14.66M | 23.7% | 150.0 |
| Constant workforce, 20% AI | $58.55M | $11.38M | 19.4% | 150.0 |
| Responsive hiring, no AI | $61.98M | $23.52M | 38.0% | 117.7 |
| Responsive hiring, 20% AI | $58.55M | $22.10M | 37.7% | 108.5 |
| Responsive hiring, 20% AI + BD | $58.65M | $22.16M | 37.8% | 108.6 |
| Responsive hiring, 20% AI + 20% pipeline | $70.26M | $28.27M | 40.2% | 126.5 |
| Responsive hiring, 20% AI + 40% pipeline | $81.97M | $34.28M | 41.8% | 145.0 |

The first-year constant-workforce baseline produces $62.04M revenue and $14.73M EBITDA (23.7%) after modeled operating costs.

The AI cases shown use 20% savings on eligible tasks, a six-month adoption ramp and 50% pass-through of the reduction in hourly-equivalent value to new fixed-fee prices. This is not a uniform 20% reduction in practice hours.

Compare AI with the same workforce policy. Responsive hiring alone reduces expected staffing and lifts the modeled margin. Comparing AI/responsive EBITDA with the no-AI/constant-workforce baseline would incorrectly attribute that policy effect to AI. The matched responsive comparison shows a revenue and EBITDA decline from AI at flat demand; the tested 20% pipeline uplift more than offsets it. These sampled uplifts do not establish a precise minimum-growth threshold.

Long-run margins are sensitive to the assumed skill requirements, exit rates and cost behavior. Fractional expected FTE omits person-level vacancy and concentration risk. High modeled margins are not forecasts of achievable Ankura profitability.

## Verification

- **506,056** public monthly forecast values agree with the independent Decimal ledger; maximum absolute gap **3.58e-07**.
- Tolerances: absolute `2e-6`, relative `2e-10`; all 47,702 native declared-check evaluations pass.
- All 12 forecast batches pass exact one/eight-thread replay and portable-bundle replay.
- The observed-filings input demonstration adds 1,952 independent comparisons and 184 native rule evaluations.
- **27 focused normal tests pass**: 17 native mechanism tests and 10 data-adapter tests, registered as two CTest suites.
- All 17 native mechanism tests also pass under AddressSanitizer/UndefinedBehaviorSanitizer. A separate eight-level, three-scenario, three-month sanitizer run passes 2,928 independent comparisons and 276 native checks.
- A deliberately wrong T&M formula passes accounting identities and is rejected by the independent oracle. Future-history changes and corruption of the truth file cannot change calibration.
- No production C++ engine sources changed. No new full platform regression or full sanitizer result is claimed.

## Calibration and remaining gates

Training uses records before January 1, 2025: 2,369 opportunities, including 191 with missing fee types and 440 still open. Estimated arrivals are 98.71/month, mature-cohort win probability 43.44%, and hourly realization 90.01%. The planted mature-cohort probability (45%) lies inside the estimated 95% Wilson interval (41.11%–45.79%).

These are stochastic fixture recovery tests. CRM and timesheets are separate adapter fixtures, with no new engagement-level historical revenue reconstruction. Real export mapping, fee-application extraction, agreed staffing/cost assumptions, integer-person dynamics, contingent success events, calibrated BD/demand effects and acquisition valuation remain open.

The [compact evidence record](run-evidence.json) retains hashes and verification receipts. Full local artifacts are in `artifacts/tr-pilot-150-validated-20260930/`; large result files are excluded from Git. Run commands are in the README.
