// Native Agent Framework — C ABI shim implementation.
//
// All entry points catch every exception type so that nothing crosses the C
// boundary. Errors are reported through the thread-local agent_last_error()
// channel. JSON payloads are produced through the framework's own Value
// stringifier so the shim adds no external JSON dependency.

#include "agent_capi.h"

#ifdef AGENT_CAPI_ENABLE_FULL
#include "agent/app_api.hpp"
#include "agent/async.hpp"
#else
#include "agent/runtime_api.hpp"
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <utility>

#ifndef AGENT_NATIVE_VERSION
#define AGENT_NATIVE_VERSION "0.1.0"
#endif

namespace {

struct LastErrorSnapshot {
  int32_t code = AGENT_STATUS_OK;
  std::string type;
  std::string message;
};

thread_local LastErrorSnapshot g_last_error;

void clear_last_error() noexcept {
  g_last_error = LastErrorSnapshot{};
}

void set_last_error(int32_t code, std::string type, std::string message) noexcept {
  try {
    g_last_error.code = code;
    g_last_error.type = std::move(type);
    g_last_error.message = std::move(message);
  } catch (...) {
    // If allocating the message itself throws, drop it; the caller still gets
    // a non-zero status code from the entry point.
  }
}

void set_last_error(std::string message) noexcept {
  set_last_error(AGENT_STATUS_FRAMEWORK_ERROR, "ConfigurationError", std::move(message));
}

void set_last_error(const agent::AgentFrameworkError& error) noexcept {
  set_last_error(AGENT_STATUS_FRAMEWORK_ERROR,
                 std::string(error.error_name()),
                 error.what());
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
  clear_last_error();
  try {
    fn();
    return AGENT_STATUS_OK;
  } catch (const agent::AgentFrameworkError& error) {
    set_last_error(error);
    return AGENT_STATUS_FRAMEWORK_ERROR;
  } catch (const std::exception& error) {
    set_last_error(AGENT_STATUS_STD_EXCEPTION, "std::exception", error.what());
    return AGENT_STATUS_STD_EXCEPTION;
  } catch (...) {
    set_last_error(AGENT_STATUS_UNKNOWN_ERROR, "unknown", "unknown error");
    return AGENT_STATUS_UNKNOWN_ERROR;
  }
}

agent::Value error_to_value(const LastErrorSnapshot& error) {
  return agent::Value::object({
      {"code", error.code},
      {"type", error.type},
      {"message", error.message},
  });
}

struct CApiHostRuntimeState {
  agent_host_vtable_t vtable{};
};

agent::Value chat_tool_descriptor_to_value(const agent::ChatToolDescriptor& tool) {
  return agent::Value::object({
      {"name", tool.name},
      {"description", tool.description},
      {"inputSchema", agent::json_schema_to_value(tool.input_schema)},
      {"readOnly", tool.read_only},
      {"mutatesFiles", tool.mutates_files},
      {"interactive", tool.interactive},
      {"longRunning", tool.long_running},
      {"batchable", tool.batchable},
      {"concurrencyKey", tool.concurrency_key},
      {"sideEffectLevel", tool.side_effect_level},
  });
}

agent::Value chat_tools_to_value(const std::vector<agent::ChatToolDescriptor>& tools) {
  agent::Value::Array values;
  values.reserve(tools.size());
  for (const auto& tool : tools) {
    values.push_back(chat_tool_descriptor_to_value(tool));
  }
  return agent::Value(std::move(values));
}

agent::Value messages_to_value(const std::vector<agent::AgentMessage>& messages);

agent::Value host_model_request_to_value(const agent::GenerateParams& params,
                                         const std::string& provider,
                                         const std::string& model) {
  return agent::Value::object({
      {"provider", provider},
      {"model", model},
      {"messages", messages_to_value(params.messages)},
      {"tools", chat_tools_to_value(params.tools)},
      {"settings", agent::model_settings_to_json_value(params.settings)},
  });
}

agent::Value normalize_host_model_response(const agent::Value& value) {
  if (value.is_string()) {
    return agent::Value::object({{"text", value.as_string()}});
  }
  if (!value.is_object()) {
    throw agent::ConfigurationError("Host model response must be a JSON object or string.");
  }
  return value;
}

class CApiHostModelAdapter final : public agent::ChatModelAdapter {
 public:
  CApiHostModelAdapter(std::shared_ptr<CApiHostRuntimeState> state,
                       std::string provider,
                       std::string model,
                       double temperature,
                       int max_output_tokens,
                       std::set<std::string> capabilities)
      : agent::ChatModelAdapter(std::move(provider),
                                std::move(model),
                                temperature,
                                max_output_tokens,
                                std::move(capabilities)),
        state_(std::move(state)) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    if (!state_ || !state_->vtable.model_generate_json) {
      throw agent::ConfigurationError("Host model runtime does not provide model_generate_json.");
    }
    if (params.cancellation) {
      params.cancellation->throw_if_cancelled(agent::ExecutionTarget::Model);
    }
    if (state_->vtable.cancelled && state_->vtable.cancelled(state_->vtable.user_data) != 0) {
      throw agent::CancellationError("Host requested model cancellation.", "model", "host-cancelled");
    }

    const std::string request_json = agent::safe_json_stringify(
        host_model_request_to_value(params, provider(), model()));
    char* response_json = nullptr;
    const int32_t status = state_->vtable.model_generate_json(
        request_json.c_str(), &response_json, state_->vtable.user_data);
    std::unique_ptr<char, decltype(&std::free)> response_guard(response_json, &std::free);
    if (status != AGENT_STATUS_OK) {
      const std::string details = response_json ? std::string(response_json) : std::string{};
      throw agent::AgentFrameworkError(
          "Host model callback failed with status " + std::to_string(status) +
          (details.empty() ? "." : ": " + details));
    }
    if (response_json == nullptr) {
      throw agent::ConfigurationError("Host model callback returned a null response JSON.");
    }

    auto output = agent::agent_output_from_value(
        normalize_host_model_response(agent::parse_json(response_json)));
    return build_output(std::move(output));
  }

 private:
  std::shared_ptr<CApiHostRuntimeState> state_;
};

class CApiEchoReActModel final : public agent::ChatModelAdapter {
 public:
  CApiEchoReActModel()
      : agent::ChatModelAdapter("capi-echo", "capi-echo-react", 0.0, 1024, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    std::string input;
    if (!params.messages.empty()) {
      input = agent::extract_text_content(params.messages.back().content);
    }
    agent::AgentOutput response;
    response.text = "Thought: echo the input privately.\nFinal Answer: " + input;
    response.finish_reason = "stop";
    return build_output(response);
  }
};

agent::Value tool_call_to_value(const agent::ToolCall& tool_call) {
  return agent::Value::object({
      {"id", tool_call.id},
      {"name", tool_call.name},
      {"arguments", tool_call.arguments},
  });
}

agent::Value tool_calls_to_value(const std::vector<agent::ToolCall>& tool_calls) {
  agent::Value::Array values;
  values.reserve(tool_calls.size());
  for (const auto& tool_call : tool_calls) {
    values.push_back(tool_call_to_value(tool_call));
  }
  return agent::Value(std::move(values));
}

agent::Value model_usage_to_value(const agent::ModelUsage& usage) {
  return agent::Value::object({
      {"inputTokens", usage.input_tokens},
      {"outputTokens", usage.output_tokens},
      {"totalTokens", usage.total_tokens},
      {"inputTokensSource", agent::to_string(usage.input_tokens_source)},
      {"outputTokensSource", agent::to_string(usage.output_tokens_source)},
      {"totalTokensSource", agent::to_string(usage.total_tokens_source)},
      {"quality", agent::to_string(usage.quality)},
      {"cachedInputTokens", usage.cached_input_tokens},
      {"cachedInputTokensSource", agent::to_string(usage.cached_input_tokens_source)},
      {"reasoningTokens", usage.reasoning_tokens},
      {"reasoningSource", agent::to_string(usage.reasoning_source)},
      {"provider", usage.provider},
  });
}

agent::Value model_response_to_value(const agent::AgentOutput& response) {
  auto value = agent::agent_output_to_value(response);
  value["message"] = agent::agent_message_to_value(agent::assistant_message_from_output(response));
  return value;
}

agent::Value tool_invoke_result_to_value(const std::optional<agent::ToolInvokeResult>& result) {
  if (!result) {
    return {};
  }
  if (std::holds_alternative<agent::Value>(*result)) {
    return std::get<agent::Value>(*result);
  }

  const auto& envelope = std::get<agent::ToolResultEnvelope>(*result);
  agent::Value::Object object{{"metadata", envelope.metadata}};
  if (envelope.value) {
    object["value"] = *envelope.value;
  }
  if (envelope.content) {
    auto message = agent::agent_message_to_value(
        agent::create_message(agent::MessageRole::Assistant, *envelope.content));
    object["content"] = message.at("content");
  }
  return agent::Value(std::move(object));
}

agent::Value structured_tool_output_to_value(const std::optional<agent::ToolInvokeResult>& result) {
  if (!result) {
    return {};
  }
  if (std::holds_alternative<agent::Value>(*result)) {
    return std::get<agent::Value>(*result);
  }
  const auto& envelope = std::get<agent::ToolResultEnvelope>(*result);
  return envelope.value ? *envelope.value : agent::Value();
}

