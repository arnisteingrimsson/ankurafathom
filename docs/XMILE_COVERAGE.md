# Pinned XMILE corpus coverage

The current importer accepts **8 of 8 assessable files using the supported
equation subset (100%)**. Seven have complete compatible historical trajectory
checks (**87.5%**), meeting the 80% target for this assessable subset. The eighth
declares RK4 but ships an Euler export; its RK4 trajectory has separate
closed-form checks and stays excluded from historical agreement.
Full inventory: **67 files, 8 imported, 59 rejected**. M2 remains open for its
remaining source-function contracts.

## Eligibility policy v2

The source is SDXorg/test-models commit
`21aab02739dc5187bc9564e4d3de14e575905d2f`. All 67 original XMILE files are
vendored without changes, together with the upstream license and author notices.
The manifest pins each file's SHA-256. Expected output files are vendored for
accepted cases. Authored PySD fixtures are separate and never enter this denominator.

`tests/oracles/xmile/audit.py` assesses equations before attempting conversion.
Its versioned rule covers arithmetic +, −, ×, ÷, quoted names, named/embedded
lookup tables, DELAY/DELAY1/DELAY3/DELAYN/SMTH1/SMTH3/SMTHN, and STEP/RAMP/PULSE.
Version 2 adds the input functions; reviewing all 67 files leaves eligibility
and import counts unchanged. The default audit uses the constant DELAYN policy. It records
unsupported functions, conditionals, comparisons, powers, subscripts, and
placeholder equations. This is a reviewed lexical inventory for the pinned
corpus, not a general XMILE validator. The rule and importer are hashed in
`coverage.json`; widening the language requires reviewing the policy.

**Structural limitations remain in the eligible denominator.** Nonzero start,
RK4, clipping, source dialect, or missing stocks cannot make a supported-equation
file disappear from coverage. A regression test forces every importer call to
fail and verifies that the eligible denominator stays eight.

Malformed XML has unknown eligibility: 47 files have unbound prefixes and two
have mismatched tags. Those 49 files remain in the full inventory as
`unassessable`; they are not labeled unsupported equations or repaired silently.
The percentages apply only to the assessable subset and does not establish
coverage of those malformed files or general XMILE conformance.

| Classification | Files | Treatment |
|---|---:|---|
| Supported equations, imports and historical trajectory checks pass | 7 | Both import and historical numerators |
| Supported equations, imports; historical integrator conflict | 1 | Import numerator only; RK4 closed-form checks |
| Supported equations, structural import gaps | 0 | Denominator only |
| Parseable, outside supported equation subset | 10 | Explicit exclusions with evidence |
| Malformed XML, eligibility unknown | 49 | Retained as unassessable |

## Eligible files and remaining work

| Pinned source | Result | Evidence or remaining capability |
|---|---|---|
| samples/SIR/SIR.xmile | Pass | Complete stock trajectories against Stella and Vensim |
| samples/SIR/SIR_reciprocal-dt.xmile | Pass | Same reference trajectories; reciprocal dt encoding |
| samples/teacup/teacup.xmile | Pass | Complete stock trajectories against Stella and Vensim |
| tests/lookups/test_lookups_no-indirect.xmile | Pass | Signed stock trajectory against Stella and Vensim |
| tests/eval_order/eval_order.xmile | Pass | Two exact auxiliary observations against Stella export |
| samples/teacup/teacup_w_diagram.xmile | Pass | Euler clipping mapped; complete Stella/Vensim stock trajectories (limits do not bind) |
| tests/zeroled_decimals/test_zeroled_decimals.xmile | Imports; reference conflict | RK4 and stock-linked auxiliary rates supported; export follows Euler |
| tests/delay_xmile/test_delay_xmile.xmile | Pass | 208 historical observations: all eight variables, 13 ticks, both source orders; nonzero start, bounded vendor settings, filtered-flow references |

Counts include file variants. As an additional check,
**6 of 6 directory groups** have all their eligible variants imported. Directory groups are not asserted to be distinct mathematical models.

The new RK4 case is not counted as a complete historical pass. Its time-dependent
stock satisfies `stockmixed(t) = -.6777*t - t*t/2`; the vendored export instead
follows `-.6777*t - t*(t-1)/2`. Tests assert the discrepancy at every tick, check
all six RK4 stocks against closed forms in both source orders, and compare an
explicit Euler diagnostic rerun with the export. No source rewrite, tolerance
widening, or silent method substitution is used. The conflict is stored in the
audit and excluded from `historical_reference_compatible`.

The fixed-delay model now passes with `--outputs all`. Its source, expected
output and README are pinned unmodified; the README does not identify the
software that generated the export. References to filtered flows without a
limiting source stock are supported. References to stock-limited outflows,
cyclic clipping dependencies and arbitrary vendor settings remain unsupported.
The historical teacup limits never bind; active clipping has separate
hand-derived conservation, priority and sign-filter tests.

The corpus threshold is now met under eligibility policy v2. This does not
close all M2 work: the function-semantics area remains partial; see [M2 review](M2_ACCEPTANCE.md). The denominator stays explicit, with 49 malformed
files unassessable and one source/reference integrator conflict unresolved.

## Reproduction and checks

```sh
python3 tests/oracles/xmile/audit.py --verify
python3 tests/xmile_coverage_tests.py
python3 tests/xmile_import_tests.py build/fathom
python3 tests/xmile_function_tests.py build/fathom
python3 tests/xmile_delay_source_tests.py build/fathom
```

The audit works offline from vendored bytes; it can also verify a clean checkout
at the pinned commit. CTest runs inventory regeneration, hash checks, the
independent-denominator tests, and the trajectory suites on every configured
platform. Import counts alone never imply numerical validation. No remote CI
result is claimed by the local report.
