// Native Agent Framework — C ABI shim implementation.
//
// All entry points catch every exception type so that nothing crosses the C
// boundary. Errors are reported through the thread-local agent_last_error()
// channel. JSON payloads are produced through the framework's own Value
// stringifier so the shim adds no external JSON dependency.

#include "agent_capi.h"

#include "agent/agent.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <utility>

namespace {

thread_local std::string g_last_error;

void set_last_error(std::string message) noexcept {
  try {
    g_last_error = std::move(message);
  } catch (...) {
    // If allocating the message itself throws, drop it; the caller still gets
    // a non-zero status code from the entry point.
  }
}

char* dup_cstr(const std::string& source) noexcept {
  char* out = static_cast<char*>(std::malloc(source.size() + 1));
  if (out == nullptr) {
    return nullptr;
  }
  std::memcpy(out, source.data(), source.size());
  out[source.size()] = '\0';
  return out;
}

template <typename Fn>
int32_t guarded(Fn&& fn) noexcept {
  try {
    fn();
    return 0;
  } catch (const agent::AgentFrameworkError& error) {
    set_last_error(error.what());
    return 1;
  } catch (const std::exception& error) {
    set_last_error(error.what());
    return 2;
  } catch (...) {
    set_last_error("unknown error");
    return 3;
  }
}

agent::Value run_result_to_value(const agent::AgentRunnerRunResult& result) {
  return agent::Value::object({
      {"sessionId", result.session_id},
      {"text", result.text},
      {"iterationCount", static_cast<long long>(result.iteration_count)},
      {"terminationReason", agent::to_string(result.termination_reason)},
  });
}

agent::Value stream_event_to_value(const agent::AgentRunnerStreamEvent& event) {
  agent::Value payload = agent::Value::object({
      {"type", agent::to_string(event.type)},
  });
  switch (event.type) {
    case agent::AgentRunnerStreamEventType::Status:
      payload["stage"] = event.status.stage;
      payload["state"] = event.status.state;
      if (!event.status.message.empty()) {
        payload["message"] = event.status.message;
      }
      break;
    case agent::AgentRunnerStreamEventType::Loop:
      payload["loop"] = agent::Value::object({
          {"type", agent::to_string(event.loop_event.type)},
          {"iteration", static_cast<long long>(event.loop_event.iteration)},
          {"delta", event.loop_event.delta},
      });
      break;
    case agent::AgentRunnerStreamEventType::ToolCallArgumentDelta:
      payload["toolCallId"] = event.tool_call_id;
      payload["toolName"] = event.tool_call_name;
      payload["argsDelta"] = event.tool_call_args_delta;
      payload["argsAccumulated"] = event.tool_call_args_accumulated;
      break;
    case agent::AgentRunnerStreamEventType::Done:
      payload["result"] = run_result_to_value(event.result);
      break;
    case agent::AgentRunnerStreamEventType::Error:
      payload["error"] = event.error;
      break;
    default:
      break;
  }
  return payload;
}

agent::AgentRunnerConfig make_echo_runner_config() {
  agent::AgentRunnerConfig config;
  config.adapter = std::make_shared<agent::EchoChatModelAdapter>();
  config.max_iterations = 1;
  return config;
}

}  // namespace

struct agent_runner_t {
  agent::AgentRunner runner;

  agent_runner_t() : runner(make_echo_runner_config()) {}
};

extern "C" const char* agent_last_error(void) {
  return g_last_error.c_str();
}

extern "C" void agent_string_free(char* str) {
  std::free(str);
}

extern "C" const char* agent_version(void) {
  return "agent_native " __DATE__;
}

extern "C" int32_t agent_capi_abi_version(void) {
  return AGENT_CAPI_ABI_VERSION;
}

extern "C" int32_t agent_runner_create_with_echo_model(agent_runner_t** out_runner) {
  if (out_runner == nullptr) {
    set_last_error("out_runner is null.");
    return 1;
  }
  return guarded([&] {
    *out_runner = new agent_runner_t();
  });
}

extern "C" void agent_runner_release(agent_runner_t* runner) {
  delete runner;
}

extern "C" int32_t agent_runner_run(agent_runner_t* runner,
                                    const char* input,
                                    const char* session_id,
                                    char** out_result_json) {
  if (runner == nullptr || input == nullptr || out_result_json == nullptr) {
    set_last_error("runner, input, and out_result_json must be non-null.");
    return 1;
  }
  return guarded([&] {
    const std::string session = session_id ? session_id : "default";
    auto result = runner->runner.run(std::string(input), session);
    auto payload = run_result_to_value(result);
    *out_result_json = dup_cstr(agent::safe_json_stringify(payload));
    if (*out_result_json == nullptr) {
      throw agent::AgentFrameworkError("Out of memory while serializing run result.");
    }
  });
}

extern "C" int32_t agent_runner_stream(agent_runner_t* runner,
                                       const char* input,
                                       const char* session_id,
                                       agent_stream_callback_t on_event,
                                       void* user_data) {
  if (runner == nullptr || input == nullptr || on_event == nullptr) {
    set_last_error("runner, input, and on_event must be non-null.");
    return 1;
  }
  return guarded([&] {
    const std::string session = session_id ? session_id : "default";
    auto stream = runner->runner.stream(std::string(input), session);
    for (const auto& event : stream.events) {
      const auto payload = stream_event_to_value(event);
      const auto encoded = agent::safe_json_stringify(payload);
      if (on_event(encoded.c_str(), user_data) != 0) {
        break;
      }
    }
  });
}