agent::Value tool_result_to_value(const agent::ToolExecutionResult& result) {
  return agent::Value::object({
      {"toolCall", tool_call_to_value(result.tool_call)},
      {"ok", result.ok},
      {"result", tool_invoke_result_to_value(result.result)},
      {"structuredOutput", structured_tool_output_to_value(result.result)},
      {"error", result.error.empty() ? agent::Value() : agent::Value(result.error)},
      {"output", result.output},
      {"message", agent::agent_message_to_value(result.message)},
  });
}

agent::Value trace_entry_to_value(const agent::AgentTraceEntry& entry) {
  agent::Value::Object object{{"type", entry.type}, {"iteration", entry.iteration}};
  if (entry.type == "model") {
    object["response"] = model_response_to_value(entry.response);
  }
  if (!entry.tool_results.empty()) {
    agent::Value::Array tool_results;
    tool_results.reserve(entry.tool_results.size());
    for (const auto& result : entry.tool_results) {
      tool_results.push_back(tool_result_to_value(result));
    }
    object["toolResults"] = agent::Value(std::move(tool_results));
  }
  return agent::Value(std::move(object));
}

agent::Value trace_to_value(const std::vector<agent::AgentTraceEntry>& trace) {
  agent::Value::Array values;
  values.reserve(trace.size());
  for (const auto& entry : trace) {
    values.push_back(trace_entry_to_value(entry));
  }
  return agent::Value(std::move(values));
}

agent::Value react_trace_to_value(const std::vector<agent::ReActTraceEntry>& trace) {
  agent::Value::Array values;
  values.reserve(trace.size());
  for (const auto& entry : trace) {
    values.push_back(agent::react_trace_entry_to_value(entry));
  }
  return agent::Value(std::move(values));
}

agent::Value messages_to_value(const std::vector<agent::AgentMessage>& messages) {
  agent::Value::Array values;
  values.reserve(messages.size());
  for (const auto& message : messages) {
    values.push_back(agent::agent_message_to_value(message));
  }
  return agent::Value(std::move(values));
}

agent::Value memory_hits_to_value(const std::vector<agent::RetrievedMemory>& hits) {
  agent::Value::Array values;
  values.reserve(hits.size());
  for (const auto& hit : hits) {
    values.push_back(agent::retrieved_memory_to_value(hit));
  }
  return agent::Value(std::move(values));
}

agent::Value knowledge_hits_to_value(const std::vector<agent::KnowledgeSearchHit>& hits) {
  agent::Value::Array values;
  values.reserve(hits.size());
  for (const auto& hit : hits) {
    values.push_back(agent::knowledge_search_hit_to_value(hit));
  }
  return agent::Value(std::move(values));
}

agent::Value loop_run_result_to_value(const agent::AgentLoopRunResult& result) {
  return agent::Value::object({
      {"sessionId", result.session_id},
      {"text", result.text},
      {"iterationCount", result.iteration_count},
      {"terminationReason", agent::to_string(result.termination_reason)},
      {"response", model_response_to_value(result.response)},
      {"trace", trace_to_value(result.trace)},
      {"reactTrace", react_trace_to_value(result.react_trace)},
      {"messages", messages_to_value(result.messages)},
      {"usage", model_usage_to_value(result.usage)},
      {"latestNonEmptyReasoning", result.latest_non_empty_reasoning},
  });
}

agent::Value run_result_to_value(const agent::AgentRunnerRunResult& result) {
  return agent::Value::object({
      {"sessionId", result.session_id},
      {"text", result.text},
      {"iterationCount", result.iteration_count},
      {"terminationReason", agent::to_string(result.termination_reason)},
      {"response", model_response_to_value(result.response)},
      {"messages", messages_to_value(result.messages)},
      {"usage", model_usage_to_value(result.usage)},
      {"memoryHits", memory_hits_to_value(result.memory_hits)},
      {"knowledgeHits", knowledge_hits_to_value(result.knowledge_hits)},
      {"knowledgeDebug", result.knowledge_debug},
      {"plan", result.plan ? agent::execution_plan_to_value(*result.plan) : agent::Value()},
      {"latestNonEmptyReasoning", result.latest_non_empty_reasoning},
  });
}

agent::Value loop_event_to_value(const agent::AgentLoopStreamEvent& event) {
  agent::Value value = agent::Value::object({
      {"schemaVersion", event.schema_version},
      {"sequence", static_cast<long long>(event.sequence)},
      {"type", agent::to_string(event.type)},
      {"iteration", event.iteration},
  });
  if (!event.provider.empty()) {
    value["provider"] = event.provider;
  }
  if (!event.model.empty()) {
    value["model"] = event.model;
  }
  if (!event.delta.empty()) {
    value["delta"] = event.delta;
  }
  if (!event.text.empty()) {
    value["text"] = event.text;
  }
  if (!event.reasoning.empty()) {
    value["reasoning"] = event.reasoning;
    value["accumulated"] = event.reasoning;
  }
  if (!event.reasoning_id.empty()) {
    value["reasoningId"] = event.reasoning_id;
  }
  if (!event.reasoning_scope.empty()) {
    value["scope"] = event.reasoning_scope;
  }
  if (!event.run_id.empty()) {
    value["runId"] = event.run_id;
  }

  switch (event.type) {
    case agent::AgentLoopStreamEventType::AgentOutput:
      value["response"] = model_response_to_value(event.response);
      break;
    case agent::AgentLoopStreamEventType::ToolCallArgumentDelta:
      value["toolCallId"] = event.tool_call_id;
      value["toolName"] = event.tool_call_name;
      value["argsDelta"] = event.tool_call_args_delta;
      value["argsAccumulated"] = event.tool_call_args_accumulated;
      break;
    case agent::AgentLoopStreamEventType::ReActActionBatch:
      value["react"] = agent::react_trace_entry_to_value(event.react_step);
      value["toolCalls"] = tool_calls_to_value(event.tool_calls);
      break;
    case agent::AgentLoopStreamEventType::ToolBatchStart:
      value["toolCalls"] = tool_calls_to_value(event.tool_calls);
      value["toolResults"] = agent::Value::array({});
      break;
    case agent::AgentLoopStreamEventType::ToolStart:
      value["toolCall"] = tool_call_to_value(event.tool_call);
      break;
    case agent::AgentLoopStreamEventType::ToolComplete:
      value["toolResult"] = tool_result_to_value(event.tool_result);
      break;
    case agent::AgentLoopStreamEventType::ToolBatchComplete: {
      value["toolCalls"] = tool_calls_to_value(event.tool_calls);
      agent::Value::Array tool_results;
      tool_results.reserve(event.tool_results.size());
      for (const auto& result : event.tool_results) {
        tool_results.push_back(tool_result_to_value(result));
      }
      value["toolResults"] = agent::Value(std::move(tool_results));
      break;
    }
    case agent::AgentLoopStreamEventType::ReActMessage:
    case agent::AgentLoopStreamEventType::ReActObservation:
    case agent::AgentLoopStreamEventType::ReActFinal:
    case agent::AgentLoopStreamEventType::ReActFinalRejected:
    case agent::AgentLoopStreamEventType::ReActReasoningProtocolLeak:
    case agent::AgentLoopStreamEventType::ReActParseError:
      value["react"] = agent::react_trace_entry_to_value(event.react_step);
      break;
    case agent::AgentLoopStreamEventType::Done:
      value["result"] = loop_run_result_to_value(event.result);
      break;
    default:
      break;
  }
  return value;
}

agent::Value status_to_value(const agent::AgentRunnerStatus& status) {
  agent::Value value = agent::Value::object({
      {"kind", status.kind},
      {"stage", status.stage},
      {"state", status.state},
      {"message", status.message},
      {"details", status.details},
  });
  if (status.iteration >= 0) {
    value["iteration"] = status.iteration;
  }
  if (!status.provider.empty()) {
    value["provider"] = status.provider;
  }
  if (!status.model.empty()) {
    value["model"] = status.model;
  }
  if (!status.tool_name.empty()) {
    value["toolName"] = status.tool_name;
  }
  if (!status.tool_call_id.empty()) {
    value["toolCallId"] = status.tool_call_id;
  }
  return value;
}

