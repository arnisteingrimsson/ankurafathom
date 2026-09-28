#ifndef ANKURAFATHOM_C_API_H
#define ANKURAFATHOM_C_API_H
#include <stddef.h>
#include <stdint.h>
#include "ankurafathom/arrow_c.h"

#if defined(_WIN32)
# if defined(FATHOM_BUILDING_API)
#  define FATHOM_API __declspec(dllexport)
# else
#  define FATHOM_API __declspec(dllimport)
# endif
#else
# define FATHOM_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define FATHOM_ABI_VERSION UINT32_C(1)
typedef int32_t fathom_status;
#define FATHOM_OK INT32_C(0)
#define FATHOM_INVALID_ARGUMENT INT32_C(1)
#define FATHOM_IR_ERROR INT32_C(2)
#define FATHOM_RUNTIME_ERROR INT32_C(3)
#define FATHOM_OUT_OF_MEMORY INT32_C(4)
#define FATHOM_OUT_OF_RANGE INT32_C(5)
#define FATHOM_ABI_MISMATCH INT32_C(6)
#define FATHOM_UNAVAILABLE INT32_C(7)
#define FATHOM_CANCELLED INT32_C(8)

typedef struct fathom_model fathom_model;
typedef struct fathom_results fathom_results;
/* Thread-local, no allocation. Truncation bits: code=1, pointer=2, message=4.
 * Code/pointer prefixes stop at embedded NUL too and flag that truncation.
 * Successful status-returning calls clear this record. Version/error queries
 * and free calls preserve it. The pointer remains valid on the calling thread. */
typedef struct fathom_error {
    fathom_status status;
    uint32_t truncated;
    char code[64];
    char pointer[512];
    char message[1024];
} fathom_error;
typedef struct fathom_run_options {
    uint32_t abi_version;
    uint32_t threads;
    uint64_t seed;
    uint32_t override_seed;
    uint32_t reserved;
} fathom_run_options;
#define FATHOM_RUN_OPTIONS_INIT { FATHOM_ABI_VERSION, 1, 0, 0, 0 }

/* Called synchronously on the run's calling thread with completed=0..total,
 * in canonical trajectory order. Return 1 to continue, 0 to cancel; all other
 * values fail with INVALID_ARGUMENT. No exceptions/longjmp across this boundary.
 * Callback/context must remain valid until run returns. Callbacks may invoke
 * other API calls with distinct output storage, but must not free active handles.
 * Cancellation is cooperative between trajectories: active workers finish and
 * join before return. No partial result is published, even when cancelled at
 * completed==total. Progress measures trajectories, not elapsed time/export. */
typedef int32_t (*fathom_progress_callback)(uint64_t completed, uint64_t total, void* context);
typedef struct fathom_run_callbacks {
    uint32_t abi_version;
    uint32_t reserved;
    fathom_progress_callback progress;
    void* context;
} fathom_run_callbacks;
#define FATHOM_RUN_CALLBACKS_INIT { FATHOM_ABI_VERSION, 0, NULL, NULL }

typedef struct fathom_observation {
    uint32_t scenario;
    uint32_t replication;
    double time;
    double value;
    const char* output_id;
    size_t output_id_size;
} fathom_observation;

FATHOM_API uint32_t fathom_abi_version(void);
FATHOM_API const fathom_error* fathom_last_error(void);
/* Creation destinations must point to NULL. They remain NULL on failure.
 * JSON inputs are byte spans (no terminator required), limited to 256 MiB.
 * File/base paths are NUL-terminated. base_directory is optional for inline
 * models, required and absolute when data bindings are present. Inputs are
 * copied; bound data snapshots belong to the model after a successful load. */
FATHOM_API fathom_status fathom_load_json(const char* json, size_t size,
    const char* base_directory, fathom_model** model);
FATHOM_API fathom_status fathom_load_file(const char* path, fathom_model** model);
/* NULL experiment + zero size means single run. Otherwise use the CLI experiment
 * JSON schema (explicit/grid/LHS/Sobol). NULL options means default options.
 * override_seed must be 0 or 1; seed must be 0 when override_seed is 0.
 * Results own observations independently of the model and caller buffers. */
FATHOM_API fathom_status fathom_run(const fathom_model* model,
    const char* experiment_json, size_t experiment_size,
    const fathom_run_options* options, fathom_results** results);
/* Additive ABI-1 entry point. NULL callbacks equals fathom_run. Validation
 * precedes callbacks; a single run reports (0,1), then (1,1). */
FATHOM_API fathom_status fathom_run_with_callbacks(const fathom_model* model,
    const char* experiment_json, size_t experiment_size,
    const fathom_run_options* options, const fathom_run_callbacks* callbacks,
    fathom_results** results);
FATHOM_API fathom_status fathom_results_size(const fathom_results* results,
    uint64_t* trajectories, uint64_t* rows);
/* Borrowed output IDs and SHA-256 strings live until results_free. Getters do
 * not invalidate them. Output arguments are unchanged on failure. */
FATHOM_API fathom_status fathom_results_observation(const fathom_results* results,
    uint64_t index, fathom_observation* observation);
FATHOM_API fathom_status fathom_results_sha256(const fathom_results* results, const char** digest);
/* Export an independently owned CPU Arrow C Stream (observation schema 0.1).
 * Destination must be initialized with release == NULL; unchanged on failure.
 * Release stream, schemas and arrays through their own Arrow release callbacks.
 * Model/results may be freed after successful export. Returned arrays/schemas
 * also outlive the stream. Serialize operations on each stream. Stream callbacks
 * use Arrow errno/last-error semantics, not fathom_last_error(). Keep the shared
 * library loaded until all exported objects have been released.
 * Returns FATHOM_UNAVAILABLE in builds without Arrow. */
FATHOM_API fathom_status fathom_results_arrow(const fathom_results* results,
    struct ArrowArrayStream* stream);
/* Borrowed, NUL-terminated manifest JSON; size excludes the terminator.
 * Captured once per completed run, valid until results_free, also without Arrow.
 * Version 0.3 records in-memory output and empty paths for in-memory JSON inputs.
 * This receipt is not currently replayable. Outputs are unchanged on failure. */
FATHOM_API fathom_status fathom_results_manifest(const fathom_results* results,
    const char** json, size_t* size);
/* Same ownership rules as results_arrow, with schema 0.2 lineage columns and
 * the exact results_manifest receipt embedded in schema metadata. */
FATHOM_API fathom_status fathom_results_arrow_with_manifest(const fathom_results* results,
    struct ArrowArrayStream* stream);
/* NULL-safe. Non-NULL handles must be live, correctly typed library handles;
 * free each exactly once, after all concurrent uses finish. Immutable handles
 * support concurrent run/read calls; caller output storage must be distinct. */
FATHOM_API void fathom_model_free(fathom_model* model);
FATHOM_API void fathom_results_free(fathom_results* results);

#ifdef __cplusplus
}
#endif
#endif
