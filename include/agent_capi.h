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
 * ABI version is bumped on every breaking change. v4 is the first boundary
 * intended for general cross-language bindings rather than C smoke coverage
 * only.
 */

#ifndef AGENT_CAPI_H
#define AGENT_CAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_CAPI_ABI_VERSION 4

#define AGENT_STATUS_OK 0
#define AGENT_STATUS_FRAMEWORK_ERROR 1
#define AGENT_STATUS_STD_EXCEPTION 2
#define AGENT_STATUS_UNKNOWN_ERROR 3

typedef struct agent_error_t agent_error_t;
typedef struct agent_runner_t agent_runner_t;
typedef struct agent_runner_event_stream_t agent_runner_event_stream_t;
typedef struct agent_cancellation_t agent_cancellation_t;
typedef struct agent_run_t agent_run_t;
typedef struct agent_host_runtime_t agent_host_runtime_t;
typedef struct agent_tool_run_runtime_t agent_tool_run_runtime_t;

/**
 * Host model callbacks receive UTF-8 JSON requests and may return UTF-8 JSON
 * through an owned string. Allocate returned strings with agent_string_clone or
 * another malloc-compatible allocator; AgentCore releases them with
 * agent_string_free.
 */
typedef int32_t (*agent_host_model_generate_json_fn)(const char* request_json,
                                                     char** out_response_json,
                                                     void* user_data);
typedef int32_t (*agent_host_cancelled_fn)(void* user_data);

typedef struct agent_host_vtable_t {
  size_t size;
  void* user_data;
  agent_host_model_generate_json_fn model_generate_json;
  agent_host_cancelled_fn cancelled;
} agent_host_vtable_t;

/* ----- Errors and memory ------------------------------------------------ */

/** Last error message produced by any agent_* call on the current thread. */
const char* agent_last_error(void);

/** Free a string returned by the shim (e.g. result JSON, version string). */
void agent_string_free(char* str);

/** Allocate an owned C string with the shim allocator. Free with agent_string_free. */
char* agent_string_clone(const char* str);

/** Returns the framework version string (do NOT free). */
const char* agent_version(void);

/** Returns the ABI version of this shim. */
int32_t agent_capi_abi_version(void);

/**
 * Negotiate the ABI version supported by this binary. v3 intentionally has no
 * compatibility promise with older major ABI shapes; pass min=max=4 from new
 * bindings.
 */
int32_t agent_capi_negotiate_abi_version(int32_t min_version,
                                         int32_t max_version,
                                         int32_t* out_version);

/** Returns owned JSON version metadata. Free with agent_string_free. */
int32_t agent_capi_version_info_json(char** out_version_json);

/**
 * Returns a borrowed UTF-8 JSON document describing the shipped C ABI surface:
 * version metadata, constructor names, status codes, and the serialized run /
 * stream result shapes exposed by this shim. Do NOT free the returned pointer.
 */
const char* agent_capi_contract_json(void);

/** Copy the thread-local last error into an owned error object. */
int32_t agent_last_error_object(agent_error_t** out_error);

/** Release an error object. Safe to call with NULL. */
void agent_error_release(agent_error_t* error);

/** Numeric status associated with an error object. */
int32_t agent_error_code(const agent_error_t* error);

/** Stable error type, such as ConfigurationError or CancellationError. */
const char* agent_error_type(const agent_error_t* error);

/** Human-readable error message. */
const char* agent_error_message(const agent_error_t* error);

/** Serialize an error object as owned JSON. Free with agent_string_free. */
int32_t agent_error_json(const agent_error_t* error, char** out_error_json);

/* ----- Cancellation ----------------------------------------------------- */

/** Create a shared cancellation handle for sync, stream, and async operations. */
int32_t agent_cancellation_create(agent_cancellation_t** out_cancellation);

/** Request cancellation with an optional UTF-8 reason. Safe to call with NULL. */
void agent_cancellation_cancel(agent_cancellation_t* cancellation, const char* reason);

/** Returns 1 when cancelled, 0 when active or NULL. */
int32_t agent_cancellation_cancelled(agent_cancellation_t* cancellation);

/** Borrowed cancellation reason. Empty string when not cancelled or NULL. */
const char* agent_cancellation_reason(agent_cancellation_t* cancellation);

/** Release a cancellation handle. Safe to call with NULL. */
void agent_cancellation_release(agent_cancellation_t* cancellation);

/* ----- Host Runtime ----------------------------------------------------- */

/**
 * Create a host runtime from language-owned callbacks. The runtime handle owns
 * a copy of the vtable metadata; user_data remains host-owned.
 */
int32_t agent_host_runtime_create(const agent_host_vtable_t* vtable,
                                  agent_host_runtime_t** out_runtime);