agent::Value stream_event_to_value(const agent::AgentRunnerStreamEvent& event) {
  agent::Value payload = agent::Value::object({
      {"schemaVersion", event.schema_version},
      {"sequence", static_cast<long long>(event.sequence)},
      {"type", agent::to_string(event.type)},
  });
  switch (event.type) {
    case agent::AgentRunnerStreamEventType::Status:
      payload["status"] = status_to_value(event.status);
      payload["stage"] = event.status.stage;
      payload["state"] = event.status.state;
      payload["message"] = event.status.message;
      if (event.status.iteration >= 0) {
        payload["iteration"] = event.status.iteration;
      }
      break;
    case agent::AgentRunnerStreamEventType::KnowledgeRetrieval:
      payload["knowledgeHits"] = knowledge_hits_to_value(event.knowledge_hits);
      payload["knowledgeMessage"] =
          event.knowledge_message ? agent::agent_message_to_value(*event.knowledge_message) : agent::Value();
      payload["knowledgeDebug"] = event.knowledge_debug;
      break;
    case agent::AgentRunnerStreamEventType::MemoryRetrieval:
      payload["memoryHits"] = memory_hits_to_value(event.memory_hits);
      payload["memoryMessage"] =
          event.memory_message ? agent::agent_message_to_value(*event.memory_message) : agent::Value();
      break;
    case agent::AgentRunnerStreamEventType::Planning:
      payload["plan"] = event.plan ? agent::execution_plan_to_value(*event.plan) : agent::Value();
      break;
    case agent::AgentRunnerStreamEventType::UserVisibleDelta:
      payload["delta"] = event.delta;
      payload["text"] = event.text;
      break;
    case agent::AgentRunnerStreamEventType::Loop:
      payload["loop"] = loop_event_to_value(event.loop_event);
      break;
    case agent::AgentRunnerStreamEventType::ToolCallArgumentDelta:
      payload["iteration"] = event.tool_call_iteration;
      payload["provider"] = event.tool_call_provider;
      payload["model"] = event.tool_call_model;
      payload["toolCallId"] = event.tool_call_id;
      payload["toolName"] = event.tool_call_name;
      payload["argsDelta"] = event.tool_call_args_delta;
      payload["argsAccumulated"] = event.tool_call_args_accumulated;
      break;
    case agent::AgentRunnerStreamEventType::Done:
      payload["result"] = run_result_to_value(event.result);
      break;
    case agent::AgentRunnerStreamEventType::Cancelled:
      payload["cancellation"] = event.cancellation;
      break;
    case agent::AgentRunnerStreamEventType::Error:
      payload["error"] = event.error;
      break;
  }
  return payload;
}

agent::AgentRunnerConfig make_echo_runner_config() {
  agent::AgentRunnerConfig config;
  config.model_runtime.adapter = std::make_shared<CApiEchoReActModel>();
  config.context_runtime.max_iterations = 1;
  return config;
}

std::shared_ptr<agent::AgentRunner> make_echo_runner() {
  return std::make_shared<agent::AgentRunner>(make_echo_runner_config());
}

agent::AgentMessage capi_input_message_from_json_text(const char* input_json) {
  const auto input = agent::parse_json(input_json);
  if (input.is_object()) {
    if (!input.contains("role") && !input.contains("content")) {
      throw agent::ConfigurationError(
          "JSON runner input object must contain role or content.");
    }
    auto message = input;
    if (!message.contains("role")) {
      message["role"] = "user";
    }
    return agent::agent_message_from_value(message);
  }
  return agent::agent_message_from_value(
      agent::Value::object({{"role", "user"}, {"content", input}}));
}

void require_out_runner(agent_runner_t** out_runner) {
  if (out_runner == nullptr) {
    throw agent::ConfigurationError("out_runner is null.");
  }
}

void assign_runner(agent_runner_t** out_runner, std::shared_ptr<agent::AgentRunner> runner);

const std::string& capi_contract_json_text() {
  static const std::string contract = [] {
    const auto result_shape = agent::Value::object({
        {"sessionId", "string"},
        {"text", "string"},
        {"iterationCount", "number"},
        {"terminationReason", "string"},
        {"response", "object"},
        {"messages", "array"},
        {"usage", "object"},
        {"memoryHits", "array"},
        {"knowledgeHits", "array"},
        {"knowledgeDebug", "object|null"},
        {"plan", "object|null"},
    });
    const auto stream_shape = agent::Value::object({
        {"status", "status event"},
        {"knowledge-retrieval", "knowledge retrieval payload"},
        {"memory-retrieval", "memory retrieval payload"},
        {"plan", "planning payload"},
        {"loop", "nested loop event"},
        {"tool-call-argument-delta", "partial tool call arguments"},
        {"done", "final runner result"},
        {"cancelled", "cancellation snapshot"},
        {"error", "error snapshot"},
    });
    return agent::safe_json_stringify(agent::Value::object({
        {"abiVersion", AGENT_CAPI_ABI_VERSION},
        {"version", std::string("agent_native ") + AGENT_NATIVE_VERSION},
        {"statusCodes", agent::Value::object({
                            {"0", "success"},
                            {"1", "AgentFrameworkError"},
                            {"2", "std::exception"},
                            {"3", "unknown error"},
        })},
        {"constructors", agent::Value::object({
                             {"echo", "agent_runner_create_with_echo_model"},
#ifdef AGENT_CAPI_ENABLE_FULL
                             {"configJson", "agent_runner_create_from_config_json"},
                             {"configPath", "agent_runner_create_from_config_path"},
#endif
                             {"hostModel", "agent_runner_create_with_host_model"},
                         })},
        {"versionNegotiation", agent::Value::object({
                                   {"negotiate", "agent_capi_negotiate_abi_version"},
                                   {"versionInfoJson", "agent_capi_version_info_json"},
                                   {"minSupportedAbiVersion", AGENT_CAPI_ABI_VERSION},
                                   {"maxSupportedAbiVersion", AGENT_CAPI_ABI_VERSION},
                               })},
        {"errorObject", agent::Value::object({
                            {"last", "agent_last_error_object"},
                            {"release", "agent_error_release"},
                            {"code", "agent_error_code"},
                            {"type", "agent_error_type"},
                            {"message", "agent_error_message"},
                            {"json", "agent_error_json"},
                        })},
        {"cancellation", agent::Value::object({
                             {"create", "agent_cancellation_create"},
                             {"cancel", "agent_cancellation_cancel"},
                             {"cancelled", "agent_cancellation_cancelled"},
                             {"reason", "agent_cancellation_reason"},
                             {"release", "agent_cancellation_release"},
                         })},
        {"hostRuntime", agent::Value::object({
                            {"create", "agent_host_runtime_create"},
                            {"release", "agent_host_runtime_release"},
                            {"describeJson", "agent_host_runtime_describe_json"},
                            {"createRunner", "agent_runner_create_with_host_model"},
                            {"callbacks", agent::Value::array({"model", "cancelled"})},
                        })},
        {"run", agent::Value::object({
                    {"string", result_shape},
                    {"json", result_shape},
                    {"stringWithCancellation", "agent_runner_run_with_cancellation"},
                    {"jsonWithCancellation", "agent_runner_run_json_with_cancellation"},
                })},
        {"stream", stream_shape},
        {"streamCallbacks", agent::Value::object({
                                {"stringWithCancellation", "agent_runner_stream_with_cancellation"},
                                {"jsonWithCancellation", "agent_runner_stream_json_with_cancellation"},
                                {"eventEncoding", "utf8-json"},
                                {"eventLifetime", "callback-only"},
                            })},
        {"asyncRunHandle", agent::Value::object({
                               {"startText", "agent_runner_run_async"},
                               {"startJson", "agent_runner_run_json_async"},
                               {"waitJson", "agent_run_wait_json"},
                               {"tryGetJson", "agent_run_try_get_json"},
                               {"cancel", "agent_run_cancel"},
                               {"release", "agent_run_release"},
                               {"result", result_shape},
                           })},
        {"asyncIterator", agent::Value::object({
                              {"openText", "agent_runner_stream_events"},
                              {"openJson", "agent_runner_stream_events_json"},
                              {"next", "agent_runner_event_stream_next_json"},
                              {"cancel", "agent_runner_event_stream_cancel"},
                              {"close", "agent_runner_event_stream_close"},
                              {"release", "agent_runner_event_stream_release"},
                              {"eventEncoding", "utf8-json"},
                              {"terminal", "next returns hasEvent=0 after done/error, cancel, or close"},
                          })},
#ifdef AGENT_CAPI_ENABLE_FULL
        {"asyncRun", agent::Value::object({
                         {"runtimeCreate", "agent_async_runtime_create"},
                         {"runtimeRelease", "agent_async_runtime_release"},
                         {"startJson", "agent_async_run_start_json"},
                         {"runOnce", "agent_async_runtime_run_once"},
                         {"getJson", "agent_async_run_get_json"},
                         {"listJson", "agent_async_run_list_json"},
                         {"eventsJson", "agent_async_run_events_json"},
                         {"checkpointsJson", "agent_async_run_checkpoints_json"},
                         {"transcriptJson", "agent_async_run_transcript_json"},
                         {"resumeJson", "agent_async_run_resume_json"},
                         {"cancelJson", "agent_async_run_cancel_json"},
                         {"encoding", "utf8-json"},
                         {"statuses", agent::Value::array({"queued", "running", "waiting",
                                                            "completed", "failed", "cancelled"})},
                     })},
#endif
        {"toolRun", agent::Value::object({
                        {"runtimeCreate", "agent_tool_run_runtime_create"},
                        {"runtimeRelease", "agent_tool_run_runtime_release"},
                        {"startJson", "agent_tool_run_start_json"},
                        {"statusJson", "agent_tool_run_status_json"},
                        {"listJson", "agent_tool_run_list_json"},
                        {"updateJson", "agent_tool_run_update_json"},
                        {"appendEventJson", "agent_tool_run_append_event_json"},
                        {"readJson", "agent_tool_run_read_json"},
                        {"cancelJson", "agent_tool_run_cancel_json"},
                        {"waitJson", "agent_tool_run_wait_json"},
                        {"encoding", "utf8-json"},
                        {"kinds", agent::Value::array({"custom", "process", "workflow", "remote-job"})},
                        {"statuses", agent::Value::array({"queued", "running", "waiting",
                                                           "completed", "failed", "cancelled"})},
                    })},
    }));
  }();
  return contract;
}

