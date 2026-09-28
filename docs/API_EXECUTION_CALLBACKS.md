# Execution progress and cancellation

The C and Python APIs expose the native runner's ordered trajectory boundaries.
No callback changes simulation clocks, random addresses, observation order or
numeric identities. This is cooperative cancellation between trajectories;
running trajectories finish before cancellation returns.

## Python

```python
import threading
from ankurafathom import run, FathomError

stop = threading.Event()  # Another application thread may call stop.set().

def progress(completed, total):
    print(f"{completed}/{total} trajectories")
    return not stop.is_set()

try:
    table = run(model, experiment, threads=8, provenance=True, progress=progress)
except FathomError as error:
    if error.status != 8:  # FATHOM_CANCELLED
        raise
```

`progress=None` keeps the existing behavior. A callback receives two Python ints
and must return an actual `bool`: `True` continues and `False` cancels. Other
return types raise `TypeError`; accidental truthiness is not accepted. A callback
exception, including `KeyboardInterrupt` or `SystemExit`, is re-raised with its
original object and traceback after worker cleanup. Exceptions do not cross C.

Callbacks execute synchronously on the thread that called `run`, with the GIL
held. Native execution and worker joins release the GIL. Callbacks may invoke
other API operations, including nested runs on the same immutable model. Each
concurrent run owns its callback state. Application callbacks must avoid waiting
on operations that themselves require that callback to return.

## C (ABI 1)

```c
static int32_t progress(uint64_t completed, uint64_t total, void* context) {
    (void)completed;
    (void)total;
    (void)context;
    return 1; /* 1 continues, 0 cancels */
}

fathom_run_callbacks hooks = FATHOM_RUN_CALLBACKS_INIT;
hooks.progress = progress;
hooks.context = NULL;
fathom_results* results = NULL;
fathom_status status = fathom_run_with_callbacks(
    model, NULL, 0, NULL, &hooks, &results);
/* On success, consume and free results. On cancellation results stays NULL. */
```

The new entry point is additive; existing `fathom_run`, run-options layout and
ABI major remain unchanged. `callbacks=NULL` or a null `progress` member disables
callbacks. Callback configuration is copied for the call; callback code/context
must remain valid until it returns. The callback must not throw or `longjmp`
across the C boundary. Returns other than 0 or 1 fail with
`FATHOM_INVALID_ARGUMENT`. Callback ABI mismatch and reserved flags reject before
any callback runs. Shared callback contexts need caller-provided synchronization.

Cancellation returns `FATHOM_CANCELLED` (8), with code `FATHOM_CANCELLED` in the
thread-local diagnostic. Other API calls within callbacks may update that record;
the outer run's completion replaces it with its own final status. Do not free
handles still used by an active call or share writable output storage between
nested/concurrent calls.

## Boundaries and publication

After argument and experiment validation, callbacks report `(0, total)`, then
`(1, total)` through `(total, total)` as trajectories are merged in canonical
scenario/replication order. This sequence is independent of worker scheduling.
A single run has total 1. `total` counts expanded scenarios times replications,
not observations or elapsed time. Failed trajectories publish no progress for
themselves; preceding successful trajectories may already have been reported.

Returning cancel at zero prevents trajectory execution. Cancelling later stops
further publication and joins active workers; other work may already have run.
No partial C result or Python table escapes. Cancelling even at the final boundary
discards the result. Retrying the immutable model starts a fresh run.

Final progress means trajectories completed, not that receipt construction or
Arrow export succeeded. There are no callbacks during loading, individual model
steps, manifest construction, or export. A long trajectory can therefore delay
cancellation; no cancellation-latency bound or asynchronous native cancellation
handle is claimed. Python signal responsiveness outside callbacks is unchanged.
CSV-only builds run the same callbacks; a completed Python run then reports Arrow
unavailable, while cancellation or callback exceptions return before export.

## Verification

The C11 contract checks calling-thread identity, every boundary for single and
ensemble execution at 1/8/32 workers, NULL result publication, numeric hash parity,
invalid callbacks/options, nested calls and final diagnostic replacement. The
Python contract checks the same ordering with strict bool results, original
exception objects/tracebacks, runtime-failure prefixes, retry and concurrent use.
Exact binary64 and receipt identities are compared across SD, DES, typed ABM,
agent stocks, agent pools, hybrid and the three data bindings; all scenario-design
callback totals are checked. See the session log for executed build coverage.
