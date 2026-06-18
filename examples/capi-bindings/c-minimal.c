#include "agent_capi.h"

#include <stdio.h>
#include <stdlib.h>

static void require_ok(int32_t status, const char* context) {
  if (status == AGENT_STATUS_OK) {
    return;
  }
  fprintf(stderr, "%s failed: %s\n", context, agent_last_error());
  exit(1);
}

int main(void) {
  int32_t negotiated = 0;
  require_ok(agent_capi_negotiate_abi_version(3, 3, &negotiated),
             "agent_capi_negotiate_abi_version");

  agent_runner_t* runner = NULL;
  require_ok(agent_runner_create_with_echo_model(&runner),
             "agent_runner_create_with_echo_model");

  char* result_json = NULL;
  require_ok(agent_runner_run(runner, "hello from C", "c-example", &result_json),
             "agent_runner_run");

  puts(result_json);
  agent_string_free(result_json);
  agent_runner_release(runner);
  return 0;
}
