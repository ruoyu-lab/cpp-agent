/*
 * Round-trip smoke test for the agent_capi C ABI shim.
 *
 * Compiled as C99 — proves the header is genuinely C-compatible and that the
 * framework can be driven entirely through the C boundary that host
 * languages will use.
 */

#include "agent_capi.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_event_count = 0;
static int g_saw_done = 0;
static int g_aborted = 0;

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

int main(void) {
  /* ----- ABI metadata ----- */
  if (agent_capi_abi_version() != 1) {
    fprintf(stderr, "unexpected ABI version: %d\n", agent_capi_abi_version());
    return 1;
  }
  const char* version = agent_version();
  if (version == NULL || version[0] == '\0') {
    fprintf(stderr, "missing version string\n");
    return 1;
  }

  /* ----- Null-argument rejection ----- */
  if (agent_runner_create_with_echo_model(NULL) == 0) {
    fprintf(stderr, "expected null out_runner to be rejected\n");
    return 1;
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
  if (strstr(result_json, "\"sessionId\": \"test-session\"") == NULL &&
      strstr(result_json, "\"sessionId\":\"test-session\"") == NULL) {
    fprintf(stderr, "result missing session id: %s\n", result_json);
    agent_string_free(result_json);
    agent_runner_release(runner);
    return 1;
  }
  agent_string_free(result_json);

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

  /* ----- Stream cancellation via callback return value ----- */
  if (agent_runner_stream(runner, "abort me", "abort-session", abort_after_first, NULL) != 0) {
    fprintf(stderr, "abortable stream failed: %s\n", agent_last_error());
    agent_runner_release(runner);
    return 1;
  }
  assert(g_aborted == 1);

  /* ----- Idempotent release ----- */
  agent_runner_release(runner);
  agent_runner_release(NULL); /* must be safe */

  printf("agent_capi_smoke OK (events=%d)\n", g_event_count);
  return 0;
}
