# M3 CPU DES acceptance checkpoint

The M3 process/resource core meets the numerical acceptance criteria in implementation-plan §7.2 on this machine. Full regression passes **107/107 normal tests** and **107/107 ASan/UBSan tests**. Schema conformance passes **39 valid fixtures, 225 structural invalid mutations, and 112 semantic invalid mutations**. Remote CI has not run; this workspace has no Git repository.

| Required area | Evidence | Result |
|---|---|---|
| M/M/1, M/M/c, M/G/1, finite-buffer queues | Frozen seed/replication plans, analytic mean/occupancy gates, individual conservation and area identities | Pass |
| Three-node open Jackson networks | Two tandem and two branched configurations; marginal/joint occupancy, means, and per-job conservation | Pass |
| Priority and non-preemptive seize/release | Hand schedules, 24 bag permutations, 96 independent resource traces, shared leases, safe resize, atomic failure/retry | Pass |
| Independent SimPy/Ciw comparisons | 444 statistical pair gates over twelve workloads; independent engines agree on 876,921 services and 141,345 rejections | Pass |
| Separate typed process blocks | Runtime column stores, source/queue/delay/seize/release/select/sink, pool adapter; 96 exact queue traces and 24 resource traces | Pass |
| Declarative typed graph | 120 pinned reference cases, 76,576 observations, maximum absolute error 2.84e-14; hand routing, generation, capacities, overrides, and declaration-order checks | Pass |

The statistical totals include 222 analytical gates over 672 preplanned native replications. Independent trace references retain hashes of plans, adapters, generators, and pinned dependencies. Each CTest reference comparison requires a fresh native report. Normal and sanitizer records are checked separately; no test outcome implies a production deployment.

The accepted scope includes acyclic typed graphs, immutable prepared entity fields, fixed/exponential arrival and service policies, FIFO/LIFO/priority queues, resource leases, and explicit capacity schedules. Preemption is rejected as required by Phase 1. Loops/revisits, general entity mutation, cancellation, named random-expression streams, and general typed hybrid graphs remain explicit extensions; this checkpoint does not claim the complete eventual expression language or platform is finished. Existing linear hybrid and agent-backed-pool paths retain their own tested contracts.

M4 proceeds with spatial/network/broker primitives, async populations, statecharts, and independent Mesa model comparisons. M2's unresolved source-history dialects remain separately open; M3 acceptance does not close them. See [typed DES usage](DES_TYPED_IR.md), [statistical evidence](DES_ENGINE_ORACLES.md), and [status](STATUS.md).
