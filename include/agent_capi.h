/*
 * Native Agent Framework — C ABI shim.
 *
 * This header is the stable boundary for embedding the C++ framework from any
 * language that has a C FFI (Python, Go, Rust, Java, .NET, Node addons, …).
 *
 * Conventions:
 *  - Opaque handles are returned by *_create and freed by *_release.
 *  - Strings are UTF-8 null-terminated C strings.
 *  - JSON payloads are UTF-8 JSON encoded as null-terminated C strings.
 *  - Functions returning a pointer through an out-parameter give ownership
 *    to the caller; release using the matching *_release or
 *    agent_string_free function.
 *  - All non-void entry points return an int32_t status code:
 *      0  -> success
 *      1  -> AgentFrameworkError
 *      2  -> std::exception
 *      3  -> unknown error
 *    On non-zero return, call agent_last_error() for a thread-local message.
 *  - No C++ exceptions propagate across the C boundary.
 *
 * ABI version is bumped on every breaking change. New functionality is added
 * as new functions, not by changing existing signatures.
 */

#ifndef AGENT_CAPI_H
#define AGENT_CAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_CAPI_ABI_VERSION 1

/* ----- Errors and memory ------------------------------------------------ */

/** Last error message produced by any agent_* call on the current thread. */
const char* agent_last_error(void);

/** Free a string returned by the shim (e.g. result JSON, version string). */
void agent_string_free(char* str);

/** Returns the framework version string (do NOT free). */
const char* agent_version(void);

/** Returns the ABI version of this shim. */
int32_t agent_capi_abi_version(void);

/* ----- Runner ----------------------------------------------------------- */

typedef struct agent_runner_t agent_runner_t;

/**
 * Create a runner that uses the built-in echo chat model. Tools are empty.
 * Intended for smoke tests, examples, and binding integration tests with no
 * external provider configuration required.
 */
int32_t agent_runner_create_with_echo_model(agent_runner_t** out_runner);

/**
 * Release a runner created by any agent_runner_create_* function.
 * Safe to call with NULL.
 */
void agent_runner_release(agent_runner_t* runner);

/**
 * Synchronously run the agent with the given UTF-8 input string.
 *
 * `session_id` may be NULL for the default session. The result is written to
 * `*out_result_json` as a JSON object with keys:
 *   { "sessionId": "...", "text": "...", "iterationCount": <int>,
 *     "terminationReason": "..." }
 *
 * Free *out_result_json with agent_string_free.
 */
int32_t agent_runner_run(agent_runner_t* runner,
                         const char* input,
                         const char* session_id,
                         char** out_result_json);

/**
 * Stream callback. Receives one JSON-encoded runner event per invocation.
 * Return non-zero to abort streaming early.
 *
 * The event JSON shape is at minimum { "type": "<event-type>" } plus any
 * additional keys specific to the event (e.g. "delta" for model text deltas,
 * "iteration" for iteration start, etc.).
 */
typedef int (*agent_stream_callback_t)(const char* event_json, void* user_data);

/**
 * Synchronously stream the agent for the given input. Events are forwarded
 * to `on_event` as they are produced. The terminal "done" event carries the
 * same payload returned by agent_runner_run.
 */
int32_t agent_runner_stream(agent_runner_t* runner,
                            const char* input,
                            const char* session_id,
                            agent_stream_callback_t on_event,
                            void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_CAPI_H */
