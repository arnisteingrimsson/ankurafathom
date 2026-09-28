# Benchmark workload campaign

These are native platform workloads checked against independent answers. They
supplement CTest; they do not replace it or certify entire external libraries.
No reporting UI or new third-party runtime dependency is introduced.

Acceptance is **engine results and accuracy**: numerical values, event timing,
state transitions, invariants, convergence and independent reference comparisons.
GUI interactions, display rendering and graphical publication artifacts are
excluded at the user's direction; they are not unfinished acceptance requirements.
Numerical histogram bins and summary statistics remain testable engine outputs.

```sh
cmake --build build-arrow -j 3
ctest --test-dir build-arrow -j 3 --output-on-failure --output-junit campaign.xml
python3 tests/benchmarks/run.py --out artifacts/my-benchmark-run
python3 tests/benchmarks/run.py --out artifacts/my-benchmark-sanitized-run --sanitize
python3 tests/benchmarks/assess.py --junit build-arrow/campaign.xml \
  --adapters artifacts/my-benchmark-run --out artifacts/my-coverage.json
```

Use a **new output directory** for each run. The runner compiles against the
current native headers, stores commands/source hashes, and stops on any failed
contract. The assessor retains every requested workload and distinguishes
passing bounded coverage from absent adapters. Its overall verdict remains
`partial` while catalog gaps remain. It exits nonzero for failed/missing execution
evidence; unimplemented workloads stay explicitly `not_run`.

For an incremental run, use `--only c17` (or another adapter name). The assessor
accepts repeated `--adapters` roots for disjoint prior/new results; duplicate
evidence for the same adapter is rejected. Reusing evidence is not a rerun.

| Workload | Independent acceptance |
|---|---|
| ARGESIM C22 | 70 cases, 187,920 numeric/record comparisons against a Python event calendar; selected published deterministic jockeying, reneging and classing tables |
| Life | 67 cases, 46,656 cell comparisons against a direct coordinate stencil; block, blinker and glider behavior |
| Random walk | 4 × 4,096 walkers; 16 snapshots; exact binomial CDF and finite-time mean/MSD targets |
| Percolation | Every occupancy pattern on bounded 3×3 and 4×4 four-neighbor grids: 66,048 comparisons against union-find connectivity |
| Ising | Every 3×3 periodic state at three temperatures; 27,648 one-spin updates; 13,824 detailed-balance checks against exact Boltzmann weights |
| Hybrid clock | Eight constant-stock/teacup cases, 576 comparisons; off-grid events, standalone execution on the same mesh, independent Euler product formula |
| StupidModel computational rules | Isaac 2011 versions 1–16; 43 cases, 789,942 comparisons against a separate Python record-table implementation; four malformed landscapes rejected |
| ARGESIM C21 event-contact ball | Eight cases, 12,388 comparisons against analytic flight formulas; first 100 impact times, with/without quadratic drag; wrong coarse-event implementation rejected |
| ARGESIM C17R subset | 12,417 local cases and 30 spatial trajectories, 2,995,770 state/rate comparisons; 12 ODE refinements, 3,636 state comparisons against independent order-18 Taylor integration |

