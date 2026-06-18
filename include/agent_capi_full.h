/*
 * Native Agent Framework — full C ABI extensions.
 *
 * Include this header, and link agent_capi_full, when a binding wants the
 * batteries-included app/config and async-agent-run modules. The default
 * agent_capi.h header stays limited to the embeddable runner ABI.
 */

#ifndef AGENT_CAPI_FULL_H
#define AGENT_CAPI_FULL_H

#include "agent_capi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_async_runtime_t agent_async_runtime_t;

/**
 * Create a runner from a native JSON config document.
 *
 * `config_json` must be a UTF-8 JSON object accepted by the native config
 * loader. `requested_agent_id` may be NULL to resolve the default agent.
 * Relative paths inside the config are resolved from the host process current
 * working directory because no config file path is available at this boundary.
 */
int32_t agent_runner_create_from_config_json(const char* config_json,
                                             const char* requested_agent_id,
                                             agent_runner_t** out_runner);

/**
 * Create a runner from a native config file or config directory path.
 *
 * `config_path` may point at a supported JSON config file or a directory to
 * search for the default config file. JS/TS module configs are rejected at
 * this boundary because the C shim does not accept a module loader callback.
 */
int32_t agent_runner_create_from_config_path(const char* config_path,
                                             const char* requested_agent_id,
                                             agent_runner_t** out_runner);

/**
 * Create a language-neutral async-run runtime backed by in-memory task,
 * queue, and transcript stores. Production hosts that need durable storage
 * should use the C++ AsyncAgentRunStore API or the server routes with an
 * injected store adapter.
 */
int32_t agent_async_runtime_create(agent_runner_t* runner,
                                   agent_async_runtime_t** out_runtime);

/** Release an async runtime. Safe to call with NULL. */
void agent_async_runtime_release(agent_async_runtime_t* runtime);

/**
 * Start an async agent run from JSON:
 * { input, sessionId?, modelSettings?, retrievalOptions?, writebackOptions?,
 *   skillActivations?, context?, knowledgeRetrievalOptions?, enablePlanning?,
 *   idempotencyKey?, ownerApiKeyId?, tenantId?, metadata? }
 *
 * The returned JSON is an AsyncAgentRunSnapshot.
 */
int32_t agent_async_run_start_json(agent_async_runtime_t* runtime,
                                   const char* start_json,
                                   char** out_snapshot_json);

/** Claim and execute at most one queued async run. */
int32_t agent_async_runtime_run_once(agent_async_runtime_t* runtime,
                                     int32_t* out_processed);

/** Read one AsyncAgentRunSnapshot by run id. */
int32_t agent_async_run_get_json(agent_async_runtime_t* runtime,
                                 const char* run_id,
                                 char** out_snapshot_json);

/** List async runs. filter_json may be NULL or { status?, ownerApiKeyId?, tenantId? }. */
int32_t agent_async_run_list_json(agent_async_runtime_t* runtime,
                                  const char* filter_json,
                                  char** out_runs_json);

/** Read events for one run. */
int32_t agent_async_run_events_json(agent_async_runtime_t* runtime,
                                    const char* run_id,
                                    char** out_events_json);

/** Read checkpoints for one run. */
int32_t agent_async_run_checkpoints_json(agent_async_runtime_t* runtime,
                                         const char* run_id,
                                         char** out_checkpoints_json);

/** Read transcript entries for one run. */
int32_t agent_async_run_transcript_json(agent_async_runtime_t* runtime,
                                        const char* run_id,
                                        char** out_transcript_json);

/** Resume a queued/waiting/failed/cancelled run. resume_json may be NULL. */
int32_t agent_async_run_resume_json(agent_async_runtime_t* runtime,
                                    const char* run_id,
                                    const char* resume_json,
                                    char** out_snapshot_json);

/** Cancel a queued or running async run. cancel_json may be NULL. */
int32_t agent_async_run_cancel_json(agent_async_runtime_t* runtime,
                                    const char* run_id,
                                    const char* cancel_json,
                                    char** out_snapshot_json);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_CAPI_FULL_H */
