/*
 * Round-trip smoke test for the agent_capi C ABI shim.
 *
 * Compiled as C99 — proves the header is genuinely C-compatible and that the
 * framework can be driven entirely through the C boundary that host
 * languages will use.
 */

#include "agent_capi_full.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_event_count = 0;
static int g_saw_done = 0;
static int g_aborted = 0;
static int g_json_event_count = 0;
static int g_json_saw_done = 0;

static int32_t host_model_generate(const char* request_json, char** out_response_json, void* user_data) {
  (void)user_data;
  if (request_json == NULL || strstr(request_json, "host hello") == NULL) {
    return 1;
  }
  *out_response_json = agent_string_clone(
      "{"
      "\"text\":\"Thought: answer through the host callback.\\nFinal Answer: host callback ok\","
      "\"usage\":{"
      "\"inputTokens\":3,"
      "\"outputTokens\":5,"
      "\"totalTokens\":8,"
      "\"inputTokensSource\":\"provider\","
      "\"outputTokensSource\":\"provider\","
      "\"totalTokensSource\":\"provider\","
      "\"quality\":\"provider\""
      "}"
      "}");
  return *out_response_json == NULL ? 2 : 0;
}

static int on_stream_event(const char* event_json, void* user_data) {
  (void)user_data;
  ++g_event_count;
  /* The framework's stream always finishes with a "done" event. */
  if (strstr(event_json, "\"type\":\"done\"") != NULL ||
      strstr(event_json, "\"type\": \"done\"") != NULL) {
    g_saw_done = 1;
  }
  return 0;
}

static int abort_after_first(const char* event_json, void* user_data) {
  (void)event_json;
  (void)user_data;
  g_aborted = 1;
  return 1; /* non-zero aborts the stream */
}

static int on_json_stream_event(const char* event_json, void* user_data) {
  (void)user_data;
  ++g_json_event_count;
  if (strstr(event_json, "\"type\":\"done\"") != NULL ||
      strstr(event_json, "\"type\": \"done\"") != NULL) {
    g_json_saw_done = 1;
  }
  return 0;
}