#ifdef AGENT_CAPI_ENABLE_FULL
void create_runner_from_resolved_app(agent::NativeResolvedAgentApp app, agent_runner_t** out_runner) {
  if (!app.runner) {
    throw agent::ConfigurationError("Resolved config did not produce a runner.");
  }
  assign_runner(out_runner, std::move(app.runner));
}
#endif

agent::AgentRunnerStreamEventHandler capi_stream_event_handler(agent_stream_callback_t on_event,
                                                               void* user_data,
                                                               agent::CancellationToken* cancellation) {
  return [on_event, user_data, cancellation, forwarding = true](const agent::AgentRunnerStreamEvent& event) mutable {
    if (!forwarding) {
      return;
    }
    const auto encoded = agent::safe_json_stringify(stream_event_to_value(event));
    if (on_event(encoded.c_str(), user_data) != 0) {
      forwarding = false;
      if (cancellation) {
        cancellation->cancel("C stream callback aborted.");
      }
    }
  };
}

agent::Value parse_optional_json_object(const char* json) {
  if (json == nullptr || json[0] == '\0') {
    return agent::Value::object({});
  }
  auto value = agent::parse_json(json);
  return value.is_object() ? value : agent::Value::object({});
}

agent::ToolRunStartOptions tool_run_start_options_from_value(const agent::Value& value) {
  agent::ToolRunStartOptions options;
  options.run_id = value.at("runId").as_string(value.at("run_id").as_string());
  options.tool_call_id = value.at("toolCallId").as_string(value.at("tool_call_id").as_string());
  options.tool_name = value.at("toolName").as_string(value.at("tool_name").as_string());
  options.kind = value.at("kind").as_string("custom");
  options.label = value.at("label").as_string();
  options.status = agent::tool_run_status_from_string(value.at("status").as_string("running"));
  options.ready = value.at("ready").as_bool(false);
  options.metadata = value.at("metadata").is_object() ? value.at("metadata") : agent::Value::object({});
  return options;
}

agent::ToolRunListOptions tool_run_list_options_from_value(const agent::Value& value) {
  agent::ToolRunListOptions options;
  const auto status = value.at("status").as_string();
  if (!status.empty()) {
    options.status = agent::tool_run_status_from_string(status);
  }
  options.kind = value.at("kind").as_string();
  options.tool_name = value.at("toolName").as_string(value.at("tool_name").as_string());
  options.active_only = value.at("activeOnly").as_bool(value.at("active_only").as_bool(false));
  options.limit = static_cast<std::size_t>(std::max<long long>(0, value.at("limit").as_integer(100)));
  return options;
}

agent::ToolRunUpdate tool_run_update_from_value(const agent::Value& value) {
  agent::ToolRunUpdate update;
  const auto status = value.at("status").as_string();
  if (!status.empty()) {
    update.status = agent::tool_run_status_from_string(status);
  }
  if (value.contains("ready")) {
    update.ready = value.at("ready").as_bool();
  }
  if (value.contains("error")) {
    update.error = value.at("error").is_null() ? std::string{} : value.at("error").as_string();
  }
  if (value.contains("metadata")) {
    update.metadata = value.at("metadata").is_object() ? value.at("metadata") : agent::Value::object({});
  }
  update.merge_metadata = value.at("mergeMetadata").as_bool(value.at("merge_metadata").as_bool(true));
  return update;
}

agent::ToolRunEventInput tool_run_event_input_from_value(const agent::Value& value) {
  agent::ToolRunEventInput event;
  event.type = value.at("type").as_string("event");
  event.stream = value.at("stream").as_string();
  event.text = value.at("text").as_string();
  event.message = value.at("message").as_string();
  event.payload = value.at("payload").is_null() ? agent::Value::object({}) : value.at("payload");
  event.metadata = value.at("metadata").is_object() ? value.at("metadata") : agent::Value::object({});
  return event;
}

agent::ToolRunReadOptions tool_run_read_options_from_value(const agent::Value& value) {
  agent::ToolRunReadOptions options;
  options.cursor = static_cast<std::uint64_t>(std::max<long long>(0, value.at("cursor").as_integer(0)));
  options.limit = static_cast<std::size_t>(std::max<long long>(0, value.at("limit").as_integer(100)));
  options.tail = static_cast<std::size_t>(std::max<long long>(0, value.at("tail").as_integer(0)));
  options.include_events = value.at("includeEvents").as_bool(value.at("include_events").as_bool(true));
  options.include_logs = value.at("includeLogs").as_bool(value.at("include_logs").as_bool(true));
  return options;
}

agent::ToolRunWaitOptions tool_run_wait_options_from_value(const agent::Value& value) {
  agent::ToolRunWaitOptions options;
  const auto until = value.at("until").as_string("terminal");
  options.until = until == "ready" ? agent::ToolRunWaitUntil::Ready : agent::ToolRunWaitUntil::Terminal;
  const auto status = value.at("status").as_string();
  if (!status.empty()) {
    options.status = agent::tool_run_status_from_string(status);
  }
  if (value.contains("ready")) {
    options.ready = value.at("ready").as_bool();
  }
  if (value.contains("terminal")) {
    options.terminal = value.at("terminal").as_bool();
  }
  options.timeout_ms = static_cast<int>(std::max<long long>(0, value.at("timeoutMs").as_integer(
                                                        value.at("timeout_ms").as_integer(0))));
  return options;
}

void assign_json(agent::Value value, char** out_json, std::string error_context) {
  if (out_json == nullptr) {
    throw agent::ConfigurationError(error_context + " out_json is null.");
  }
  *out_json = dup_cstr(agent::safe_json_stringify(value));
  if (*out_json == nullptr) {
    throw agent::AgentFrameworkError("Out of memory while serializing " + error_context + ".");
  }
}

}  // namespace

struct agent_runner_t {
  std::shared_ptr<agent::AgentRunner> runner;

  explicit agent_runner_t(std::shared_ptr<agent::AgentRunner> runner_ptr)
      : runner(std::move(runner_ptr)) {}
};

struct agent_runner_event_stream_t {
  agent::AgentRunnerEventStream stream;

  explicit agent_runner_event_stream_t(agent::AgentRunnerEventStream stream_value)
      : stream(std::move(stream_value)) {}
};

struct agent_error_t {
  LastErrorSnapshot snapshot;
};

struct agent_cancellation_t {
  std::shared_ptr<agent::CancellationToken> token;

  agent_cancellation_t()
      : token(std::make_shared<agent::CancellationToken>()) {}

  explicit agent_cancellation_t(std::shared_ptr<agent::CancellationToken> token_value)
      : token(std::move(token_value)) {}
};

struct agent_run_t {
  std::shared_ptr<agent::AgentRunner> runner;
  std::shared_ptr<agent::CancellationToken> cancellation;
  std::thread worker;
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool joined = false;
  int32_t status = AGENT_STATUS_OK;
  LastErrorSnapshot error;
  std::string result_json;

  ~agent_run_t() {
    if (worker.joinable()) {
      if (cancellation) {
        cancellation->cancel("Run handle released.");
      }
      worker.join();
    }
  }
};

struct agent_host_runtime_t {
  std::shared_ptr<CApiHostRuntimeState> state;

  explicit agent_host_runtime_t(std::shared_ptr<CApiHostRuntimeState> state_value)
      : state(std::move(state_value)) {}
};

#ifdef AGENT_CAPI_ENABLE_FULL
struct agent_async_runtime_t {
  std::shared_ptr<agent::AgentRunner> runner;
  agent::InMemoryTaskStore task_store;
  agent::InMemoryTaskQueue queue;
  agent::InMemoryRunTranscript transcript;
  agent::TaskBackedAsyncAgentRunStore async_store;
  agent::AsyncAgentRunWorker worker;
  agent::AsyncAgentRunController controller;

  explicit agent_async_runtime_t(std::shared_ptr<agent::AgentRunner> runner_ptr)
      : runner(std::move(runner_ptr)),
        queue(task_store),
        async_store(agent::TaskBackedAsyncAgentRunStoreConfig{
            .task_store = &task_store,
            .transcript = &transcript,
        }),
        worker(agent::AsyncAgentRunWorkerConfig{
            .task_store = &task_store,
            .queue = &queue,
            .store = &async_store,
            .transcript = &transcript,
            .runner = runner.get(),
        }),
        controller(agent::AsyncAgentRunControllerConfig{
            .store = &async_store,
            .queue = &queue,
            .worker = &worker,
            .transcript = &transcript,
        }) {}
};
#endif

struct agent_tool_run_runtime_t {
  agent::InMemoryToolRunManager manager;
};