C22 runs as a **custom native DEVS atomic workload**. Its queue policies are
benchmark code, not newly supported declarative library policies. Input paths
are shared with the Python oracle; this is pathwise comparison, not independent
RNG distributional equivalence. The classing adapter uses the published
incoming-chain/called-batch convention: newly arriving jobs await a subsequent
call, even if their class matches the current batch. Deterministic values dock
to Tables 3, 5 and 7 of the [MatlabGPSS solution](https://www.sne-journal.org/fileadmin/user_upload_sne/SNE_Issues_OA/SNE_29_4/articles/sne.29.4.10496.bn22.OA.pdf)
for the [C22 definition](https://www.sne-journal.org/fileadmin/user_upload_sne/SNE_Issues_OA/SNE_29_3/articles/sne.29.3.10481.bn22.OA.pdf).

Life uses native GridSpace/SyncPopulation. Random walk uses the native population
and addressed Philox. CDF gates are frozen before execution, using DKW with a
union bound and familywise alpha 0.001; six-standard-error moment gates are
additional approximate checks. Missing cases/snapshots fail the contract.

Percolation uses a static occupied lattice and synchronous fire reachability;
the oracle instead unions explicit right/down edges. Ising uses one selected
spin per update, zero field and J=kB=1. Acceptance probabilities, energy,
magnetization and the stationary finite-state measure are checked. Neither
workload establishes an infinite-lattice critical threshold, stochastic mixing,
or a full NetLogo implementation. Larger finite-size studies remain open.

Zero pulses split ClockedSD's integration intervals. Coupled and standalone
Euler agree on the same merged mesh; expecting equality to a different coarse
mesh would incorrectly reject valid finite-step behavior. Constant-rate stock
integration has no coupling drift. Teacup error decreases under refinement.

StupidModel follows [Isaac's 2011 reformulation](https://www.jasss.org/14/2/5.html),
not an unspecified mixture of original and revised versions. The native workload
uses GridSpace and SyncPopulation for sequential movement, identity and lifecycle.
The Python oracle instead maintains positions and live agents in record tables.
Both consume frozen random input tapes; initial positions and normal deviates
are supplied by the case generator. This validates native execution of those
inputs, not native random initialization or distributional equivalence to NetLogo.
All movement occurs before growth; offspring join mortality testing in the same
iteration. Stable IDs break size ties; neighborhoods are deduplicated on small
tori, and the operational stopping cap is 1,000 completed iterations.

Targeted cases cover packed grids, equal-food choices, strict reproduction
thresholds, failed births, newborn mortality, normal-draw clamping, extinction,
and stopping. One initial predation control allowed its prey to move to a richer
cell before hunting; the independent pathwise comparison passed but its hand
expectation failed. A flat synthetic landscape now isolates the intended
same-cell hunting check. No engine change was needed.

The source archive was inspected and its SHA-256 is recorded in the validation
result, but its Python 2/Tk programs were not executed. Original `Cell.Data` remains
unavailable in this campaign: versions 15–16 use clearly labeled synthetic files,
including shuffled rows and invalid-input controls. Output contains the underlying
states, bins and statistics. GUI probes, widgets and rendered plots are outside
acceptance scope. Published-program result comparisons remain open.

The [C21 definition](https://www.sne-journal.org/fileadmin/user_upload_sne/SNE_Issues_OA/SNE_26_2/articles/sne.26.2.10339.bn21.OA.pdf)
supplies the ball parameters. Native SD RK4 handles flight; a custom DEVS atomic
locates downward height and velocity crossings by bisection. The independent
oracle uses closed-form linear or quadratic-drag flight segments. At apex heights
below `1e-10 m`, the workload enters rest; its stopping time approximates the
finite bounce-accumulation limit and is checked with an explicit remaining-flight
bound. No finite computation claims to perform infinitely many impacts.

The no-drag impact-time gap is below `4.8e-12 s`. With drag, maximum event-time
error falls from `5.60e-7 s` at dt=0.2 to `4.91e-11 s` at dt=0.025. The gate was
fixed before execution. This adapter does **not** add a general production
state-event locator or DAE solver. Dynamic contact, diode circuit, pendulum and
the velocity-compensation fitting tasks remain open.

[C17R](https://www.sne-journal.org/fileadmin/user_upload_sne/SNE_Issues_OA/SNE_25_1/articles/sne.25.1.10283.bn17r.OA.pdf)
uses a six-channel hexagonal lattice. Native SyncPopulation executes streaming,
FHP-I collisions or local diffusion, then synchronous infection/recovery. The
independent oracle pushes sparse particles to destinations; native cells pull
incoming channels from frozen snapshots. We explicitly choose periodic axial
boundaries and once-only integer-boundary interventions before streaming. Hard
interventions select `floor(fraction * eligible)` people without replacement;
soft interventions use step, linear or smooth reductions in infection/recovery
probability. Frozen shared draws establish pathwise agreement. States, counts
and intervention events agree exactly; rates allow `2e-16` rounding error.
Hex topology and policies are workload code, not new general library features.

The two 10,000-person runs use the published initial counts/probabilities, with
explicit `n=46`; integer lattice size gives effective uniform contact count
`5*9999/(6*46^2-1)`, slightly below the nominal four. All 64 collision masks and
4,096 local empty/S/I/R arrangements are checked, including momentum, population
conservation and prevention of same-tick recovery of newly infected people.
The ODE uses the definition's logarithmic rate mapping and a separate Taylor
oracle. Its fast-spread case rejects dt=1 and dt=1/32; dt=1/64 and finer meet the
fixed `1e-4`-person accuracy target, with fourth-order convergence above the
roundoff floor. Tolerances were not relaxed to accept the coarse runs. C17R's
parameter-region/tradeoff studies, global mixing, repeated/targeted interventions,
ODE intervention events and published executable docking remain unassessed.

Existing CTest coverage supplies SD analytic/convergence models, queueing
analytics, pinned Ciw/SimPy/Mesa comparisons, SIR/Bass limits and fluid limits.
Pinned reference comparisons do not imply a fresh reference-engine execution.
The complete requested-workload inventory is in [catalog.py](catalog.py).

Still open: other ARGESIM cases, remaining StupidModel data/docking tasks, rejected SDX features,
several named classic ABMs, a selected CoMSES replication set, large-lattice
critical behavior, AnyLogic execution, and empirical/calibration work. Forward
synthetic-pilot agreement does not establish parameter recovery or causal effects.