int main(void) {
  /* ----- ABI metadata ----- */
  if (agent_capi_abi_version() != 4) {
    fprintf(stderr, "unexpected ABI version: %d\n", agent_capi_abi_version());
    return 1;
  }
  int32_t negotiated = 0;
  if (agent_capi_negotiate_abi_version(4, 4, &negotiated) != 0 || negotiated != 4) {
    fprintf(stderr, "ABI negotiation failed: %s\n", agent_last_error());
    return 1;
  }
  if (agent_capi_negotiate_abi_version(1, 3, &negotiated) == 0) {
    fprintf(stderr, "expected incompatible ABI range to be rejected\n");
    return 1;
  }

  const char* version = agent_version();
  if (version == NULL || version[0] == '\0') {
    fprintf(stderr, "missing version string\n");
    return 1;
  }
  char* version_info_json = NULL;
  if (agent_capi_version_info_json(&version_info_json) != 0 ||
      version_info_json == NULL ||
      strstr(version_info_json, "\"abiVersion\"") == NULL) {
    fprintf(stderr, "missing version info JSON: %s\n", agent_last_error());
    agent_string_free(version_info_json);
    return 1;
  }
  agent_string_free(version_info_json);

  const char* contract_json = agent_capi_contract_json();
  if (contract_json == NULL ||
      strstr(contract_json, "\"abiVersion\":") == NULL ||
      strstr(contract_json, "agent_capi_negotiate_abi_version") == NULL ||
      strstr(contract_json, "agent_last_error_object") == NULL ||
      strstr(contract_json, "agent_host_runtime_create") == NULL ||
      strstr(contract_json, "agent_runner_run_async") == NULL ||
      strstr(contract_json, "agent_runner_create_from_config_path") == NULL ||
      strstr(contract_json, "agent_runner_stream_events") == NULL ||
      strstr(contract_json, "agent_async_run_start_json") == NULL ||
      strstr(contract_json, "agent_tool_run_start_json") == NULL) {
    fprintf(stderr, "missing C ABI contract metadata\n");
    return 1;
  }

  /* ----- Null-argument rejection ----- */
  if (agent_runner_create_with_echo_model(NULL) == 0) {
    fprintf(stderr, "expected null out_runner to be rejected\n");
    return 1;
  }
  {
    agent_error_t* error = NULL;
    if (agent_last_error_object(&error) != 0 || error == NULL) {
      fprintf(stderr, "failed to copy last error object\n");
      return 1;
    }
    if (agent_error_code(error) != AGENT_STATUS_FRAMEWORK_ERROR ||
        agent_error_type(error) == NULL ||
        agent_error_type(error)[0] == '\0' ||
        agent_error_message(error) == NULL ||
        strstr(agent_error_message(error), "out_runner") == NULL) {
      fprintf(stderr, "unexpected last error object: code=%d type=%s message=%s\n",
              agent_error_code(error),
              agent_error_type(error),
              agent_error_message(error));
      agent_error_release(error);
      return 1;
    }
    char* error_json = NULL;
    if (agent_error_json(error, &error_json) != 0 ||
        error_json == NULL ||
        strstr(error_json, "\"message\"") == NULL) {
      fprintf(stderr, "failed to serialize error object\n");
      agent_string_free(error_json);
      agent_error_release(error);
      return 1;
    }
    agent_string_free(error_json);
    agent_error_release(error);
  }

  /* ----- Lifecycle + synchronous run ----- */
  agent_runner_t* runner = NULL;
  if (agent_runner_create_with_echo_model(&runner) != 0) {
    fprintf(stderr, "create failed: %s\n", agent_last_error());
    return 1;
  }
  assert(runner != NULL);

  char* result_json = NULL;
  if (agent_runner_run(runner, "hello world", "test-session", &result_json) != 0) {
    fprintf(stderr, "run failed: %s\n", agent_last_error());
    agent_runner_release(runner);
    return 1;
  }
  assert(result_json != NULL);
  /* Echo model returns the input verbatim — the result must contain it. */
  if (strstr(result_json, "hello world") == NULL) {
    fprintf(stderr, "result missing input: %s\n", result_json);
    agent_string_free(result_json);
    agent_runner_release(runner);
    return 1;
  }
  if (strstr(result_json, "Thought:") != NULL) {
    fprintf(stderr, "C API result leaked ReAct thought: %s\n", result_json);
    agent_string_free(result_json);
    agent_runner_release(runner);
    return 1;
  }
  if (strstr(result_json, "\"sessionId\": \"test-session\"") == NULL &&
      strstr(result_json, "\"sessionId\":\"test-session\"") == NULL) {
    fprintf(stderr, "result missing session id: %s\n", result_json);
    agent_string_free(result_json);
    agent_runner_release(runner);
    return 1;
  }
  if (strstr(result_json, "\"usage\"") == NULL ||
      strstr(result_json, "\"quality\"") == NULL) {
    fprintf(stderr, "result missing usage metadata: %s\n", result_json);
    agent_string_free(result_json);
    agent_runner_release(runner);
    return 1;
  }
  agent_string_free(result_json);

  /* ----- Explicit cancellation handle + cancellable sync run ----- */
  {
    agent_cancellation_t* cancellation = NULL;
    if (agent_cancellation_create(&cancellation) != 0 || cancellation == NULL) {
      fprintf(stderr, "cancellation create failed: %s\n", agent_last_error());
      agent_runner_release(runner);
      return 1;
    }
    result_json = NULL;
    if (agent_runner_run_with_cancellation(runner,
                                           "with cancellation handle",
                                           "cancel-handle-session",
                                           cancellation,
                                           &result_json) != 0) {
      fprintf(stderr, "run_with_cancellation failed: %s\n", agent_last_error());
      agent_cancellation_release(cancellation);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(result_json, "with cancellation handle") == NULL) {
      fprintf(stderr, "cancellable run result missing input: %s\n", result_json);
      agent_string_free(result_json);
      agent_cancellation_release(cancellation);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(result_json);
    agent_cancellation_cancel(cancellation, "c smoke cancellation");
    if (!agent_cancellation_cancelled(cancellation) ||
        strstr(agent_cancellation_reason(cancellation), "c smoke cancellation") == NULL) {
      fprintf(stderr, "cancellation state was not visible\n");
      agent_cancellation_release(cancellation);
      agent_runner_release(runner);
      return 1;
    }
    agent_cancellation_release(cancellation);
  }

  /* ----- Host vtable model injection ----- */
  {
    agent_host_vtable_t vtable;
    memset(&vtable, 0, sizeof(vtable));
    vtable.size = sizeof(vtable);
    vtable.model_generate_json = host_model_generate;

    agent_host_runtime_t* host_runtime = NULL;
    if (agent_host_runtime_create(&vtable, &host_runtime) != 0 || host_runtime == NULL) {
      fprintf(stderr, "host runtime create failed: %s\n", agent_last_error());
      agent_runner_release(runner);
      return 1;
    }
    char* host_description_json = NULL;
    if (agent_host_runtime_describe_json(host_runtime, &host_description_json) != 0 ||
        host_description_json == NULL ||
        (strstr(host_description_json, "\"model\":true") == NULL &&
         strstr(host_description_json, "\"model\": true") == NULL)) {
      fprintf(stderr, "host runtime description failed: %s\n", agent_last_error());
      agent_string_free(host_description_json);
      agent_host_runtime_release(host_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(host_description_json);

    agent_runner_t* host_runner = NULL;
    if (agent_runner_create_with_host_model(
            host_runtime,
            "{\"provider\":\"host-smoke\",\"model\":\"host-model\",\"capabilities\":[\"input.text\"]}",
            &host_runner) != 0 ||
        host_runner == NULL) {
      fprintf(stderr, "host runner create failed: %s\n", agent_last_error());
      agent_host_runtime_release(host_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_host_runtime_release(host_runtime);

    result_json = NULL;
    if (agent_runner_run(host_runner, "host hello", "host-session", &result_json) != 0) {
      fprintf(stderr, "host runner run failed: %s\n", agent_last_error());
      agent_runner_release(host_runner);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(result_json, "host callback ok") == NULL ||
        strstr(result_json, "\"usage\"") == NULL) {
      fprintf(stderr, "host runner result incomplete: %s\n", result_json);
      agent_string_free(result_json);
      agent_runner_release(host_runner);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(result_json);
    agent_runner_release(host_runner);
  }

  /* ----- Opaque async run handle ----- */
  {
    agent_run_t* run = NULL;
    if (agent_runner_run_async(runner,
                               "async handle hello",
                               "async-handle-session",
                               NULL,
                               &run) != 0 ||
        run == NULL) {
      fprintf(stderr, "run_async failed: %s\n", agent_last_error());
      agent_runner_release(runner);
      return 1;
    }
    result_json = NULL;
    if (agent_run_wait_json(run, &result_json) != 0) {
      fprintf(stderr, "run_wait_json failed: %s\n", agent_last_error());
      agent_run_release(run);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(result_json, "async handle hello") == NULL ||
        strstr(result_json, "\"usage\"") == NULL) {
      fprintf(stderr, "async handle result incomplete: %s\n", result_json);
      agent_string_free(result_json);
      agent_run_release(run);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(result_json);
    agent_run_release(run);
  }

  /* ----- Default session id when NULL is passed ----- */
  result_json = NULL;
  if (agent_runner_run(runner, "second", NULL, &result_json) != 0) {
    fprintf(stderr, "run with default session failed: %s\n", agent_last_error());
    agent_runner_release(runner);
    return 1;
  }
  assert(result_json != NULL);
  agent_string_free(result_json);

  /* ----- Streaming ----- */
  if (agent_runner_stream(runner, "stream me", "stream-session", on_stream_event, NULL) != 0) {
    fprintf(stderr, "stream failed: %s\n", agent_last_error());
    agent_runner_release(runner);
    return 1;
  }
  if (g_event_count == 0) {
    fprintf(stderr, "stream produced zero events\n");
    agent_runner_release(runner);
    return 1;
  }
  if (!g_saw_done) {
    fprintf(stderr, "stream never produced a Done event\n");
    agent_runner_release(runner);
    return 1;
  }

  /* ----- Pull streaming ----- */
  {
    agent_runner_event_stream_t* stream = NULL;
    if (agent_runner_stream_events(runner, "pull stream", "pull-stream-session", 1, &stream) != 0) {
      fprintf(stderr, "stream_events failed: %s\n", agent_last_error());
      agent_runner_release(runner);
      return 1;
    }
    assert(stream != NULL);
    int pull_count = 0;
    int pull_saw_done = 0;
    while (1) {
      char* event_json = NULL;
      int32_t has_event = 0;
      if (agent_runner_event_stream_next_json(stream, &event_json, &has_event) != 0) {
        fprintf(stderr, "stream next failed: %s\n", agent_last_error());
        agent_runner_event_stream_release(stream);
        agent_runner_release(runner);
        return 1;
      }
      if (!has_event) {
        break;
      }
      assert(event_json != NULL);
      ++pull_count;
      if (strstr(event_json, "\"schemaVersion\":") == NULL ||
          strstr(event_json, "\"sequence\":") == NULL) {
        fprintf(stderr, "pull event missing stream envelope fields: %s\n", event_json);
        agent_string_free(event_json);
        agent_runner_event_stream_release(stream);
        agent_runner_release(runner);
        return 1;
      }
      if (strstr(event_json, "\"type\":\"done\"") != NULL ||
          strstr(event_json, "\"type\": \"done\"") != NULL) {
        pull_saw_done = 1;
      }
      agent_string_free(event_json);
    }
    agent_runner_event_stream_release(stream);
    if (pull_count == 0 || !pull_saw_done) {
      fprintf(stderr, "pull stream did not complete correctly\n");
      agent_runner_release(runner);
      return 1;
    }
  }

  {
    agent_runner_event_stream_t* stream = NULL;
    if (agent_runner_stream_events(runner, "cancel pull stream", "pull-stream-cancel-session", 1, &stream) != 0) {
      fprintf(stderr, "stream_events for cancel failed: %s\n", agent_last_error());
      agent_runner_release(runner);
      return 1;
    }
    assert(stream != NULL);
    agent_runner_event_stream_cancel(stream, "binding cancelled");
    agent_runner_event_stream_release(stream);
  }

  /* ----- Stream cancellation via callback return value ----- */
  if (agent_runner_stream(runner, "abort me", "abort-session", abort_after_first, NULL) != 0) {
    fprintf(stderr, "abortable stream failed: %s\n", agent_last_error());
    agent_runner_release(runner);
    return 1;
  }
  assert(g_aborted == 1);

  /* ----- Async agent runs through JSON C ABI ----- */
  {
    agent_async_runtime_t* async_runtime = NULL;
    if (agent_async_runtime_create(runner, &async_runtime) != 0) {
      fprintf(stderr, "async runtime create failed: %s\n", agent_last_error());
      agent_runner_release(runner);
      return 1;
    }
    assert(async_runtime != NULL);

    char* snapshot_json = NULL;
    const char* start_json =
        "{"
        "\"id\":\"async-capi-run\","
        "\"input\":\"async hello\","
        "\"sessionId\":\"async-session\""
        "}";
    if (agent_async_run_start_json(async_runtime, start_json, &snapshot_json) != 0) {
      fprintf(stderr, "async start failed: %s\n", agent_last_error());
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(snapshot_json, "\"status\":\"queued\"") == NULL &&
        strstr(snapshot_json, "\"status\": \"queued\"") == NULL) {
      fprintf(stderr, "async start did not queue run: %s\n", snapshot_json);
      agent_string_free(snapshot_json);
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(snapshot_json);

    int32_t processed = 0;
    if (agent_async_runtime_run_once(async_runtime, &processed) != 0 || processed != 1) {
      fprintf(stderr, "async run_once failed: %s processed=%d\n", agent_last_error(), processed);
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }

    snapshot_json = NULL;
    if (agent_async_run_get_json(async_runtime, "async-capi-run", &snapshot_json) != 0) {
      fprintf(stderr, "async get failed: %s\n", agent_last_error());
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if ((strstr(snapshot_json, "\"status\":\"completed\"") == NULL &&
         strstr(snapshot_json, "\"status\": \"completed\"") == NULL) ||
        strstr(snapshot_json, "async hello") == NULL ||
        strstr(snapshot_json, "resourceLedger") == NULL ||
        strstr(snapshot_json, "activity") == NULL) {
      fprintf(stderr, "async run did not complete correctly: %s\n", snapshot_json);
      agent_string_free(snapshot_json);
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(snapshot_json);

    char* events_json = NULL;
    if (agent_async_run_events_json(async_runtime, "async-capi-run", &events_json) != 0) {
      fprintf(stderr, "async events failed: %s\n", agent_last_error());
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(events_json, "async_run.completed") == NULL) {
      fprintf(stderr, "async events missing completion: %s\n", events_json);
      agent_string_free(events_json);
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(events_json);

    char* checkpoints_json = NULL;
    if (agent_async_run_checkpoints_json(async_runtime, "async-capi-run", &checkpoints_json) != 0) {
      fprintf(stderr, "async checkpoints failed: %s\n", agent_last_error());
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(checkpoints_json, "\"result\"") == NULL ||
        strstr(checkpoints_json, "\"output\"") == NULL) {
      fprintf(stderr, "async checkpoints incomplete: %s\n", checkpoints_json);
      agent_string_free(checkpoints_json);
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(checkpoints_json);

    char* transcript_json = NULL;
    if (agent_async_run_transcript_json(async_runtime, "async-capi-run", &transcript_json) != 0) {
      fprintf(stderr, "async transcript failed: %s\n", agent_last_error());
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(transcript_json, "stream-event") == NULL ||
        strstr(transcript_json, "run-completed") == NULL) {
      fprintf(stderr, "async transcript incomplete: %s\n", transcript_json);
      agent_string_free(transcript_json);
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(transcript_json);

    char* list_json = NULL;
    if (agent_async_run_list_json(async_runtime, "{\"status\":\"completed\"}", &list_json) != 0) {
      fprintf(stderr, "async list failed: %s\n", agent_last_error());
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(list_json, "async-capi-run") == NULL) {
      fprintf(stderr, "async list missing completed run: %s\n", list_json);
      agent_string_free(list_json);
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(list_json);

    const char* cancel_start_json =
        "{"
        "\"id\":\"async-capi-cancel\","
        "\"input\":\"async cancel\","
        "\"sessionId\":\"async-cancel-session\""
        "}";
    if (agent_async_run_start_json(async_runtime, cancel_start_json, &snapshot_json) != 0) {
      fprintf(stderr, "async cancel start failed: %s\n", agent_last_error());
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(snapshot_json);
    snapshot_json = NULL;
    if (agent_async_run_cancel_json(async_runtime,
                                    "async-capi-cancel",
                                    "{\"reason\":\"test cancel\"}",
                                    &snapshot_json) != 0) {
      fprintf(stderr, "async cancel failed: %s\n", agent_last_error());
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(snapshot_json, "\"status\":\"cancelled\"") == NULL &&
        strstr(snapshot_json, "\"status\": \"cancelled\"") == NULL) {
      fprintf(stderr, "async cancel did not mark cancelled: %s\n", snapshot_json);
      agent_string_free(snapshot_json);
      agent_async_runtime_release(async_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(snapshot_json);
    agent_async_runtime_release(async_runtime);
  }

  /* ----- Generic ToolRun / background task runtime through JSON C ABI ----- */
  {
    agent_tool_run_runtime_t* tool_runtime = NULL;
    if (agent_tool_run_runtime_create(&tool_runtime) != 0) {
      fprintf(stderr, "tool run runtime create failed: %s\n", agent_last_error());
      agent_runner_release(runner);
      return 1;
    }
    assert(tool_runtime != NULL);

    char* tool_run_json = NULL;
    const char* tool_start_json =
        "{"
        "\"runId\":\"toolrun-capi-1\","
        "\"toolCallId\":\"tool-call-capi\","
        "\"toolName\":\"custom.background\","
        "\"kind\":\"custom\","
        "\"label\":\"C API custom run\","
        "\"metadata\":{\"owner\":\"c-smoke\"}"
        "}";
    if (agent_tool_run_start_json(tool_runtime, tool_start_json, &tool_run_json) != 0) {
      fprintf(stderr, "tool run start failed: %s\n", agent_last_error());
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(tool_run_json, "\"runId\":\"toolrun-capi-1\"") == NULL &&
        strstr(tool_run_json, "\"runId\": \"toolrun-capi-1\"") == NULL) {
      fprintf(stderr, "tool run start missing run id: %s\n", tool_run_json);
      agent_string_free(tool_run_json);
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(tool_run_json);

    char* event_json = NULL;
    if (agent_tool_run_append_event_json(tool_runtime,
                                         "toolrun-capi-1",
                                         "{\"type\":\"log\",\"stream\":\"stdout\",\"text\":\"ready soon\"}",
                                         &event_json) != 0) {
      fprintf(stderr, "tool run append event failed: %s\n", agent_last_error());
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(event_json, "ready soon") == NULL) {
      fprintf(stderr, "tool run event missing text: %s\n", event_json);
      agent_string_free(event_json);
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(event_json);

    char* read_json = NULL;
    if (agent_tool_run_read_json(tool_runtime, "toolrun-capi-1", "{\"cursor\":0,\"limit\":10}", &read_json) != 0) {
      fprintf(stderr, "tool run read failed: %s\n", agent_last_error());
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(read_json, "ready soon") == NULL || strstr(read_json, "\"events\"") == NULL) {
      fprintf(stderr, "tool run read incomplete: %s\n", read_json);
      agent_string_free(read_json);
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(read_json);

    if (agent_tool_run_update_json(tool_runtime,
                                   "toolrun-capi-1",
                                   "{\"status\":\"waiting\",\"ready\":true,\"metadata\":{\"url\":\"http://localhost:5173\"}}",
                                   &tool_run_json) != 0) {
      fprintf(stderr, "tool run update failed: %s\n", agent_last_error());
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(tool_run_json, "\"status\":\"waiting\"") == NULL &&
        strstr(tool_run_json, "\"status\": \"waiting\"") == NULL) {
      fprintf(stderr, "tool run update missing waiting status: %s\n", tool_run_json);
      agent_string_free(tool_run_json);
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(tool_run_json, "\"ready\":true") == NULL &&
        strstr(tool_run_json, "\"ready\": true") == NULL) {
      fprintf(stderr, "tool run update did not mark ready: %s\n", tool_run_json);
      agent_string_free(tool_run_json);
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(tool_run_json);

    if (agent_tool_run_wait_json(tool_runtime,
                                 "toolrun-capi-1",
                                 "{\"until\":\"ready\",\"timeoutMs\":10}",
                                 &tool_run_json) != 0) {
      fprintf(stderr, "tool run wait failed: %s\n", agent_last_error());
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(tool_run_json, "\"ready\":true") == NULL &&
        strstr(tool_run_json, "\"ready\": true") == NULL) {
      fprintf(stderr, "tool run wait did not observe ready: %s\n", tool_run_json);
      agent_string_free(tool_run_json);
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(tool_run_json);

    char* list_json = NULL;
    if (agent_tool_run_list_json(tool_runtime, "{\"kind\":\"custom\"}", &list_json) != 0) {
      fprintf(stderr, "tool run list failed: %s\n", agent_last_error());
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(list_json, "toolrun-capi-1") == NULL) {
      fprintf(stderr, "tool run list missing run: %s\n", list_json);
      agent_string_free(list_json);
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(list_json);

    if (agent_tool_run_cancel_json(tool_runtime,
                                   "toolrun-capi-1",
                                   "{\"reason\":\"c smoke cancel\"}",
                                   &tool_run_json) != 0) {
      fprintf(stderr, "tool run cancel failed: %s\n", agent_last_error());
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    if (strstr(tool_run_json, "\"status\":\"cancelled\"") == NULL &&
        strstr(tool_run_json, "\"status\": \"cancelled\"") == NULL) {
      fprintf(stderr, "tool run cancel did not mark cancelled: %s\n", tool_run_json);
      agent_string_free(tool_run_json);
      agent_tool_run_runtime_release(tool_runtime);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(tool_run_json);
    agent_tool_run_runtime_release(tool_runtime);
  }

  /* ----- Config-backed runner + JSON input surface ----- */
  {
    const char* config_json =
        "{"
        "\"agents\":{"
        "\"ffi\":{"
        "\"model\":{\"provider\":\"echo\",\"model\":\"echo\"}"
        "}"
        "}"
        "}";
    agent_runner_t* config_runner = NULL;
    if (agent_runner_create_from_config_json(config_json, "ffi", &config_runner) != 0) {
      fprintf(stderr, "config create failed: %s\n", agent_last_error());
      agent_runner_release(runner);
      return 1;
    }
    assert(config_runner != NULL);

    if (agent_runner_create_from_config_json("{\"agents\":{}}", "missing", NULL) == 0) {
      fprintf(stderr, "expected config create null out_runner to be rejected\n");
      agent_runner_release(config_runner);
      agent_runner_release(runner);
      return 1;
    }

    result_json = NULL;
    if (agent_runner_run_json(config_runner,
                              "\"json hello\"",
                              "json-session",
                              &result_json) != 0) {
      fprintf(stderr, "run_json failed: %s\n", agent_last_error());
      agent_runner_release(config_runner);
      agent_runner_release(runner);
      return 1;
    }
    assert(result_json != NULL);
    if (strstr(result_json, "json hello") == NULL) {
      fprintf(stderr, "run_json result missing input: %s\n", result_json);
      agent_string_free(result_json);
      agent_runner_release(config_runner);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(result_json);

    result_json = NULL;
    if (agent_runner_run_json(config_runner,
                              "{\"content\":\"json message input\"}",
                              "json-message-session",
                              &result_json) != 0) {
      fprintf(stderr, "run_json message failed: %s\n", agent_last_error());
      agent_runner_release(config_runner);
      agent_runner_release(runner);
      return 1;
    }
    assert(result_json != NULL);
    if (strstr(result_json, "json message input") == NULL) {
      fprintf(stderr, "run_json message result missing input: %s\n", result_json);
      agent_string_free(result_json);
      agent_runner_release(config_runner);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(result_json);

    if (agent_runner_run_json(config_runner,
                              "{\"unexpected\":true}",
                              "json-invalid-session",
                              &result_json) == 0) {
      fprintf(stderr, "expected invalid JSON message object to be rejected\n");
      agent_runner_release(config_runner);
      agent_runner_release(runner);
      return 1;
    }

    if (agent_runner_stream_json(config_runner,
                                 "[{\"type\":\"text\",\"text\":\"json stream\"}]",
                                 "json-stream-session",
                                 on_json_stream_event,
                                 NULL) != 0) {
      fprintf(stderr, "stream_json failed: %s\n", agent_last_error());
      agent_runner_release(config_runner);
      agent_runner_release(runner);
      return 1;
    }
    if (g_json_event_count == 0 || !g_json_saw_done) {
      fprintf(stderr, "stream_json did not complete correctly\n");
      agent_runner_release(config_runner);
      agent_runner_release(runner);
      return 1;
    }

    agent_runner_release(config_runner);
  }

  /* ----- Config path runner ----- */
  {
    const char* config_path = "agent-capi-smoke-config.json";
    FILE* file = fopen(config_path, "w");
    if (file == NULL) {
      fprintf(stderr, "failed to create config path fixture\n");
      agent_runner_release(runner);
      return 1;
    }
    fputs("{\"agents\":{\"ffi-path\":{\"model\":{\"provider\":\"echo\",\"model\":\"echo\"}}}}", file);
    fclose(file);

    agent_runner_t* path_runner = NULL;
    if (agent_runner_create_from_config_path(config_path, "ffi-path", &path_runner) != 0) {
      fprintf(stderr, "config path create failed: %s\n", agent_last_error());
      remove(config_path);
      agent_runner_release(runner);
      return 1;
    }
    assert(path_runner != NULL);

    result_json = NULL;
    if (agent_runner_run(path_runner, "path hello", "path-session", &result_json) != 0) {
      fprintf(stderr, "path runner run failed: %s\n", agent_last_error());
      agent_runner_release(path_runner);
      remove(config_path);
      agent_runner_release(runner);
      return 1;
    }
    assert(result_json != NULL);
    if (strstr(result_json, "path hello") == NULL) {
      fprintf(stderr, "path runner result missing input: %s\n", result_json);
      agent_string_free(result_json);
      agent_runner_release(path_runner);
      remove(config_path);
      agent_runner_release(runner);
      return 1;
    }
    agent_string_free(result_json);
    agent_runner_release(path_runner);
    remove(config_path);
  }

  /* ----- Idempotent release ----- */
  agent_runner_release(runner);
  agent_runner_release(NULL); /* must be safe */

  printf("agent_capi_smoke OK (events=%d)\n", g_event_count);
  return 0;
}