namespace {

void assign_runner(agent_runner_t** out_runner, std::shared_ptr<agent::AgentRunner> runner) {
  require_out_runner(out_runner);
  *out_runner = nullptr;
  *out_runner = new agent_runner_t(std::move(runner));
}

std::set<std::string> capabilities_from_value(const agent::Value& value) {
  if (!value.is_array()) {
    return {"input.text"};
  }
  std::set<std::string> capabilities;
  for (const auto& item : value.as_array()) {
    if (item.is_string() && !item.as_string().empty()) {
      capabilities.insert(item.as_string());
    }
  }
  if (capabilities.empty()) {
    capabilities.insert("input.text");
  }
  return capabilities;
}

std::shared_ptr<agent::AgentRunner> make_host_runner(
    const std::shared_ptr<CApiHostRuntimeState>& state,
    const char* model_json) {
  agent::Value model_config = agent::Value::object({});
  if (model_json != nullptr && model_json[0] != '\0') {
    model_config = agent::parse_json(model_json);
    if (!model_config.is_object()) {
      throw agent::ConfigurationError("model_json must be a JSON object.");
    }
  }
  const std::string provider = model_config.at("provider").as_string("host");
  const std::string model = model_config.at("model").as_string("host-model");
  const double temperature = model_config.at("temperature").as_number(0.2);
  const int max_output_tokens = static_cast<int>(
      std::max<long long>(1, model_config.at("maxOutputTokens").as_integer(
                              model_config.at("max_output_tokens").as_integer(1024))));
  auto capabilities = capabilities_from_value(model_config.at("capabilities"));

  agent::AgentRunnerConfig config;
  config.model_runtime.adapter = std::make_shared<CApiHostModelAdapter>(
      state, provider, model, temperature, max_output_tokens, std::move(capabilities));
  config.context_runtime.max_iterations = static_cast<int>(
      std::max<long long>(1, model_config.at("maxIterations").as_integer(1)));
  return std::make_shared<agent::AgentRunner>(config);
}

agent::CancellationToken* token_from_handle(agent_cancellation_t* cancellation) {
  return cancellation && cancellation->token ? cancellation->token.get() : nullptr;
}

std::shared_ptr<agent::CancellationToken> shared_token_from_handle(agent_cancellation_t* cancellation) {
  return cancellation && cancellation->token ? cancellation->token
                                             : std::make_shared<agent::CancellationToken>();
}

std::string run_text_to_json(agent_runner_t* runner,
                             const char* input,
                             const char* session_id,
                             agent::CancellationToken* cancellation) {
  const std::string session = session_id ? session_id : "default";
  auto result = runner->runner->execution().run(std::string(input),
                                    session,
                                    {},
                                    {},
                                    {},
                                    {},
                                    agent::Value::object({}),
                                    std::nullopt,
                                    {},
                                    cancellation);
  return agent::safe_json_stringify(run_result_to_value(result));
}

std::string run_message_to_json(agent_runner_t* runner,
                                agent::AgentMessage input_message,
                                const char* session_id,
                                agent::CancellationToken* cancellation) {
  const std::string session = session_id ? session_id : "default";
  auto result = runner->runner->execution().run(std::move(input_message),
                                    session,
                                    {},
                                    {},
                                    {},
                                    {},
                                    agent::Value::object({}),
                                    std::nullopt,
                                    {},
                                    cancellation);
  return agent::safe_json_stringify(run_result_to_value(result));
}

void assign_owned_string(std::string value, char** out_json, const std::string& error_context) {
  if (out_json == nullptr) {
    throw agent::ConfigurationError(error_context + " out_json is null.");
  }
  *out_json = dup_cstr(value);
  if (*out_json == nullptr) {
    throw agent::AgentFrameworkError("Out of memory while serializing " + error_context + ".");
  }
}

void join_run_if_needed(agent_run_t* run) {
  if (run != nullptr && run->worker.joinable()) {
    run->worker.join();
    run->joined = true;
  }
}

void finish_run(agent_run_t* run,
                int32_t status,
                LastErrorSnapshot error,
                std::string result_json = {}) {
  {
    std::lock_guard<std::mutex> lock(run->mutex);
    run->status = status;
    run->error = std::move(error);
    run->result_json = std::move(result_json);
    run->done = true;
  }
  run->cv.notify_all();
}

void start_run_worker(agent_run_t* run,
                      std::function<std::string(agent::CancellationToken*)> operation) {
  run->worker = std::thread([run, operation = std::move(operation)] {
    int32_t status = guarded([&] {
      const std::string result_json = operation(run->cancellation.get());
      finish_run(run, AGENT_STATUS_OK, LastErrorSnapshot{}, result_json);
    });
    if (status != AGENT_STATUS_OK) {
      finish_run(run, status, g_last_error);
    }
  });
}

}  // namespace

extern "C" const char* agent_last_error(void) {
  return g_last_error.message.c_str();
}

extern "C" void agent_string_free(char* str) {
  std::free(str);
}

extern "C" char* agent_string_clone(const char* str) {
  return dup_cstr(str ? str : "");
}

extern "C" const char* agent_version(void) {
  return "agent_native " AGENT_NATIVE_VERSION;
}

extern "C" int32_t agent_capi_abi_version(void) {
  return AGENT_CAPI_ABI_VERSION;
}

