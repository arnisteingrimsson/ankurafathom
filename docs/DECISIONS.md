# Decisions

## ADR-A01 — Begin with a standalone economics reference

Date: 2026-09-23

Context: The general DEVS/SD/ABM/DES platform plan would otherwise produce its first Ankura decision model after the kernel and every vocabulary were complete. That delays discovery of errors in fee-model and demand economics.

Decision: Implement a small deterministic C++ reference model first, with synthetic practices and a standalone `fathom` CLI. Phase 1 must reproduce its business invariants in the general runtime. This reference is not presented as a formal DEVS implementation or a causal estimate.

Consequence: The project gets executable accounting and scenario tests early. The initial CLI uses a narrow CSV input and output contract; the planned JSON IR, Arrow outputs, and formal kernel follow in later milestones.

## ADR-A04 — Pin adevs as a reference oracle

Date: 2026-09-23

Decision: Vendor a source snapshot of `smiz/adevs` at commit `ee9fed91224ed412088a3795ea8e67ffe2ee9b59` under `third_party/adevs`, retaining its copyright notice. Build it as `adevs_reference` and run an upstream test. Our kernel remains a separate implementation and will be compared against the pinned reference with event traces.

Consequence: The formal kernel has a concrete, reproducible upstream baseline. This is a local vendor snapshot, not a GitHub-hosted fork, and the current economics simulator remains independent of adevs.

## ADR-A02 — Name and integration boundary

Date: 2026-09-23

Decision: The platform is AnkuraFathom, with `fathom` as its CLI and `ankurafathom` as its code namespace. It runs independently. Connections to other platforms are deferred.

## ADR-A03 — Phase A labor economics

Date: 2026-09-23

Decision: Store T&M and fixed-fee backlog in baseline work-hours. Productivity lowers actual hours required to deliver each baseline hour. T&M bills actual hours; fixed-fee recognizes contracted value as baseline work is delivered. Payroll is fixed for the current workforce; variable delivery cost depends on actual hours. Interventions multiply their remaining labor factors rather than adding percentage gains.

Consequence: Saved T&M hours cannot turn into revenue without additional work. Fixed-fee productivity can reduce variable cost at unchanged contract value. The result depends materially on synthetic task shares, demand, and costs, all of which must become explicit calibrated or assumed inputs in later phases.

## ADR-A05 — Isolate pinned validation engines

Date: 2026-09-25

SimPy and Ciw provide independent DES scheduling/queue oracles; their NumPy,
NetworkX, tqdm, and setuptools dependencies are pinned with them under
`tests/oracles/des/requirements.txt`. They are used only for reference generation;
the production C++ runtime and routine offline comparison have no Python-engine
dependency. PySD and its pinned dependencies provide XMILE source references;
SciPy/NumPy provide high-accuracy independent SD reference trajectories. Existing
JSON Schema tooling independently checks the declarative contract. These packages
remain in isolated oracle environments and dedicated CI regeneration steps.

## ADR-A06 — Extend the existing router with an explicit probability rule

Date: 2026-09-25

The current binary `router` gains a mutually exclusive addressed probability
rule. It retains its ports, metadata, counters, and DEVS publication contract.
This is an incremental implementation of the plan's `select_output` vocabulary;
multiway rules, expression conditions, and repeated visits remain separate
increments. Explicit stream ownership is checked at load time rather than
allocated from declaration order.

## ADR-A07 — Pin Mesa as an isolated ABM oracle

Date: 2026-09-25

Mesa and its pinned numerical/network dependencies provide independent M4 space and model references in an isolated oracle environment; they are not production runtime dependencies. The chosen version and exact transitive pins are recorded with each reference before comparisons.

## ADR-A08 — Bound expression source and recursive structure

Date: 2026-09-27

The first sanitizer fuzz corpus reproduced a stack overflow while evaluating a
long expression. Limit expression source to 65,536 bytes and both recursive
parser nesting and AST depth to 256. Reject before creating an oversized tree,
including flat binary chains, so evaluation, unit inference and destruction have
the same bound. Parentheses and unary plus count toward parser nesting even when
they do not add AST nodes. Limits raise `invalid_argument`, mapped by IR loading
to the existing expression diagnostic. Ordinary arithmetic order stays unchanged.
This is a correctness/resource contract for the reference interpreter, not a
performance optimization; models exceeding it must split expressions into auxiliaries.

## ADR-A09 — Pure bounded conditional expressions for pilot economics

Date: 2026-09-27

Expose the planned min/max/conditional operations as `MIN(a,b)`, `MAX(a,b)` and
`IF_POSITIVE(condition,then,else)` in the current function grammar. The conditional
requires a dimensionless condition, chooses `then` exactly when it is positive,
and evaluates only the chosen branch. Both branches must have identical inferred
units. This permits exact zero-demand/zero-capacity economics without epsilon
denominators. It does not add general code, loops or randomness; the eventual
infix conditional/comparison grammar remains separate. The pilot can now express
the reference equations declaratively instead of calling the economics engine.
