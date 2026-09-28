# M5 hybrid acceptance tracking

M5 is **in progress**. Implementation-plan §6.6 requires seven bridge kinds;
§7.5 and the milestone acceptance also require hybrid convergence and exact traces.
Existing bounded integrations are useful evidence, but do not complete general
typed bridge support.

| §7.5 acceptance model | Evidence | Remaining work |
|---|---|---|
| Bass ABM/SD | [Six population curves](HYBRID_BASS_MEAN_FIELD.md), N=40/160/640 at two steps, 4,608 runs per stochastic engine, 90 binomial-reference comparisons, 30 analytic and 30 finest-N mean gates; separate Euler/RK4 refinement against closed-form SD; plots and numeric gaps | Declared finite-case gate passed; fixed-dt population limit is explicitly discrete SD, with time-step convergence measured separately |
| SIR mean field | [Four frozen cases](HYBRID_SIR_MEAN_FIELD.md), N=40/160/640, 3,072 runs per stochastic engine, 168 independent comparisons, 84 mean gates, four curves, RK4 accuracy and stored plots/numeric gaps | Declared gate passed locally |
| DES→SD fluid limit | [Four reflected-fluid cases](HYBRID_DES_FLUID_LIMIT.md), N=8/32/128, 3,072 runs per stochastic engine, 180 birth/death comparisons, four scaling curves, exact pulse-stock accounting, 48 addressed snapshots and 120 declaration-order hand traces | Declared constant-rate single-server gate passed; general networks/time-varying fluid limits remain outside this evidence |
| Agent continuous stocks | [Native typed Euler bridge](HYBRID_AGENT_STOCKS.md): 108 closed-form snapshots, three refinement curves, 576 homogeneous pure-SD snapshots; [DEVS composition](HYBRID_AGENT_STOCKS_ATOMIC.md) adds 3,024 exact-rational snapshots across six declaration orders and four clock ratios | Declared native Euler/DEVS gate passed; [bounded declarative mode](AGENT_STOCK_SD_IR.md) adds 2,728 exact-rational scalar checks; [native lifecycle/pulse composition](HYBRID_DYNAMIC_AGENT_STOCKS.md) adds 7,872 independent snapshots; general declarative graph bindings remain |
| Agent-backed pool | [Typed workforce owner](HYBRID_TYPED_AGENT_POOL.md): static-pool grant equality, availability/lifecycle/phases, broker rollback and 21,168 exact pool/aggregate/SD snapshots | Declared native integer/single-pool gate passed; general declarative graph bindings remain |
| Event pulse | Exact scheduled values, on/off-grid completion, confluence and rollback tests | Existing bounded trace gate passed; preserve coverage as composition expands |

| §6.6 bridge kind | Current implementation | Work before general M5 acceptance |
|---|---|---|
| `aggregate` | [Typed snapshot DEVS publication](HYBRID_TYPED_AGGREGATE.md) supports five filtered reducers, explicit empty policies, scalar-driven SD and rollback; 1,008 exact-rational snapshots pass; sync/async ABM result adapter adds 336 exact lifecycle/command/timer snapshots across all 24 graph orders | Unit-checked expressions implemented in `agent_stock_sd`; general named graph bindings remain |
| `pulse` | Entity and staffed-delivery completion pulses to clocked SD; signed arrival/completion pulses preserve an exact queue backlog stock; bounded declarative paths | [Native typed event projections](HYBRID_TYPED_PULSES.md) and signed vectors implemented; general unit-checked declarative bindings remain |
| `sd_driven_rate` | Stock-triggered source and piecewise-constant Poisson source with preserved residual hazard | [Native scalar projection/source/routing](HYBRID_SIGNAL_RATES.md) implemented with 9,408 coupled runs; general unit-checked declarative bindings remain |
| `agent_stock` | Value-owned per-agent Euler stocks and agent/global transfers, plus autonomous DEVS snapshot publication into aggregate/SD endpoints; conservation, pure-SD equivalence, exact-rational clock traces and rollback | Typed expressions and bounded IR implemented; native lifecycle/pulse ordering and receipts now pass 7,872 rational-recurrence snapshots; general graph bindings remain and higher-order stage semantics are outside the initial Euler scope |
| `spawn/despawn` | [Typed DES event lifecycle bridge](HYBRID_LIFECYCLE_BRIDGE.md), destination/replay checks, population-owned allocation/cancellation; 8,064 independent snapshots | General declarative wiring; shared process/entity ownership is implemented in the dedicated bridge |
| `agent_pool` | [Typed population/pool owner](HYBRID_TYPED_AGENT_POOL.md), reserved allocation field, lifecycle/phase controls, transactional broker handlers and typed DES/aggregate composition; bounded legacy declarative subset | General declarative graphs; multi-pool staffing and automatic behavior clocks are outside the declared initial native scope |
| `entity_agent` | [Shared owner](HYBRID_ENTITY_AGENT.md), immutable dispatch snapshots, statechart return actions and one-journey ledger; 18,000 independent snapshots across 720 configurations | General declarative wiring; revisits/cancellation remain outside initial one-journey scope |

Next: review native M5 acceptance against all seven bridge kinds, then general graph bindings and the M6 model-loader contract.
M2 source-history work and M6–M8 remain separate. Correctness comes before
acceleration; no Tenstorrent work starts from this checkpoint.