extern "C" int32_t agent_capi_negotiate_abi_version(int32_t min_version,
                                                    int32_t max_version,
                                                    int32_t* out_version) {
  if (out_version == nullptr) {
    set_last_error("out_version must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_version = 0;
  return guarded([&] {
    if (min_version > AGENT_CAPI_ABI_VERSION || max_version < AGENT_CAPI_ABI_VERSION) {
      throw agent::ConfigurationError(
          "No compatible Agent C ABI version in requested range.");
    }
    *out_version = AGENT_CAPI_ABI_VERSION;
  });
}

extern "C" int32_t agent_capi_version_info_json(char** out_version_json) {
  if (out_version_json == nullptr) {
    set_last_error("out_version_json must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_version_json = nullptr;
  return guarded([&] {
    assign_json(agent::Value::object({
                    {"abiVersion", AGENT_CAPI_ABI_VERSION},
                    {"minSupportedAbiVersion", AGENT_CAPI_ABI_VERSION},
                    {"maxSupportedAbiVersion", AGENT_CAPI_ABI_VERSION},
                    {"version", std::string("agent_native ") + AGENT_NATIVE_VERSION},
                    {"features", agent::Value::array({
                                     "error-object",
                                     "host-model-vtable",
                                     "cancellation-handle",
                                     "async-run-handle",
                                     "usage-metadata",
                                     "stream-callback",
                                 })},
                }),
                out_version_json,
                "C ABI version info");
  });
}

extern "C" const char* agent_capi_contract_json(void) {
  return capi_contract_json_text().c_str();
}

extern "C" int32_t agent_last_error_object(agent_error_t** out_error) {
  if (out_error == nullptr) {
    set_last_error("out_error must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_error = nullptr;
  try {
    auto* error = new agent_error_t();
    error->snapshot = g_last_error;
    *out_error = error;
    return AGENT_STATUS_OK;
  } catch (const std::exception& error) {
    set_last_error(AGENT_STATUS_STD_EXCEPTION, "std::exception", error.what());
    return AGENT_STATUS_STD_EXCEPTION;
  } catch (...) {
    set_last_error(AGENT_STATUS_UNKNOWN_ERROR, "unknown", "unknown error");
    return AGENT_STATUS_UNKNOWN_ERROR;
  }
}

extern "C" void agent_error_release(agent_error_t* error) {
  delete error;
}

extern "C" int32_t agent_error_code(const agent_error_t* error) {
  return error ? error->snapshot.code : AGENT_STATUS_OK;
}

extern "C" const char* agent_error_type(const agent_error_t* error) {
  static const char* empty = "";
  return error ? error->snapshot.type.c_str() : empty;
}

extern "C" const char* agent_error_message(const agent_error_t* error) {
  static const char* empty = "";
  return error ? error->snapshot.message.c_str() : empty;
}

extern "C" int32_t agent_error_json(const agent_error_t* error, char** out_error_json) {
  if (error == nullptr || out_error_json == nullptr) {
    set_last_error("error and out_error_json must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_error_json = nullptr;
  return guarded([&] {
    assign_json(error_to_value(error->snapshot), out_error_json, "error object");
  });
}

extern "C" int32_t agent_cancellation_create(agent_cancellation_t** out_cancellation) {
  if (out_cancellation == nullptr) {
    set_last_error("out_cancellation must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_cancellation = nullptr;
  return guarded([&] {
    *out_cancellation = new agent_cancellation_t();
  });
}

extern "C" void agent_cancellation_cancel(agent_cancellation_t* cancellation, const char* reason) {
  if (cancellation && cancellation->token) {
    cancellation->token->cancel(reason ? reason : "cancelled");
  }
}

extern "C" int32_t agent_cancellation_cancelled(agent_cancellation_t* cancellation) {
  return cancellation && cancellation->token && cancellation->token->cancelled() ? 1 : 0;
}

extern "C" const char* agent_cancellation_reason(agent_cancellation_t* cancellation) {
  static thread_local std::string reason;
  reason = cancellation && cancellation->token ? cancellation->token->reason() : "";
  return reason.c_str();
}

extern "C" void agent_cancellation_release(agent_cancellation_t* cancellation) {
  delete cancellation;
}

extern "C" int32_t agent_host_runtime_create(const agent_host_vtable_t* vtable,
                                             agent_host_runtime_t** out_runtime) {
  if (vtable == nullptr || out_runtime == nullptr) {
    set_last_error("vtable and out_runtime must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_runtime = nullptr;
  return guarded([&] {
    if (vtable->size < sizeof(agent_host_vtable_t)) {
      throw agent::ConfigurationError("Host vtable size does not match Agent C ABI v4.");
    }
    auto state = std::make_shared<CApiHostRuntimeState>();
    state->vtable = *vtable;
    state->vtable.size = sizeof(agent_host_vtable_t);
    *out_runtime = new agent_host_runtime_t(std::move(state));
  });
}

extern "C" void agent_host_runtime_release(agent_host_runtime_t* runtime) {
  delete runtime;
}

extern "C" int32_t agent_host_runtime_describe_json(agent_host_runtime_t* runtime,
                                                    char** out_description_json) {
  if (runtime == nullptr || out_description_json == nullptr) {
    set_last_error("runtime and out_description_json must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_description_json = nullptr;
  return guarded([&] {
    const auto& vtable = runtime->state->vtable;
    assign_json(agent::Value::object({
                    {"abiVersion", AGENT_CAPI_ABI_VERSION},
                    {"size", static_cast<long long>(vtable.size)},
                    {"model", vtable.model_generate_json != nullptr},
                    {"cancelled", vtable.cancelled != nullptr},
                }),
                out_description_json,
                "host runtime description");
  });
}

extern "C" int32_t agent_runner_create_with_host_model(agent_host_runtime_t* runtime,
                                                       const char* model_json,
                                                       agent_runner_t** out_runner) {
  if (runtime == nullptr || out_runner == nullptr) {
    set_last_error("runtime and out_runner must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_runner = nullptr;
  return guarded([&] {
    if (!runtime->state || !runtime->state->vtable.model_generate_json) {
      throw agent::ConfigurationError("Host runtime requires model_generate_json.");
    }
    assign_runner(out_runner, make_host_runner(runtime->state, model_json));
  });
}

extern "C" int32_t agent_runner_create_with_echo_model(agent_runner_t** out_runner) {
  if (out_runner != nullptr) {
    *out_runner = nullptr;
  }
  return guarded([&] {
    assign_runner(out_runner, make_echo_runner());
  });
}

#ifdef AGENT_CAPI_ENABLE_FULL
extern "C" int32_t agent_runner_create_from_config_json(const char* config_json,
                                                        const char* requested_agent_id,
                                                        agent_runner_t** out_runner) {
  if (out_runner != nullptr) {
    *out_runner = nullptr;
  }
  if (config_json == nullptr) {
    set_last_error("config_json must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  return guarded([&] {
    auto loaded = agent::define_native_loaded_agent_config(
        agent::parse_json(config_json),
        std::filesystem::current_path());
    create_runner_from_resolved_app(
        agent::resolve_native_agent_app(std::move(loaded),
                                        requested_agent_id ? requested_agent_id : ""),
        out_runner);
  });
}

extern "C" int32_t agent_runner_create_from_config_path(const char* config_path,
                                                        const char* requested_agent_id,
                                                        agent_runner_t** out_runner) {
  if (out_runner != nullptr) {
    *out_runner = nullptr;
  }
  if (config_path == nullptr) {
    set_last_error("config_path must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  return guarded([&] {
    create_runner_from_resolved_app(
        agent::load_native_agent_app(std::filesystem::path(config_path),
                                     requested_agent_id ? requested_agent_id : "",
                                     agent::NativeConfigModuleLoader{}),
        out_runner);
  });
}
#endif

extern "C" void agent_runner_release(agent_runner_t* runner) {
  delete runner;
}

extern "C" int32_t agent_runner_run(agent_runner_t* runner,
                                    const char* input,
                                    const char* session_id,
                                    char** out_result_json) {
  return agent_runner_run_with_cancellation(runner, input, session_id, nullptr, out_result_json);
}

extern "C" int32_t agent_runner_run_with_cancellation(agent_runner_t* runner,
                                                      const char* input,
                                                      const char* session_id,
                                                      agent_cancellation_t* cancellation,
                                                      char** out_result_json) {
  if (runner == nullptr || input == nullptr || out_result_json == nullptr) {
    set_last_error("runner, input, and out_result_json must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_result_json = nullptr;
  return guarded([&] {
    assign_owned_string(run_text_to_json(runner, input, session_id, token_from_handle(cancellation)),
                        out_result_json,
                        "run result");
  });
}

extern "C" int32_t agent_runner_run_json(agent_runner_t* runner,
                                         const char* input_json,
                                         const char* session_id,
                                         char** out_result_json) {
  return agent_runner_run_json_with_cancellation(runner, input_json, session_id, nullptr, out_result_json);
}

extern "C" int32_t agent_runner_run_json_with_cancellation(agent_runner_t* runner,
                                                           const char* input_json,
                                                           const char* session_id,
                                                           agent_cancellation_t* cancellation,
                                                           char** out_result_json) {
  if (runner == nullptr || input_json == nullptr || out_result_json == nullptr) {
    set_last_error("runner, input_json, and out_result_json must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_result_json = nullptr;
  return guarded([&] {
    assign_owned_string(run_message_to_json(runner,
                                            capi_input_message_from_json_text(input_json),
                                            session_id,
                                            token_from_handle(cancellation)),
                        out_result_json,
                        "JSON run result");
  });
}

extern "C" int32_t agent_runner_stream(agent_runner_t* runner,
                                       const char* input,
                                       const char* session_id,
                                       agent_stream_callback_t on_event,
                                       void* user_data) {
  return agent_runner_stream_with_cancellation(runner, input, session_id, nullptr, on_event, user_data);
}

extern "C" int32_t agent_runner_stream_with_cancellation(agent_runner_t* runner,
                                                         const char* input,
                                                         const char* session_id,
                                                         agent_cancellation_t* cancellation,
                                                         agent_stream_callback_t on_event,
                                                         void* user_data) {
  if (runner == nullptr || input == nullptr || on_event == nullptr) {
    set_last_error("runner, input, and on_event must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  return guarded([&] {
    const std::string session = session_id ? session_id : "default";
    agent::CancellationToken local_cancellation;
    agent::CancellationToken* effective_cancellation = token_from_handle(cancellation);
    if (effective_cancellation == nullptr) {
      effective_cancellation = &local_cancellation;
    }
    try {
      (void)runner->runner->streaming().stream(std::string(input),
                                   capi_stream_event_handler(on_event, user_data, effective_cancellation),
                                   session, {}, {}, {}, {}, agent::Value::object({}), std::nullopt,
                                   effective_cancellation);
    } catch (const agent::CancellationError&) {
      if (!effective_cancellation->cancelled()) {
        throw;
      }
    }
  });
}

extern "C" int32_t agent_runner_stream_json(agent_runner_t* runner,
                                            const char* input_json,
                                            const char* session_id,
                                            agent_stream_callback_t on_event,
                                            void* user_data) {
  return agent_runner_stream_json_with_cancellation(runner,
                                                   input_json,
                                                   session_id,
                                                   nullptr,
                                                   on_event,
                                                   user_data);
}

extern "C" int32_t agent_runner_stream_json_with_cancellation(agent_runner_t* runner,
                                                              const char* input_json,
                                                              const char* session_id,
                                                              agent_cancellation_t* cancellation,
                                                              agent_stream_callback_t on_event,
                                                              void* user_data) {
  if (runner == nullptr || input_json == nullptr || on_event == nullptr) {
    set_last_error("runner, input_json, and on_event must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  return guarded([&] {
    const std::string session = session_id ? session_id : "default";
    agent::CancellationToken local_cancellation;
    agent::CancellationToken* effective_cancellation = token_from_handle(cancellation);
    if (effective_cancellation == nullptr) {
      effective_cancellation = &local_cancellation;
    }
    try {
      (void)runner->runner->streaming().stream(capi_input_message_from_json_text(input_json),
                                   capi_stream_event_handler(on_event, user_data, effective_cancellation),
                                   session, {}, {}, {}, {}, agent::Value::object({}), std::nullopt,
                                   effective_cancellation);
    } catch (const agent::CancellationError&) {
      if (!effective_cancellation->cancelled()) {
        throw;
      }
    }
  });
}

extern "C" int32_t agent_runner_run_async(agent_runner_t* runner,
                                          const char* input,
                                          const char* session_id,
                                          agent_cancellation_t* cancellation,
                                          agent_run_t** out_run) {
  if (runner == nullptr || input == nullptr || out_run == nullptr) {
    set_last_error("runner, input, and out_run must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_run = nullptr;
  return guarded([&] {
    auto* run = new agent_run_t();
    run->runner = runner->runner;
    run->cancellation = shared_token_from_handle(cancellation);
    const std::string input_text(input);
    const std::string session = session_id ? session_id : "default";
    start_run_worker(run, [run, input_text, session](agent::CancellationToken* token) {
      agent_runner_t runner_handle(run->runner);
      return run_text_to_json(&runner_handle, input_text.c_str(), session.c_str(), token);
    });
    *out_run = run;
  });
}

extern "C" int32_t agent_runner_run_json_async(agent_runner_t* runner,
                                               const char* input_json,
                                               const char* session_id,
                                               agent_cancellation_t* cancellation,
                                               agent_run_t** out_run) {
  if (runner == nullptr || input_json == nullptr || out_run == nullptr) {
    set_last_error("runner, input_json, and out_run must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_run = nullptr;
  return guarded([&] {
    auto* run = new agent_run_t();
    run->runner = runner->runner;
    run->cancellation = shared_token_from_handle(cancellation);
    const std::string input_text(input_json);
    const std::string session = session_id ? session_id : "default";
    start_run_worker(run, [run, input_text, session](agent::CancellationToken* token) {
      agent_runner_t runner_handle(run->runner);
      return run_message_to_json(&runner_handle,
                                 capi_input_message_from_json_text(input_text.c_str()),
                                 session.c_str(),
                                 token);
    });
    *out_run = run;
  });
}

extern "C" int32_t agent_run_wait_json(agent_run_t* run, char** out_result_json) {
  if (run == nullptr || out_result_json == nullptr) {
    set_last_error("run and out_result_json must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_result_json = nullptr;
  join_run_if_needed(run);
  std::unique_lock<std::mutex> lock(run->mutex);
  run->cv.wait(lock, [&] { return run->done; });
  if (run->status != AGENT_STATUS_OK) {
    set_last_error(run->error.code, run->error.type, run->error.message);
    return run->status;
  }
  return guarded([&] {
    assign_owned_string(run->result_json, out_result_json, "async run result");
  });
}

extern "C" int32_t agent_run_try_get_json(agent_run_t* run,
                                          char** out_result_json,
                                          int32_t* out_done) {
  if (run == nullptr || out_result_json == nullptr || out_done == nullptr) {
    set_last_error("run, out_result_json, and out_done must be non-null.");
    return AGENT_STATUS_FRAMEWORK_ERROR;
  }
  *out_result_json = nullptr;
  *out_done = 0;
  std::lock_guard<std::mutex> lock(run->mutex);
  if (!run->done) {
    return AGENT_STATUS_OK;
  }
  *out_done = 1;
  if (run->status != AGENT_STATUS_OK) {
    set_last_error(run->error.code, run->error.type, run->error.message);
    return run->status;
  }
  return guarded([&] {
    assign_owned_string(run->result_json, out_result_json, "async run result");
  });
}

extern "C" void agent_run_cancel(agent_run_t* run, const char* reason) {
  if (run && run->cancellation) {
    run->cancellation->cancel(reason ? reason : "cancelled");
  }
}

extern "C" void agent_run_release(agent_run_t* run) {
  delete run;
}

extern "C" int32_t agent_runner_stream_events(agent_runner_t* runner,
                                              const char* input,
                                              const char* session_id,
                                              size_t capacity,
                                              agent_runner_event_stream_t** out_stream) {
  if (runner == nullptr || input == nullptr || out_stream == nullptr) {
    set_last_error("runner, input, and out_stream must be non-null.");
    return 1;
  }
  *out_stream = nullptr;
  return guarded([&] {
    const std::string session = session_id ? session_id : "default";
    agent::StreamQueueOptions options;
    if (capacity > 0) {
      options.capacity = capacity;
    }
    auto stream = runner->runner->streaming().events(std::string(input), options, session);
    *out_stream = new agent_runner_event_stream_t(std::move(stream));
  });
}

extern "C" int32_t agent_runner_stream_events_json(agent_runner_t* runner,
                                                   const char* input_json,
                                                   const char* session_id,
                                                   size_t capacity,
                                                   agent_runner_event_stream_t** out_stream) {
  if (runner == nullptr || input_json == nullptr || out_stream == nullptr) {
    set_last_error("runner, input_json, and out_stream must be non-null.");
    return 1;
  }
  *out_stream = nullptr;
  return guarded([&] {
    const std::string session = session_id ? session_id : "default";
    agent::StreamQueueOptions options;
    if (capacity > 0) {
      options.capacity = capacity;
    }
    auto stream = runner->runner->streaming().events(capi_input_message_from_json_text(input_json),
                                                options,
                                                session);
    *out_stream = new agent_runner_event_stream_t(std::move(stream));
  });
}

extern "C" int32_t agent_runner_event_stream_next_json(agent_runner_event_stream_t* stream,
                                                       char** out_event_json,
                                                       int32_t* out_has_event) {
  if (stream == nullptr || out_event_json == nullptr || out_has_event == nullptr) {
    set_last_error("stream, out_event_json, and out_has_event must be non-null.");
    return 1;
  }
  *out_event_json = nullptr;
  *out_has_event = 0;
  return guarded([&] {
    agent::AgentRunnerStreamEvent event;
    if (!stream->stream.next(event)) {
      return;
    }
    auto* encoded = dup_cstr(agent::safe_json_stringify(stream_event_to_value(event)));
    if (encoded == nullptr) {
      throw agent::AgentFrameworkError("Out of memory while serializing stream event.");
    }
    *out_event_json = encoded;
    *out_has_event = 1;
  });
}

extern "C" void agent_runner_event_stream_close(agent_runner_event_stream_t* stream) {
  if (stream) {
    stream->stream.close();
  }
}

extern "C" void agent_runner_event_stream_cancel(agent_runner_event_stream_t* stream,
                                                 const char* reason) {
  if (stream) {
    stream->stream.cancel(reason ? reason : "Stream cancelled.");
  }
}

extern "C" void agent_runner_event_stream_release(agent_runner_event_stream_t* stream) {
  delete stream;
}

#ifdef AGENT_CAPI_ENABLE_FULL
extern "C" int32_t agent_async_runtime_create(agent_runner_t* runner,
                                              agent_async_runtime_t** out_runtime) {
  if (runner == nullptr || out_runtime == nullptr) {
    set_last_error("runner and out_runtime must be non-null.");
    return 1;
  }
  *out_runtime = nullptr;
  return guarded([&] {
    *out_runtime = new agent_async_runtime_t(runner->runner);
  });
}

extern "C" void agent_async_runtime_release(agent_async_runtime_t* runtime) {
  delete runtime;
}

extern "C" int32_t agent_async_run_start_json(agent_async_runtime_t* runtime,
                                              const char* start_json,
                                              char** out_snapshot_json) {
  if (runtime == nullptr || start_json == nullptr || out_snapshot_json == nullptr) {
    set_last_error("runtime, start_json, and out_snapshot_json must be non-null.");
    return 1;
  }
  *out_snapshot_json = nullptr;
  return guarded([&] {
    auto snapshot = runtime->controller.start(
        agent::async_agent_run_start_input_from_value(agent::parse_json(start_json)));
    assign_json(agent::async_agent_run_snapshot_to_value(snapshot),
                out_snapshot_json,
                "async run start result");
  });
}

extern "C" int32_t agent_async_runtime_run_once(agent_async_runtime_t* runtime,
                                                int32_t* out_processed) {
  if (runtime == nullptr || out_processed == nullptr) {
    set_last_error("runtime and out_processed must be non-null.");
    return 1;
  }
  *out_processed = 0;
  return guarded([&] {
    *out_processed = runtime->controller.run_once() ? 1 : 0;
  });
}

extern "C" int32_t agent_async_run_get_json(agent_async_runtime_t* runtime,
                                            const char* run_id,
                                            char** out_snapshot_json) {
  if (runtime == nullptr || run_id == nullptr || out_snapshot_json == nullptr) {
    set_last_error("runtime, run_id, and out_snapshot_json must be non-null.");
    return 1;
  }
  *out_snapshot_json = nullptr;
  return guarded([&] {
    auto snapshot = runtime->controller.get(run_id);
    if (!snapshot) {
      throw agent::AgentFrameworkError(std::string("Async agent run not found: ") + run_id);
    }
    assign_json(agent::async_agent_run_snapshot_to_value(*snapshot),
                out_snapshot_json,
                "async run snapshot");
  });
}

extern "C" int32_t agent_async_run_list_json(agent_async_runtime_t* runtime,
                                             const char* filter_json,
                                             char** out_runs_json) {
  if (runtime == nullptr || out_runs_json == nullptr) {
    set_last_error("runtime and out_runs_json must be non-null.");
    return 1;
  }
  *out_runs_json = nullptr;
  return guarded([&] {
    auto filter = agent::async_agent_run_filter_from_value(parse_optional_json_object(filter_json));
    agent::Value::Array runs;
    for (const auto& run : runtime->controller.list(filter)) {
      runs.push_back(agent::async_agent_run_to_value(run));
    }
    assign_json(agent::Value(std::move(runs)), out_runs_json, "async run list");
  });
}

extern "C" int32_t agent_async_run_events_json(agent_async_runtime_t* runtime,
                                               const char* run_id,
                                               char** out_events_json) {
  if (runtime == nullptr || run_id == nullptr || out_events_json == nullptr) {
    set_last_error("runtime, run_id, and out_events_json must be non-null.");
    return 1;
  }
  *out_events_json = nullptr;
  return guarded([&] {
    agent::Value::Array events;
    for (const auto& event : runtime->controller.events(run_id)) {
      events.push_back(agent::async_agent_run_event_to_value(event));
    }
    assign_json(agent::Value(std::move(events)), out_events_json, "async run events");
  });
}

extern "C" int32_t agent_async_run_checkpoints_json(agent_async_runtime_t* runtime,
                                                    const char* run_id,
                                                    char** out_checkpoints_json) {
  if (runtime == nullptr || run_id == nullptr || out_checkpoints_json == nullptr) {
    set_last_error("runtime, run_id, and out_checkpoints_json must be non-null.");
    return 1;
  }
  *out_checkpoints_json = nullptr;
  return guarded([&] {
    agent::Value::Array checkpoints;
    for (const auto& checkpoint : runtime->controller.checkpoints(run_id)) {
      checkpoints.push_back(agent::async_agent_run_checkpoint_to_value(checkpoint));
    }
    assign_json(agent::Value(std::move(checkpoints)), out_checkpoints_json, "async run checkpoints");
  });
}

extern "C" int32_t agent_async_run_transcript_json(agent_async_runtime_t* runtime,
                                                   const char* run_id,
                                                   char** out_transcript_json) {
  if (runtime == nullptr || run_id == nullptr || out_transcript_json == nullptr) {
    set_last_error("runtime, run_id, and out_transcript_json must be non-null.");
    return 1;
  }
  *out_transcript_json = nullptr;
  return guarded([&] {
    agent::Value::Array transcript;
    for (const auto& entry : runtime->controller.transcript(run_id)) {
      transcript.push_back(agent::run_transcript_entry_to_value(entry));
    }
    assign_json(agent::Value(std::move(transcript)), out_transcript_json, "async run transcript");
  });
}

extern "C" int32_t agent_async_run_resume_json(agent_async_runtime_t* runtime,
                                               const char* run_id,
                                               const char* resume_json,
                                               char** out_snapshot_json) {
  if (runtime == nullptr || run_id == nullptr || out_snapshot_json == nullptr) {
    set_last_error("runtime, run_id, and out_snapshot_json must be non-null.");
    return 1;
  }
  *out_snapshot_json = nullptr;
  return guarded([&] {
    auto snapshot = runtime->controller.resume(
        run_id,
        agent::async_agent_run_resume_input_from_value(parse_optional_json_object(resume_json)));
    assign_json(agent::async_agent_run_snapshot_to_value(snapshot),
                out_snapshot_json,
                "async run resume result");
  });
}

extern "C" int32_t agent_async_run_cancel_json(agent_async_runtime_t* runtime,
                                               const char* run_id,
                                               const char* cancel_json,
                                               char** out_snapshot_json) {
  if (runtime == nullptr || run_id == nullptr || out_snapshot_json == nullptr) {
    set_last_error("runtime, run_id, and out_snapshot_json must be non-null.");
    return 1;
  }
  *out_snapshot_json = nullptr;
  return guarded([&] {
    auto snapshot = runtime->controller.cancel(
        run_id,
        agent::async_agent_run_cancel_input_from_value(parse_optional_json_object(cancel_json)));
    assign_json(agent::async_agent_run_snapshot_to_value(snapshot),
                out_snapshot_json,
                "async run cancel result");
  });
}
#endif

extern "C" int32_t agent_tool_run_runtime_create(agent_tool_run_runtime_t** out_runtime) {
  if (out_runtime == nullptr) {
    set_last_error("out_runtime must be non-null.");
    return 1;
  }
  *out_runtime = nullptr;
  return guarded([&] {
    *out_runtime = new agent_tool_run_runtime_t();
  });
}

extern "C" void agent_tool_run_runtime_release(agent_tool_run_runtime_t* runtime) {
  delete runtime;
}

extern "C" int32_t agent_tool_run_start_json(agent_tool_run_runtime_t* runtime,
                                             const char* start_json,
                                             char** out_snapshot_json) {
  if (runtime == nullptr || start_json == nullptr || out_snapshot_json == nullptr) {
    set_last_error("runtime, start_json, and out_snapshot_json must be non-null.");
    return 1;
  }
  *out_snapshot_json = nullptr;
  return guarded([&] {
    auto snapshot = runtime->manager.start(tool_run_start_options_from_value(agent::parse_json(start_json)));
    assign_json(agent::tool_run_snapshot_to_value(snapshot), out_snapshot_json, "tool run start result");
  });
}

extern "C" int32_t agent_tool_run_status_json(agent_tool_run_runtime_t* runtime,
                                              const char* run_id,
                                              char** out_snapshot_json) {
  if (runtime == nullptr || run_id == nullptr || out_snapshot_json == nullptr) {
    set_last_error("runtime, run_id, and out_snapshot_json must be non-null.");
    return 1;
  }
  *out_snapshot_json = nullptr;
  return guarded([&] {
    auto snapshot = runtime->manager.get(run_id);
    if (!snapshot) {
      throw agent::AgentFrameworkError(std::string("Tool run not found: ") + run_id);
    }
    assign_json(agent::tool_run_snapshot_to_value(*snapshot), out_snapshot_json, "tool run snapshot");
  });
}

extern "C" int32_t agent_tool_run_list_json(agent_tool_run_runtime_t* runtime,
                                            const char* filter_json,
                                            char** out_runs_json) {
  if (runtime == nullptr || out_runs_json == nullptr) {
    set_last_error("runtime and out_runs_json must be non-null.");
    return 1;
  }
  *out_runs_json = nullptr;
  return guarded([&] {
    auto runs = runtime->manager.list(tool_run_list_options_from_value(parse_optional_json_object(filter_json)));
    assign_json(agent::tool_run_snapshots_to_value(runs), out_runs_json, "tool run list");
  });
}

extern "C" int32_t agent_tool_run_update_json(agent_tool_run_runtime_t* runtime,
                                              const char* run_id,
                                              const char* update_json,
                                              char** out_snapshot_json) {
  if (runtime == nullptr || run_id == nullptr || update_json == nullptr || out_snapshot_json == nullptr) {
    set_last_error("runtime, run_id, update_json, and out_snapshot_json must be non-null.");
    return 1;
  }
  *out_snapshot_json = nullptr;
  return guarded([&] {
    auto snapshot = runtime->manager.update(run_id, tool_run_update_from_value(agent::parse_json(update_json)));
    assign_json(agent::tool_run_snapshot_to_value(snapshot), out_snapshot_json, "tool run update result");
  });
}

extern "C" int32_t agent_tool_run_append_event_json(agent_tool_run_runtime_t* runtime,
                                                    const char* run_id,
                                                    const char* event_json,
                                                    char** out_event_json) {
  if (runtime == nullptr || run_id == nullptr || event_json == nullptr || out_event_json == nullptr) {
    set_last_error("runtime, run_id, event_json, and out_event_json must be non-null.");
    return 1;
  }
  *out_event_json = nullptr;
  return guarded([&] {
    auto event = runtime->manager.append_event(run_id, tool_run_event_input_from_value(agent::parse_json(event_json)));
    assign_json(agent::tool_run_event_to_value(event), out_event_json, "tool run event");
  });
}

extern "C" int32_t agent_tool_run_read_json(agent_tool_run_runtime_t* runtime,
                                            const char* run_id,
                                            const char* read_json,
                                            char** out_read_json) {
  if (runtime == nullptr || run_id == nullptr || out_read_json == nullptr) {
    set_last_error("runtime, run_id, and out_read_json must be non-null.");
    return 1;
  }
  *out_read_json = nullptr;
  return guarded([&] {
    auto result = runtime->manager.read(run_id, tool_run_read_options_from_value(parse_optional_json_object(read_json)));
    assign_json(agent::tool_run_read_result_to_value(result), out_read_json, "tool run read result");
  });
}

extern "C" int32_t agent_tool_run_cancel_json(agent_tool_run_runtime_t* runtime,
                                              const char* run_id,
                                              const char* cancel_json,
                                              char** out_snapshot_json) {
  if (runtime == nullptr || run_id == nullptr || out_snapshot_json == nullptr) {
    set_last_error("runtime, run_id, and out_snapshot_json must be non-null.");
    return 1;
  }
  *out_snapshot_json = nullptr;
  return guarded([&] {
    const auto input = parse_optional_json_object(cancel_json);
    auto snapshot = runtime->manager.cancel(run_id, input.at("reason").as_string());
    assign_json(agent::tool_run_snapshot_to_value(snapshot), out_snapshot_json, "tool run cancel result");
  });
}

extern "C" int32_t agent_tool_run_wait_json(agent_tool_run_runtime_t* runtime,
                                            const char* run_id,
                                            const char* wait_json,
                                            char** out_snapshot_json) {
  if (runtime == nullptr || run_id == nullptr || out_snapshot_json == nullptr) {
    set_last_error("runtime, run_id, and out_snapshot_json must be non-null.");
    return 1;
  }
  *out_snapshot_json = nullptr;
  return guarded([&] {
    auto snapshot = runtime->manager.wait(run_id, tool_run_wait_options_from_value(parse_optional_json_object(wait_json)));
    assign_json(snapshot ? agent::tool_run_snapshot_to_value(*snapshot) : agent::Value(),
                out_snapshot_json,
                "tool run wait result");
  });
}