/** Release a host runtime. Existing runners keep their own shared runtime copy. */
void agent_host_runtime_release(agent_host_runtime_t* runtime);

/** Return owned JSON describing which host model callbacks are present. */
int32_t agent_host_runtime_describe_json(agent_host_runtime_t* runtime,
                                         char** out_description_json);

/**
 * Create a runner backed by the host model callback.
 *
 * model_json may be NULL or { provider?, model?, temperature?,
 * maxOutputTokens?, capabilities?: string[] }. Host model callbacks receive
 * the framework's canonical AgentOutput JSON shape and should return the same
 * shape, or a minimal { "text": "..." } object.
 */
int32_t agent_runner_create_with_host_model(agent_host_runtime_t* runtime,
                                            const char* model_json,
                                            agent_runner_t** out_runner);

/* ----- Runner ----------------------------------------------------------- */

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

/** Synchronous text run using an explicit cancellation handle. */
int32_t agent_runner_run_with_cancellation(agent_runner_t* runner,
                                           const char* input,
                                           const char* session_id,
                                           agent_cancellation_t* cancellation,
                                           char** out_result_json);

/**
 * Synchronously run the agent with JSON input.
 *
 * `input_json` may be a JSON string, a JSON array of content parts, or a JSON
 * object representing an `AgentMessage`. Objects without `role`/`content`
 * fields are rejected so the boundary remains explicit and stable.
 *
 * Free *out_result_json with agent_string_free.
 */
int32_t agent_runner_run_json(agent_runner_t* runner,
                              const char* input_json,
                              const char* session_id,
                              char** out_result_json);

/** Synchronous JSON run using an explicit cancellation handle. */
int32_t agent_runner_run_json_with_cancellation(agent_runner_t* runner,
                                                const char* input_json,
                                                const char* session_id,
                                                agent_cancellation_t* cancellation,
                                                char** out_result_json);

/**
 * Stream callback. Receives one JSON-encoded runner event per invocation.
 * Return non-zero to abort streaming early.
 *
 * The event JSON shape is at minimum { "type": "<event-type>" } plus any
 * additional keys specific to the event (e.g. "delta" for model text deltas,
 * "iteration" for iteration start, etc.).
 */
typedef int32_t (*agent_stream_callback_t)(const char* event_json, void* user_data);

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

/** Synchronous text stream using an explicit cancellation handle. */
int32_t agent_runner_stream_with_cancellation(agent_runner_t* runner,
                                              const char* input,
                                              const char* session_id,
                                              agent_cancellation_t* cancellation,
                                              agent_stream_callback_t on_event,
                                              void* user_data);

/**
 * Synchronously stream the agent for JSON input.
 *
 * Input normalization follows the same rules as agent_runner_run_json.
 */
int32_t agent_runner_stream_json(agent_runner_t* runner,
                                 const char* input_json,
                                 const char* session_id,
                                 agent_stream_callback_t on_event,
                                 void* user_data);

/** Synchronous JSON stream using an explicit cancellation handle. */
int32_t agent_runner_stream_json_with_cancellation(agent_runner_t* runner,
                                                   const char* input_json,
                                                   const char* session_id,
                                                   agent_cancellation_t* cancellation,
                                                   agent_stream_callback_t on_event,
                                                   void* user_data);

/* ----- Async Run Handles ------------------------------------------------ */

/** Start one background text run. The returned handle owns the worker thread. */
int32_t agent_runner_run_async(agent_runner_t* runner,
                               const char* input,
                               const char* session_id,
                               agent_cancellation_t* cancellation,
                               agent_run_t** out_run);

/** Start one background JSON run. */
int32_t agent_runner_run_json_async(agent_runner_t* runner,
                                    const char* input_json,
                                    const char* session_id,
                                    agent_cancellation_t* cancellation,
                                    agent_run_t** out_run);

/**
 * Wait for the background run to finish and return the owned result JSON.
 * Returns the worker status when the run failed or was cancelled.
 */
int32_t agent_run_wait_json(agent_run_t* run, char** out_result_json);

/**
 * Poll a background run without blocking. When done, out_done=1 and
 * out_result_json owns the result JSON on success.
 */
int32_t agent_run_try_get_json(agent_run_t* run,
                               char** out_result_json,
                               int32_t* out_done);

/** Request cancellation for a background run. Safe to call with NULL. */
void agent_run_cancel(agent_run_t* run, const char* reason);

/** Release a run handle, joining the worker thread if needed. */
void agent_run_release(agent_run_t* run);

/**
 * Open a pull-based runner event stream for UTF-8 text input.
 *
 * `capacity` is the bounded queue capacity. Passing 0 uses the framework
 * default. Language bindings should expose this as an async iterator where
 * each await calls agent_runner_event_stream_next_json.
 */
int32_t agent_runner_stream_events(agent_runner_t* runner,
                                   const char* input,
                                   const char* session_id,
                                   size_t capacity,
                                   agent_runner_event_stream_t** out_stream);

/**
 * Open a pull-based runner event stream for JSON input.
 *
 * Input normalization follows the same rules as agent_runner_run_json.
 */
int32_t agent_runner_stream_events_json(agent_runner_t* runner,
                                        const char* input_json,
                                        const char* session_id,
                                        size_t capacity,
                                        agent_runner_event_stream_t** out_stream);

/**
 * Pull the next JSON-encoded stream event.
 *
 * On success, `*out_has_event` is set to 1 and `*out_event_json` owns a string
 * that must be freed with agent_string_free. When the stream is exhausted,
 * `*out_has_event` is set to 0 and `*out_event_json` is NULL.
 */
int32_t agent_runner_event_stream_next_json(agent_runner_event_stream_t* stream,
                                            char** out_event_json,
                                            int32_t* out_has_event);

/** Cancel a pull stream early with an optional UTF-8 reason. Safe to call with NULL. */
void agent_runner_event_stream_cancel(agent_runner_event_stream_t* stream,
                                      const char* reason);

/** Close a pull stream early. Safe to call with NULL. */
void agent_runner_event_stream_close(agent_runner_event_stream_t* stream);

/** Release a pull stream handle. Safe to call with NULL. */
void agent_runner_event_stream_release(agent_runner_event_stream_t* stream);

/* ----- Tool Runs / Background Tasks ------------------------------------ */

/**
 * Create a language-neutral in-memory ToolRun runtime. This runtime is not
 * tied to shell commands or AgentRunner; it manages generic/custom background
 * work handles for C, C#, Rust, and other bindings through JSON.
 */
int32_t agent_tool_run_runtime_create(agent_tool_run_runtime_t** out_runtime);

/** Release a ToolRun runtime. Safe to call with NULL. */
void agent_tool_run_runtime_release(agent_tool_run_runtime_t* runtime);

/**
 * Start a custom/background tool run from JSON:
 * { runId?, toolCallId?, toolName?, kind?, label?, status?, ready?, metadata? }
 *
 * The returned JSON is a ToolRun snapshot with language-neutral fields:
 * runId/toolCallId/toolName/kind/label/status/startedAt/updatedAt/finishedAt/
 * ready/error/metadata.
 */
int32_t agent_tool_run_start_json(agent_tool_run_runtime_t* runtime,
                                  const char* start_json,
                                  char** out_snapshot_json);

/** Read one ToolRun snapshot by run id. */
int32_t agent_tool_run_status_json(agent_tool_run_runtime_t* runtime,
                                   const char* run_id,
                                   char** out_snapshot_json);

/** List ToolRuns. filter_json may be NULL or { status?, kind?, toolName? }. */
int32_t agent_tool_run_list_json(agent_tool_run_runtime_t* runtime,
                                 const char* filter_json,
                                 char** out_runs_json);

/**
 * Update a ToolRun snapshot:
 * { status?, ready?, error?, metadata?, mergeMetadata? }
 */
int32_t agent_tool_run_update_json(agent_tool_run_runtime_t* runtime,
                                   const char* run_id,
                                   const char* update_json,
                                   char** out_snapshot_json);

/**
 * Append one event/log record:
 * { type?, stream?, text?, message?, payload?, metadata? }
 */
int32_t agent_tool_run_append_event_json(agent_tool_run_runtime_t* runtime,
                                         const char* run_id,
                                         const char* event_json,
                                         char** out_event_json);

/** Read ToolRun events. read_json may be NULL or { cursor?, limit?, tail? }. */
int32_t agent_tool_run_read_json(agent_tool_run_runtime_t* runtime,
                                 const char* run_id,
                                 const char* read_json,
                                 char** out_read_json);

/** Cancel a non-terminal ToolRun. cancel_json may be NULL or { reason? }. */
int32_t agent_tool_run_cancel_json(agent_tool_run_runtime_t* runtime,
                                   const char* run_id,
                                   const char* cancel_json,
                                   char** out_snapshot_json);

/**
 * Wait for a ToolRun. wait_json may be NULL or { until?: "ready"|"terminal",
 * timeoutMs? }. A timeout returns JSON null rather than an error.
 */
int32_t agent_tool_run_wait_json(agent_tool_run_runtime_t* runtime,
                                 const char* run_id,
                                 const char* wait_json,
                                 char** out_snapshot_json);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_CAPI_H */
