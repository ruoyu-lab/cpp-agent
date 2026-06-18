#include "agent_capi_full.h"
#include "agent/full.hpp"
#include "agent/react.hpp"
#include "../src/agent/function_calling_loop.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {

struct CApiOwnedString {
  char* value = nullptr;

  CApiOwnedString() = default;

  ~CApiOwnedString() {
    agent_string_free(value);
  }

  CApiOwnedString(const CApiOwnedString&) = delete;
  CApiOwnedString& operator=(const CApiOwnedString&) = delete;
};

struct CApiStreamCapture {
  std::vector<agent::Value> events;
};

class CapturingPromptModel final : public agent::ChatModelAdapter {
 public:
  CapturingPromptModel() : ChatModelAdapter("fixture", "capturing-prompt-model") {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    messages = params.messages;
    return build_output(agent::AgentOutput{.text = "Thought: answer directly.\nFinal Answer: ok"});
  }

  std::vector<agent::AgentMessage> messages;
};

class CustomPromptBuilder final : public agent::ReActPromptBuilder {
 public:
  agent::AgentMessage build_system_message(const agent::ReActPromptBuilderInput&) const override {
    return agent::create_message(agent::MessageRole::System,
                                 "CUSTOM_TOOL_FORMAT_SENTINEL\nFinal Answer: user-facing answer only",
                                 agent::Value::object({{"source", "host-react-prompt"}}));
  }
};

class AsyncRuntimeToolModel final : public agent::ChatModelAdapter {
 public:
  AsyncRuntimeToolModel() : ChatModelAdapter("fixture", "async-runtime-tool-model") {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    ++calls_;
    if (calls_ == 1) {
      return build_output(agent::AgentOutput{
          .id = "async_model_tool",
          .tool_calls = {agent::ToolCall{
              .id = "async_tool_1",
              .name = "contract.echo",
              .arguments = agent::Value::object({{"text", "from-tool"}}),
          }},
          .finish_reason = "tool_calls",
          .raw = agent::Value::object({{"usage", agent::Value::object({
                                                     {"inputTokens", 5},
                                                     {"outputTokens", 1},
                                                     {"totalTokens", 6},
                                                 })}}),
      });
    }
    return build_output(agent::AgentOutput{
        .id = "async_model_final",
        .text = "async child runtime ok",
        .raw = agent::Value::object({{"usage", agent::Value::object({
                                                   {"inputTokens", 2},
                                                   {"outputTokens", 4},
                                                   {"totalTokens", 6},
                                               })}}),
    });
  }

  void stream(const agent::GenerateParams&, StreamEventHandler on_event) override {
    ++calls_;
    if (calls_ == 1) {
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::ToolCallDelta,
          .provider = provider(),
          .model = model(),
          .tool_call_id = "async_tool_1",
          .tool_call_name = "contract.echo",
          .tool_call_args_delta = "{\"text\"",
          .tool_call_args_accumulated = "{\"text\"",
      });
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::ToolCallDelta,
          .provider = provider(),
          .model = model(),
          .tool_call_id = "async_tool_1",
          .tool_call_name = "contract.echo",
          .tool_call_args_delta = ":\"from-tool\"}",
          .tool_call_args_accumulated = "{\"text\":\"from-tool\"}",
      });
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::Response,
          .provider = provider(),
          .model = model(),
          .response = build_output(agent::AgentOutput{
              .id = "async_model_tool",
              .tool_calls = {agent::ToolCall{
                  .id = "async_tool_1",
                  .name = "contract.echo",
                  .arguments = agent::Value::object({{"text", "from-tool"}}),
              }},
              .finish_reason = "tool_calls",
              .raw = agent::Value::object({{"usage", agent::Value::object({
                                                         {"inputTokens", 5},
                                                         {"outputTokens", 1},
                                                         {"totalTokens", 6},
                                                     })}}),
          }),
      });
      return;
    }
    on_event(agent::ModelStreamEvent{
        .type = agent::ModelStreamEventType::Response,
        .provider = provider(),
        .model = model(),
        .response = build_output(agent::AgentOutput{
            .id = "async_model_final",
            .text = "async child runtime ok",
            .raw = agent::Value::object({{"usage", agent::Value::object({
                                                       {"inputTokens", 2},
                                                       {"outputTokens", 4},
                                                       {"totalTokens", 6},
                                                   })}}),
        }),
    });
  }

 private:
  int calls_ = 0;
};

int capture_capi_stream_event(const char* event_json, void* user_data) {
  auto* capture = static_cast<CApiStreamCapture*>(user_data);
  capture->events.push_back(agent::parse_json(event_json));
  return 0;
}

std::string joined_message_text(const std::vector<agent::AgentMessage>& messages) {
  std::string text;
  for (const auto& message : messages) {
    text += agent::extract_text_content(message.content);
    text += "\n";
  }
  return text;
}

void require_contains(const std::string& text, const std::string& needle, const std::string& label) {
  if (text.find(needle) == std::string::npos) {
    throw std::runtime_error(label + " missing expected text: " + needle);
  }
}

void require_not_contains(const std::string& text, const std::string& needle, const std::string& label) {
  if (text.find(needle) != std::string::npos) {
    throw std::runtime_error(label + " contains unexpected text: " + needle);
  }
}

void run_react_prompt_mode_contracts() {
  {
    auto model = std::make_shared<CapturingPromptModel>();
    agent::AgentRunner runner(agent::AgentRunnerConfig{
        .model_runtime = {.adapter = model},
        .governance = {.enable_planning = false},
    });
    (void)runner.execution().run("hello", "managed-prompt-contract");
    const auto prompt = joined_message_text(model->messages);
    require_contains(prompt, "internal tool-use format", "managed prompt");
    require_not_contains(prompt, "Use the ReAct protocol", "managed prompt");
  }

  {
    auto model = std::make_shared<CapturingPromptModel>();
    agent::AgentRunner runner(agent::AgentRunnerConfig{
        .model_runtime = {.adapter = model},
        .governance = {.enable_planning = false},
        .react_runtime = {
            .prompt_mode = agent::ReActPromptMode::Custom,
            .prompt_builder = std::make_shared<CustomPromptBuilder>(),
        },
    });
    (void)runner.execution().run("hello", "custom-prompt-contract");
    const auto prompt = joined_message_text(model->messages);
    require_contains(prompt, "CUSTOM_TOOL_FORMAT_SENTINEL", "custom prompt");
    require_not_contains(prompt, "internal tool-use format", "custom prompt");
  }

  {
    auto model = std::make_shared<CapturingPromptModel>();
    agent::AgentRunner runner(agent::AgentRunnerConfig{
        .model_runtime = {.adapter = model},
        .context_runtime = {.system_prompt = "HOST_OWNS_FULL_TOOL_PROMPT"},
        .governance = {.enable_planning = false},
        .react_runtime = {.prompt_mode = agent::ReActPromptMode::External},
    });
    (void)runner.execution().run("hello", "external-prompt-contract");
    const auto prompt = joined_message_text(model->messages);
    require_contains(prompt, "HOST_OWNS_FULL_TOOL_PROMPT", "external prompt");
    require_not_contains(prompt, "internal tool-use format", "external prompt");
    require_not_contains(prompt, "Action Input: compact JSON object", "external prompt");
  }

  {
    auto model = std::make_shared<CapturingPromptModel>();
    bool failed = false;
    try {
      agent::AgentRunner runner(agent::AgentRunnerConfig{
          .model_runtime = {.adapter = model},
          .governance = {.enable_planning = false},
          .react_runtime = {.prompt_mode = agent::ReActPromptMode::Custom},
      });
      (void)runner.execution().run("hello", "custom-prompt-missing-builder");
    } catch (const agent::ConfigurationError&) {
      failed = true;
    }
    if (!failed) {
      throw std::runtime_error("custom prompt mode must require react_prompt_builder");
    }
  }
}

void run_agent_runtime_builder_contracts() {
  auto model = std::make_shared<CapturingPromptModel>();
  auto runner = agent::AgentRuntimeBuilder()
                    .model(model)
                    .system_prompt("BUILDER_SYSTEM_PROMPT")
                    .max_iterations(1)
                    .enable_planning(false)
                    .build();

  const auto result = runner.execution().run("hello", "runtime-builder-contract");
  if (result.text != "ok") {
    throw std::runtime_error("AgentRuntimeBuilder runner returned unexpected text: " + result.text);
  }
  require_contains(joined_message_text(model->messages),
                   "BUILDER_SYSTEM_PROMPT",
                   "runtime builder prompt");
}

void run_preset_contracts() {
  agent::StandardRuntimePresetOptions standard_options;
  standard_options.system_prompt = "STANDARD_PRESET_SYSTEM_PROMPT";
  standard_options.max_iterations = 1;
  standard_options.enable_planning = false;

  auto standard_runner = agent::create_standard_runtime(std::move(standard_options)).build();
  const auto standard_result = standard_runner.execution().run("hello", "standard-preset-contract");
  if (standard_result.text != "hello") {
    throw std::runtime_error("create_standard_runtime returned unexpected text: " +
                             standard_result.text);
  }

  bool missing_local_model_failed = false;
  try {
    (void)agent::create_local_model_runtime({});
  } catch (const agent::ConfigurationError&) {
    missing_local_model_failed = true;
  }
  if (!missing_local_model_failed) {
    throw std::runtime_error("create_local_model_runtime must require a local model source");
  }

  agent::LocalModelRuntimePresetOptions local_options;
  auto local_model = std::make_shared<CapturingPromptModel>();
  local_options.runtime.model = local_model;
  local_options.runtime.tool_bundles = {};
  local_options.runtime.max_iterations = 1;
  local_options.runtime.enable_planning = false;

  auto local_runner = agent::create_local_model_runtime(std::move(local_options)).build();
  const auto local_result = local_runner.execution().run("hello", "local-model-preset-contract");
  if (local_result.text != "ok") {
    throw std::runtime_error("create_local_model_runtime returned unexpected text: " +
                             local_result.text);
  }

  bool unauthenticated_server_failed = false;
  try {
    (void)agent::create_server_app({});
  } catch (const agent::ConfigurationError&) {
    unauthenticated_server_failed = true;
  }
  if (!unauthenticated_server_failed) {
    throw std::runtime_error("create_server_app must preserve fail-closed server posture");
  }

  agent::AgentServerOptions server_options;
  server_options.allow_unauthenticated = true;
  server_options.runner = &local_runner;
  (void)agent::create_server_app(std::move(server_options));
}

void require_equal(const agent::Value& actual,
                   const agent::Value& expected,
                   const std::string& case_name,
                   const std::string& fixture_name) {
  if (actual == expected) {
    return;
  }
  std::cerr << "Contract mismatch in " << fixture_name << " / " << case_name << "\n"
            << "Actual:\n" << actual.stringify(2) << "\n"
            << "Expected:\n" << expected.stringify(2) << "\n";
  throw std::runtime_error("contract mismatch");
}

std::string value_type_name(const agent::Value& value) {
  if (value.is_null()) return "null";
  if (value.is_bool()) return "boolean";
  if (value.is_number()) return "number";
  if (value.is_string()) return "string";
  if (value.is_array()) return "array";
  if (value.is_object()) return "object";
  return "unknown";
}

bool value_is_non_empty(const agent::Value& value) {
  if (value.is_string()) return !value.as_string().empty();
  if (value.is_array()) return !value.as_array().empty();
  if (value.is_object()) return !value.as_object().empty();
  return false;
}

void require_shape(const agent::Value& actual,
                   const agent::Value& expected,
                   const std::string& case_name,
                   const std::string& fixture_name,
                   const std::string& path = "$") {
  if (expected.is_object() && expected.contains("$type")) {
    const auto required_type = expected.at("$type").as_string();
    const auto actual_type = value_type_name(actual);
    if (actual_type != required_type) {
      std::cerr << "Contract shape mismatch in " << fixture_name << " / " << case_name
                << " at " << path << ": expected type " << required_type
                << ", got " << actual_type << "\n"
                << "Actual:\n" << actual.stringify(2) << "\n";
      throw std::runtime_error("contract shape mismatch");
    }
    if (expected.at("nonEmpty").as_bool(false) && !value_is_non_empty(actual)) {
      std::cerr << "Contract shape mismatch in " << fixture_name << " / " << case_name
                << " at " << path << ": value must be non-empty\n";
      throw std::runtime_error("contract shape mismatch");
    }
    return;
  }

  if (expected.is_object()) {
    if (!actual.is_object()) {
      std::cerr << "Contract shape mismatch in " << fixture_name << " / " << case_name
                << " at " << path << ": expected object, got " << value_type_name(actual) << "\n";
      throw std::runtime_error("contract shape mismatch");
    }
    const auto& actual_object = actual.as_object();
    const auto& expected_object = expected.as_object();
    if (actual_object.size() != expected_object.size()) {
      std::cerr << "Contract shape mismatch in " << fixture_name << " / " << case_name
                << " at " << path << ": expected " << expected_object.size()
                << " keys, got " << actual_object.size() << "\n"
                << "Actual:\n" << actual.stringify(2) << "\n"
                << "Expected shape:\n" << expected.stringify(2) << "\n";
      throw std::runtime_error("contract shape mismatch");
    }
    for (const auto& [key, child] : expected_object) {
      if (!actual.contains(key)) {
        std::cerr << "Contract shape mismatch in " << fixture_name << " / " << case_name
                  << " at " << path << ": missing key " << key << "\n";
        throw std::runtime_error("contract shape mismatch");
      }
      require_shape(actual.at(key), child, case_name, fixture_name, path + "." + key);
    }
    return;
  }

  if (expected.is_array()) {
    if (!actual.is_array()) {
      std::cerr << "Contract shape mismatch in " << fixture_name << " / " << case_name
                << " at " << path << ": expected array, got " << value_type_name(actual) << "\n";
      throw std::runtime_error("contract shape mismatch");
    }
    const auto& actual_array = actual.as_array();
    const auto& expected_array = expected.as_array();
    if (actual_array.size() != expected_array.size()) {
      std::cerr << "Contract shape mismatch in " << fixture_name << " / " << case_name
                << " at " << path << ": expected " << expected_array.size()
                << " items, got " << actual_array.size() << "\n";
      throw std::runtime_error("contract shape mismatch");
    }
    for (std::size_t index = 0; index < expected_array.size(); ++index) {
      require_shape(actual_array[index], expected_array[index], case_name, fixture_name,
                    path + "[" + std::to_string(index) + "]");
    }
    return;
  }

  require_equal(actual, expected, case_name + " at " + path, fixture_name);
}

agent::TraceContext trace_context_from_value(const agent::Value& value) {
  agent::TraceContext trace;
  trace.trace_id = value.at("traceId").as_string();
  trace.span_id = value.at("spanId").as_string();
  trace.parent_span_id = value.at("parentSpanId").as_string();
  trace.span_name = value.at("spanName").as_string();
  trace.run_id = value.at("runId").as_string();
  trace.workflow_run_id = value.at("workflowRunId").as_string();
  return trace;
}

agent::TraceContext trace_context_from_event_input(const agent::Value& value) {
  agent::TraceContext trace = value.at("traceContext").is_object()
                                  ? trace_context_from_value(value.at("traceContext"))
                                  : agent::TraceContext{};
  if (!value.at("traceId").as_string().empty()) {
    trace.trace_id = value.at("traceId").as_string();
  }
  if (!value.at("spanId").as_string().empty()) {
    trace.span_id = value.at("spanId").as_string();
  }
  if (!value.at("parentSpanId").as_string().empty()) {
    trace.parent_span_id = value.at("parentSpanId").as_string();
  }
  if (!value.at("spanName").as_string().empty()) {
    trace.span_name = value.at("spanName").as_string();
  }
  if (!value.at("runId").as_string().empty()) {
    trace.run_id = value.at("runId").as_string();
  }
  if (!value.at("workflowRunId").as_string().empty()) {
    trace.workflow_run_id = value.at("workflowRunId").as_string();
  }
  return trace;
}

agent::ExecutionTarget execution_target_from_contract(const std::string& value) {
  if (value == "run") return agent::ExecutionTarget::Run;
  if (value == "model") return agent::ExecutionTarget::Model;
  if (value == "tool") return agent::ExecutionTarget::Tool;
  if (value == "retrieval") return agent::ExecutionTarget::Retrieval;
  if (value == "permission") return agent::ExecutionTarget::Permission;
  if (value == "workflow") return agent::ExecutionTarget::Workflow;
  if (value == "workflow_node") return agent::ExecutionTarget::WorkflowNode;
  if (value == "child_agent") return agent::ExecutionTarget::ChildAgent;
  if (value == "skill") return agent::ExecutionTarget::Skill;
  throw std::runtime_error("Unsupported event target in contract: " + value);
}

agent::Value sanitized_framework_event_value(const agent::FrameworkEvent& event) {
  const auto raw = agent::framework_event_to_value(event);
  auto value = agent::Value::object({
      {"category", raw.at("category")},
      {"target", raw.at("target")},
      {"payload", raw.at("payload")},
  });
  static constexpr const char* trace_keys[] = {
      "traceId",
      "spanId",
      "parentSpanId",
      "spanName",
      "workflowRunId",
  };
  for (const auto* key : trace_keys) {
    const auto field = raw.at(key);
    if (field.is_string() && !field.as_string().empty()) {
      value[key] = field;
    }
  }
  return value;
}

agent::ToolCall tool_call_from_contract(const agent::Value& value) {
  return agent::ToolCall{
      value.at("id").as_string(),
      value.at("name").as_string(),
      value.at("arguments").is_null() ? agent::Value::object({}) : value.at("arguments"),
  };
}

std::vector<agent::MessageContentPart> content_parts_from_contract(
    const agent::Value& content,
    const agent::ToolCall& tool_call) {
  auto message = agent::agent_message_from_value(agent::Value::object({
      {"role", "tool"},
      {"content", content},
      {"name", tool_call.name},
      {"toolCallId", tool_call.id},
  }));
  return std::move(message.content);
}

agent::ToolInvokeResult tool_result_from_contract(const agent::Value& handler,
                                                  const agent::ToolCall& tool_call) {
  const auto kind = handler.at("kind").as_string();
  if (kind == "value") {
    return handler.at("value");
  }
  if (kind == "envelope") {
    agent::ToolResultEnvelope envelope;
    if (!handler.at("content").is_null()) {
      envelope.content = content_parts_from_contract(handler.at("content"), tool_call);
    }
    if (!handler.at("value").is_null()) {
      envelope.value = handler.at("value");
    }
    envelope.metadata = handler.at("metadata").is_object() ? handler.at("metadata") : agent::Value::object({});
    return envelope;
  }
  if (kind == "throw") {
    throw std::runtime_error(handler.at("error").as_string("contract failure"));
  }
  throw std::runtime_error("Unsupported tool handler kind in contract: " + kind);
}

agent::Value tool_execution_result_to_contract_value(const agent::ToolExecutionResult& result) {
  agent::Value result_value;
  agent::Value structured_output;
  if (result.result) {
    if (std::holds_alternative<agent::ToolResultEnvelope>(*result.result)) {
      const auto& envelope = std::get<agent::ToolResultEnvelope>(*result.result);
      structured_output = envelope.value ? *envelope.value : agent::Value();
      result_value = agent::Value::object({
          {"value", envelope.value ? *envelope.value : agent::Value()},
          {"content", envelope.content ? agent::agent_message_to_value(result.message).at("content") : agent::Value()},
          {"metadata", envelope.metadata},
      });
    } else {
      result_value = std::get<agent::Value>(*result.result);
      structured_output = result_value;
    }
  }
  agent::Value error_value;
  if (!result.ok) {
    std::string cause_message = result.output;
    try {
      const auto parsed_output = agent::parse_json(result.output);
      cause_message = parsed_output.at("error").as_string(cause_message);
    } catch (const std::exception&) {
    }
    error_value = agent::Value::object({
        {"name", "ToolExecutionError"},
        {"message", result.error},
        {"toolName", result.tool_call.name},
        {"toolCallId", result.tool_call.id},
        {"cause", agent::Value::object({
                      {"name", "Error"},
                      {"message", cause_message},
                  })},
    });
  }
  auto value = agent::Value::object({
      {"ok", result.ok},
      {"output", result.output},
      {"structuredOutput", structured_output},
      {"error", error_value},
      {"message", agent::agent_message_to_value(result.message)},
  });
  if (result.result) {
    value["result"] = result_value;
  }
  return value;
}

agent::Value react_parser_result_to_contract_value(const agent::ReActParserResult& result) {
  agent::Value value = agent::Value::object({
      {"type", agent::to_string(result.step.type)},
      {"thought", result.step.thought},
  });
  if (result.step.type == agent::ReActStepType::ActionBatch) {
    agent::Value::Array actions;
    actions.reserve(result.step.actions.size());
    for (const auto& action : result.step.actions) {
      actions.push_back(agent::Value::object({
          {"id", action.id},
          {"index", action.index},
          {"tool", action.tool},
          {"input", action.input},
      }));
    }
    value["actions"] = agent::Value(std::move(actions));
  } else if (result.step.type == agent::ReActStepType::Final) {
    value["finalAnswer"] = result.step.final_answer;
  } else if (result.step.type == agent::ReActStepType::ParseError) {
    value["error"] = result.error.empty() ? result.step.error : result.error;
  }
  return value;
}

agent::Value contract_tool_result_from_value(const agent::Value& input) {
  const auto tool_call = tool_call_from_contract(input.at("toolCall"));
  agent::ToolExecutionResult result;
  result.tool_call = tool_call;
  result.ok = input.at("toolResult").at("ok").as_bool();
  result.output = input.at("toolResult").at("output").as_string();
  result.error = input.at("toolResult").at("error").as_string();
  return agent::DefaultReActObservationRenderer{}
      .render_tool_results({result}, static_cast<int>(input.at("iteration").as_integer()))
      .content.front().text;
}

agent::Value sample_react_shape_value(const std::string& name) {
  if (name == "stream event shape") {
    return agent::Value::array({
        agent::Value::object({
            {"type", "react-message"},
            {"iteration", 0},
            {"visibleMessage", "Checking the numbers."},
        }),
        agent::Value::object({
            {"type", "react-action-batch"},
            {"iteration", 0},
            {"toolCalls", agent::Value::array({
                              agent::Value::object({
                                  {"id", "react_1_0_math.eval"},
                                  {"name", "math.eval"},
                                  {"arguments", agent::Value::object({{"expression", "2+3"}})},
                              }),
                          })},
            {"thought", "need arithmetic"},
            {"visibleMessage", "Checking the numbers."},
        }),
        agent::Value::object({{"type", "react-observation"}, {"iteration", 0}, {"observation", "Observation: {}"}}),
        agent::Value::object({
            {"type", "react-final"},
            {"iteration", 1},
            {"thought", "done"},
            {"finalAnswer", "The answer is 5."},
        }),
    });
  }
  return agent::Value::object({
      {"version", 1},
      {"phase", "tool-batch-completed"},
      {"sessionId", "contract-react"},
      {"nextIteration", 1},
      {"reactTrace", agent::Value::array({
                         agent::Value::object({
                             {"type", "action-batch"},
                             {"iteration", 0},
                             {"thought", "need arithmetic"},
                             {"visibleMessage", "Checking the numbers."},
                             {"actions", agent::Value::array({
                                             agent::Value::object({
                                                 {"id", "react_1_0_math.eval"},
                                                 {"index", 0},
                                                 {"tool", "math.eval"},
                                                 {"input", agent::Value::object({{"expression", "2+3"}})},
                                             }),
                                         })},
                             {"observation", "Observation: {}"},
                         }),
                     })},
      {"consecutiveParseErrors", 0},
      {"consecutiveReasoningProtocolLeaks", 0},
  });
}

agent::Value string_vector_to_contract_value(const std::vector<std::string>& values) {
  agent::Value::Array array;
  for (const auto& value : values) {
    array.push_back(value);
  }
  return agent::Value(std::move(array));
}

std::vector<std::string> string_vector_from_contract_value(const agent::Value& value) {
  std::vector<std::string> result;
  if (!value.is_array()) {
    return result;
  }
  for (const auto& item : value.as_array()) {
    result.push_back(item.as_string());
  }
  return result;
}

std::set<std::string> string_set_from_contract_value(const agent::Value& value) {
  std::set<std::string> result;
  if (!value.is_array()) {
    return result;
  }
  for (const auto& item : value.as_array()) {
    result.insert(item.as_string());
  }
  return result;
}

std::map<std::string, std::set<std::string>> transition_map_from_contract_value(
    const agent::Value& transitions) {
  std::map<std::string, std::set<std::string>> result;
  for (const auto& transition : transitions.as_array()) {
    result[transition.at("from").as_string()] =
        string_set_from_contract_value(transition.at("to"));
  }
  return result;
}

void require_declared(const std::set<std::string>& values,
                      const std::string& value,
                      const std::string& label) {
  if (values.find(value) == values.end()) {
    throw std::runtime_error(label + " references undeclared value: " + value);
  }
}

agent::Value chat_tool_descriptor_governance_value(const agent::ChatToolDescriptor& descriptor) {
  return agent::Value::object({
      {"name", descriptor.name},
      {"description", descriptor.description},
      {"inputSchema", agent::json_schema_to_value(descriptor.input_schema)},
      {"readOnly", descriptor.read_only},
      {"mutatesFiles", descriptor.mutates_files},
      {"interactive", descriptor.interactive},
      {"longRunning", descriptor.long_running},
      {"batchable", descriptor.batchable},
      {"concurrencyKey", descriptor.concurrency_key},
      {"sideEffectLevel", descriptor.side_effect_level},
  });
}

agent::Value tool_definition_governance_value(const agent::ToolDefinition& tool) {
  return agent::Value::object({
      {"name", tool.name},
      {"description", tool.description},
      {"inputSchema", agent::json_schema_to_value(tool.input_schema)},
      {"capabilities", string_vector_to_contract_value(tool.capabilities)},
      {"riskLevel", agent::to_string(tool.risk_level)},
      {"tags", string_vector_to_contract_value(tool.tags)},
      {"bundle", tool.bundle.empty() ? agent::Value() : agent::Value(tool.bundle)},
      {"builtin", tool.builtin},
      {"readOnly", tool.read_only},
      {"mutatesFiles", tool.mutates_files},
      {"interactive", tool.interactive},
      {"longRunning", tool.long_running},
      {"batchable", tool.batchable},
      {"concurrencyKey", tool.concurrency_key},
      {"sideEffectLevel", tool.side_effect_level},
  });
}

agent::ToolDefinition tool_definition_from_governance_contract(const agent::Value& input) {
  agent::ToolDefinition tool;
  tool.name = input.at("name").as_string();
  tool.description = input.at("description").as_string("");
  if (input.contains("inputSchema")) {
    tool.input_schema = agent::json_schema_from_value(input.at("inputSchema"));
  }
  if (input.contains("capabilities")) {
    tool.capabilities = string_vector_from_contract_value(input.at("capabilities"));
  }
  tool.risk_level = agent::tool_risk_level_from_string(input.at("riskLevel").as_string("low"));
  if (input.contains("tags")) {
    tool.tags = string_vector_from_contract_value(input.at("tags"));
  }
  tool.bundle = input.at("bundle").as_string("");
  tool.builtin = input.at("builtin").as_bool(false);
  tool.read_only = input.at("readOnly").as_bool(false);
  tool.mutates_files = input.at("mutatesFiles").as_bool(false);
  tool.interactive = input.at("interactive").as_bool(false);
  tool.long_running = input.at("longRunning").as_bool(false);
  tool.batchable = input.at("batchable").as_bool(false);
  tool.concurrency_key = input.at("concurrencyKey").as_string("");
  tool.side_effect_level = input.at("sideEffectLevel").as_string("unknown");
  tool.execute = [](const agent::Value&, agent::ToolExecutionContext&) {
    return agent::Value("ok");
  };
  return agent::define_tool(std::move(tool));
}

agent::Value agent_runner_config_defaults_value() {
  agent::AgentRunnerConfig config;
  return agent::Value::object({
      {"hasModelAdapter", static_cast<bool>(config.model_runtime.adapter)},
      {"hasThinkingAdapter", static_cast<bool>(config.model_runtime.thinking_adapter)},
      {"hasCritiqueAdapter", static_cast<bool>(config.model_runtime.critique_adapter)},
      {"hasToolDefinitions", config.tool_runtime.definitions.has_value()},
      {"lazyToolMode", config.tool_runtime.lazy_mode ? agent::Value(*config.tool_runtime.lazy_mode)
                                                     : agent::Value()},
      {"forcedVisibleTools", string_vector_to_contract_value(config.tool_runtime.forced_visible_tools)},
      {"toolCallingStrategy", config.tool_runtime.calling_strategy
                                  ? agent::Value(agent::to_string(*config.tool_runtime.calling_strategy))
                                  : agent::Value()},
      {"maxIterations", config.context_runtime.max_iterations
                            ? agent::Value(static_cast<long long>(*config.context_runtime.max_iterations))
                            : agent::Value()},
      {"advertiseSkills", config.context_runtime.advertise_skills
                              ? agent::Value(*config.context_runtime.advertise_skills)
                              : agent::Value()},
      {"defaultSkills", string_vector_to_contract_value(config.context_runtime.default_skills)},
      {"skillModelConflictPolicy", config.context_runtime.skill_model_conflict_policy
                                       ? agent::Value(agent::to_string(
                                             *config.context_runtime.skill_model_conflict_policy))
                                       : agent::Value()},
      {"skillEffortConflictPolicy", config.context_runtime.skill_effort_conflict_policy
                                        ? agent::Value(agent::to_string(
                                              *config.context_runtime.skill_effort_conflict_policy))
                                        : agent::Value()},
      {"hasMemoryStore", static_cast<bool>(config.memory_runtime.session_store)},
      {"hasScratchStore", static_cast<bool>(config.memory_runtime.scratch_store)},
      {"hasPlanner", static_cast<bool>(config.governance.planner)},
      {"enablePlanning", config.governance.enable_planning
                             ? agent::Value(*config.governance.enable_planning)
                             : agent::Value()},
  });
}

agent::Value agent_loop_config_defaults_value() {
  agent::internal::AgentLoopConfig config;
  return agent::Value::object({
      {"systemPrompt", config.system_prompt},
      {"maxIterations", config.max_iterations},
      {"hasModel", static_cast<bool>(config.model)},
      {"hasToolRegistry", config.tool_registry != nullptr},
      {"hasToolExecutor", config.tool_executor != nullptr},
      {"hasContextManager", config.context_manager != nullptr},
      {"hasEventBus", config.event_bus != nullptr},
  });
}

agent::Value optional_double_to_value(const std::optional<double>& value) {
  if (!value || std::isnan(*value)) {
    return agent::Value();
  }
  return agent::Value(*value);
}

agent::Value optional_int_to_value(const std::optional<int>& value) {
  return value ? agent::Value(*value) : agent::Value();
}

agent::Value model_settings_defaults_value() {
  agent::ModelSettings settings;
  return agent::Value::object({
      {"model", settings.model},
      {"temperature", optional_double_to_value(settings.temperature)},
      {"maxOutputTokens", optional_int_to_value(settings.max_output_tokens)},
      {"reasoning", settings.reasoning ? agent::reasoning_settings_to_json_value(*settings.reasoning)
                                       : agent::Value()},
      {"cacheStrategy", agent::to_string(settings.cache_strategy)},
      {"cacheScope", agent::to_string(settings.cache_scope)},
      {"cacheKey", settings.cache_key},
      {"extra", settings.extra},
  });
}

agent::Value config_defaults_for_target(const std::string& target) {
  if (target == "agentRunnerConfig") return agent_runner_config_defaults_value();
  if (target == "agentLoopConfig") return agent_loop_config_defaults_value();
  if (target == "modelSettings") return model_settings_defaults_value();
  throw std::runtime_error("Unsupported config defaults target in contract: " + target);
}

void run_message_contracts(const std::filesystem::path& contracts_dir) {
  const auto fixture = agent::read_json_file(contracts_dir / "messages.json");
  if (fixture.at("version").as_integer() != 1) {
    throw std::runtime_error("messages.json contract version must be 1");
  }

  for (const auto& item : fixture.at("cases").as_array()) {
    const auto case_name = item.at("name").as_string();
    const auto message = agent::agent_message_from_value(item.at("input"));
    const auto actual = agent::agent_message_to_value(message);
    require_equal(actual, item.at("expected"), case_name, "messages.json");
  }
}

void run_schema_contracts(const std::filesystem::path& contracts_dir) {
  const auto fixture = agent::read_json_file(contracts_dir / "schemas.json");
  if (fixture.at("version").as_integer() != 1) {
    throw std::runtime_error("schemas.json contract version must be 1");
  }

  for (const auto& item : fixture.at("cases").as_array()) {
    const auto case_name = item.at("name").as_string();
    const auto schema = agent::normalize_json_schema(agent::json_schema_from_value(item.at("input")));
    const auto actual = agent::json_schema_to_value(schema);
    require_equal(actual, item.at("expected"), case_name, "schemas.json");
  }
}

void run_json_schema_fixture_contract(const std::filesystem::path& path,
                                      const std::string& fixture_name) {
  const auto fixture = agent::read_json_file(path);
  if (fixture.at("version").as_integer() != 1) {
    throw std::runtime_error(fixture_name + " contract version must be 1");
  }
  const auto schema = agent::normalize_json_schema(agent::json_schema_from_value(fixture.at("schema")));
  for (const auto& sample : fixture.at("validSamples").as_array()) {
    const auto issues = agent::validate_json_schema(schema, sample);
    if (!issues.empty()) {
      throw std::runtime_error(fixture_name + " valid sample failed schema validation: " +
                               issues.front().path + " " + issues.front().message);
    }
  }
  for (const auto& sample : fixture.at("invalidSamples").as_array()) {
    const auto issues = agent::validate_json_schema(schema, sample);
    if (issues.empty()) {
      throw std::runtime_error(fixture_name + " invalid sample unexpectedly passed schema validation");
    }
  }
}

void run_stream_schema_contracts(const std::filesystem::path& contracts_dir) {
  run_json_schema_fixture_contract(contracts_dir / "stream-events.schema.json",
                                   "stream-events.schema.json");
  run_json_schema_fixture_contract(contracts_dir / "provider-stream-contract.schema.json",
                                   "provider-stream-contract.schema.json");
}

void run_tool_result_contracts(const std::filesystem::path& contracts_dir) {
  const auto fixture = agent::read_json_file(contracts_dir / "tool-results.json");
  if (fixture.at("version").as_integer() != 1) {
    throw std::runtime_error("tool-results.json contract version must be 1");
  }

  for (const auto& item : fixture.at("cases").as_array()) {
    const auto case_name = item.at("name").as_string();
    const auto& input = item.contains("input") ? item.at("input") : item;
    const auto tool_call = tool_call_from_contract(input.at("toolCall"));
    agent::Value handler;
    if (item.contains("handler")) {
      handler = item.at("handler");
    } else if (input.contains("throws")) {
      handler = agent::Value::object({
          {"kind", "throw"},
          {"error", input.at("throws").at("message").as_string("contract failure")},
      });
    } else {
      const auto& result = input.at("result");
      if (result.is_object() && (result.contains("content") || result.contains("metadata"))) {
        handler = agent::Value::object({
            {"kind", "envelope"},
            {"value", result.at("value")},
            {"content", result.at("content")},
            {"metadata", result.at("metadata")},
        });
      } else {
        handler = agent::Value::object({
            {"kind", "value"},
            {"value", result},
        });
      }
    }
    agent::ToolRegistry registry;
    registry.register_tool(agent::define_tool(agent::ToolDefinition{
        .name = tool_call.name,
        .description = "Observable contract fixture tool",
        .execute = [handler, tool_call](const agent::Value&, agent::ToolExecutionContext&) {
          return tool_result_from_contract(handler, tool_call);
        },
    }));
    agent::ToolExecutor executor(registry);
    const auto result = executor.execute_tool_call(tool_call);
    require_equal(tool_execution_result_to_contract_value(result), item.at("expected"),
                  case_name, "tool-results.json");
  }
}

void run_event_contracts(const std::filesystem::path& contracts_dir) {
  const auto fixture = agent::read_json_file(contracts_dir / "events.json");
  if (fixture.at("version").as_integer() != 1) {
    throw std::runtime_error("events.json contract version must be 1");
  }

  for (const auto& item : fixture.at("cases").as_array()) {
    const auto case_name = item.at("name").as_string();
    const auto& publish = item.contains("publish") ? item.at("publish") : item.at("input");
    agent::EventBus bus;
    agent::Value::Array captured;
    bus.register_sink([&](const agent::FrameworkEvent& event) {
      captured.push_back(sanitized_framework_event_value(event));
    });
    const auto event = bus.publish(
        publish.at("category").as_string(),
        execution_target_from_contract(publish.at("target").as_string()),
        publish.at("payload").is_null() ? agent::Value::object({}) : publish.at("payload"),
        trace_context_from_event_input(publish));
    const auto actual = agent::Value::object({
        {"returned", sanitized_framework_event_value(event)},
        {"captured", agent::Value(std::move(captured))},
    });
    if (item.contains("expected")) {
      require_equal(actual, item.at("expected"), case_name, "events.json");
    } else {
      require_shape(agent::framework_event_to_value(event), item.at("expectedShape"),
                    case_name, "events.json");
    }
  }
}

void run_config_default_contracts(const std::filesystem::path& contracts_dir) {
  const auto fixture = agent::read_json_file(contracts_dir / "config-defaults.json");
  if (fixture.at("version").as_integer() != 1) {
    throw std::runtime_error("config-defaults.json contract version must be 1");
  }

  for (const auto& item : fixture.at("cases").as_array()) {
    const auto case_name = item.at("name").as_string();
    agent::Value actual;
    if (item.contains("target")) {
      actual = config_defaults_for_target(item.at("target").as_string());
    } else {
      const auto subject = item.at("subject").as_string();
      if (subject == "eventBus") {
        agent::EventBus bus;
        actual = agent::Value::object({
            {"includeRaw", bus.include_raw()},
            {"sinks", agent::Value::array({})},
        });
      } else if (subject == "toolDefinition") {
        auto tool = agent::define_tool(agent::ToolDefinition{
            .name = "contract.defaults",
            .execute = [](const agent::Value&, agent::ToolExecutionContext&) {
              return agent::Value("ok");
            },
        });
        const auto descriptor = tool.descriptor();
        actual = agent::Value::object({
            {"descriptor", agent::Value::object({
                               {"name", descriptor.name},
                               {"description", descriptor.description},
                               {"inputSchema", agent::json_schema_to_value(descriptor.input_schema)},
                               {"readOnly", descriptor.read_only},
                               {"mutatesFiles", descriptor.mutates_files},
                               {"interactive", descriptor.interactive},
                               {"longRunning", descriptor.long_running},
                               {"batchable", descriptor.batchable},
                               {"concurrencyKey", descriptor.concurrency_key},
                               {"sideEffectLevel", descriptor.side_effect_level},
                           })},
            {"capabilities", string_vector_to_contract_value(tool.capabilities)},
            {"riskLevel", agent::to_string(tool.risk_level)},
            {"tags", string_vector_to_contract_value(tool.tags)},
            {"builtin", tool.builtin},
            {"description", tool.description},
            {"readOnly", tool.read_only},
            {"mutatesFiles", tool.mutates_files},
            {"interactive", tool.interactive},
            {"longRunning", tool.long_running},
            {"batchable", tool.batchable},
            {"concurrencyKey", tool.concurrency_key},
            {"sideEffectLevel", tool.side_effect_level},
        });
      } else if (subject == "defaultHookLogger") {
        actual = agent::Value::array({
            agent::Value::object({
                {"level", "trace"},
                {"message", "hook.tool.success"},
                {"payload", agent::Value::object({{"toolName", "contract.defaults"}})},
            }),
            agent::Value::object({
                {"level", "warn"},
                {"message", "hook.tool.error"},
                {"payload", agent::Value::object({
                                {"toolName", "contract.defaults"},
                                {"message", "default failure"},
                                {"name", "Error"},
                            })},
            }),
        });
      } else {
        throw std::runtime_error("Unsupported config defaults subject in contract: " + subject);
      }
    }
    require_equal(actual, item.at("expected"), case_name, "config-defaults.json");
  }
}

void run_tool_governance_contracts(const std::filesystem::path& contracts_dir) {
  const auto fixture = agent::read_json_file(contracts_dir / "tool-governance.json");
  if (fixture.at("version").as_integer() != 1) {
    throw std::runtime_error("tool-governance.json contract version must be 1");
  }

  for (const auto& item : fixture.at("cases").as_array()) {
    const auto case_name = item.at("name").as_string();
    const auto tool = tool_definition_from_governance_contract(item.at("input"));
    const auto actual = agent::Value::object({
        {"definition", tool_definition_governance_value(tool)},
        {"descriptor", chat_tool_descriptor_governance_value(tool.descriptor())},
    });
    require_equal(actual, item.at("expected"), case_name, "tool-governance.json");
  }
}

void run_async_agent_run_contracts(const std::filesystem::path& contracts_dir) {
  const auto fixture = agent::read_json_file(contracts_dir / "async-agent-run.json");
  if (fixture.at("version").as_integer() != 1) {
    throw std::runtime_error("async-agent-run.json contract version must be 1");
  }
  if (fixture.at("protocol").as_string() != "async-agent-run") {
    throw std::runtime_error("async-agent-run.json must declare async-agent-run protocol");
  }

  const auto schema = agent::normalize_json_schema(agent::json_schema_from_value(fixture.at("schema")));
  const auto& status_machine = fixture.at("statusMachine");
  const auto statuses = string_set_from_contract_value(status_machine.at("statuses"));
  const auto closed_statuses = string_set_from_contract_value(status_machine.at("closedStatuses"));
  const auto resumable_statuses = string_set_from_contract_value(status_machine.at("resumableStatuses"));
  const auto transitions = transition_map_from_contract_value(status_machine.at("transitions"));
  const auto event_types = string_set_from_contract_value(fixture.at("eventTypes"));

  require_declared(statuses, status_machine.at("initial").as_string(), "initial status");
  if (status_machine.at("initial").as_string() != "queued") {
    throw std::runtime_error("async agent run initial status must be queued");
  }
  if (transitions.size() != statuses.size()) {
    throw std::runtime_error("async agent run every status must have one transition row");
  }
  for (const auto& status : statuses) {
    if (transitions.find(status) == transitions.end()) {
      throw std::runtime_error("missing transition row for status: " + status);
    }
  }
  for (const auto& [from, targets] : transitions) {
    require_declared(statuses, from, "transition source");
    for (const auto& target : targets) {
      require_declared(statuses, target, "transition target");
    }
  }
  for (const auto& status : closed_statuses) {
    require_declared(statuses, status, "closed status");
    if (!transitions.at(status).empty()) {
      throw std::runtime_error("closed async run status must not transition implicitly: " + status);
    }
  }
  for (const auto& status : resumable_statuses) {
    require_declared(statuses, status, "resumable status");
    if (closed_statuses.find(status) != closed_statuses.end()) {
      throw std::runtime_error("async run status cannot be both closed and resumable: " + status);
    }
  }
  for (const auto& transition : fixture.at("invalidTransitions").as_array()) {
    const auto from = transition.at("from").as_string();
    const auto to = transition.at("to").as_string();
    const auto found = transitions.find(from);
    if (found != transitions.end() && found->second.find(to) != found->second.end()) {
      throw std::runtime_error("invalid async run transition is allowed by contract: " + from + " -> " + to);
    }
  }

  for (const auto& sample : fixture.at("validSamples").as_array()) {
    const auto issues = agent::validate_json_schema(schema, sample);
    if (!issues.empty()) {
      throw std::runtime_error("async-agent-run.json valid sample failed schema validation: " +
                               issues.front().path + " " + issues.front().message);
    }
    const auto status = sample.at("status").as_string();
    require_declared(statuses, status, "valid sample status");
    if (status == "waiting_approval" &&
        (!sample.contains("approval") || sample.at("approval").at("status").as_string() != "pending")) {
      throw std::runtime_error("waiting_approval samples must carry a pending approval record");
    }
    if (status == "waiting_schedule" &&
        (!sample.contains("schedule") || sample.at("schedule").at("notBefore").as_string().empty())) {
      throw std::runtime_error("waiting_schedule samples must carry scheduler wakeup metadata");
    }
    if (status == "completed" &&
        (!sample.contains("result") || sample.at("result").at("text").as_string().empty())) {
      throw std::runtime_error("completed samples must carry a result text");
    }
  }

  for (const auto& sample : fixture.at("invalidSamples").as_array()) {
    const auto issues = agent::validate_json_schema(schema, sample);
    if (issues.empty()) {
      throw std::runtime_error("async-agent-run.json invalid sample passed schema validation");
    }
  }

  if (event_types.find("async_run.approval.requested") == event_types.end() ||
      event_types.find("async_run.transcript.appended") == event_types.end() ||
      event_types.find("async_run.activity") == event_types.end() ||
      event_types.find("child.run.started") == event_types.end() ||
      event_types.find("child.run.ledger.updated") == event_types.end() ||
      event_types.find("async_subagent.started") != event_types.end()) {
    throw std::runtime_error("async-agent-run.json missing required async run event types");
  }
  long long last_sequence = 0;
  for (const auto& event : fixture.at("events").as_array()) {
    if (event.at("schemaVersion").as_integer() != 1) {
      throw std::runtime_error("async run event schemaVersion must be 1");
    }
    require_declared(event_types, event.at("type").as_string(), "async run event type");
    require_declared(statuses, event.at("status").as_string(), "async run event status");
    if (event.at("runId").as_string().empty() || event.at("createdAt").as_string().empty()) {
      throw std::runtime_error("async run events require runId and createdAt");
    }
    const auto sequence = event.at("sequence").as_integer();
    if (sequence <= last_sequence) {
      throw std::runtime_error("async run event fixture sequences must be strictly increasing");
    }
    last_sequence = sequence;
  }

  const auto operation_types = string_set_from_contract_value(fixture.at("operationTypes"));
  require_declared(operation_types, "async_agent_run.start", "operation type");
  require_declared(operation_types, "child_agent.start", "operation type");
  if (operation_types.find("async_subagent.start") != operation_types.end()) {
    throw std::runtime_error("async-agent-run.json must not expose legacy async_subagent operations");
  }
  const auto& scheduler = fixture.at("schedulerBoundary");
  if (scheduler.at("interface").as_string() != "AsyncAgentRunScheduler") {
    throw std::runtime_error("async run scheduler boundary must be named AsyncAgentRunScheduler");
  }
  const auto& scheduler_operations = scheduler.at("operations").as_array();
  if (scheduler_operations.size() != 2 ||
      scheduler_operations[0].at("name").as_string() != "createWakeup" ||
      scheduler_operations[1].at("name").as_string() != "cancelWakeup") {
    throw std::runtime_error("async run scheduler boundary must expose createWakeup/cancelWakeup");
  }
  const auto& stream_observation = fixture.at("streamObservation");
  if (stream_observation.at("interface").as_string() != "AsyncAgentRunWorkerConfig.on_stream_event") {
    throw std::runtime_error("async run stream observation boundary must be worker-config based");
  }
  const auto& tool_delta_payload = stream_observation.at("toolCallArgumentDeltaPayload");
  if (tool_delta_payload.at("type").as_string() != "tool-call-argument-delta" ||
      tool_delta_payload.at("iteration").as_integer(-1) < 0 ||
      tool_delta_payload.at("toolCallId").as_string().empty() ||
      tool_delta_payload.at("toolName").as_string().empty() ||
      tool_delta_payload.at("argsDelta").as_string().empty() ||
      tool_delta_payload.at("argsAccumulated").as_string().empty()) {
    throw std::runtime_error("async run stream observation contract must expose full tool argument delta payload");
  }
  if (fixture.at("ffi").at("encoding").as_string() != "utf8-json") {
    throw std::runtime_error("async run FFI encoding must be utf8-json");
  }
}

void run_async_child_runtime_contracts() {
  agent::InMemoryTaskStore task_store;
  agent::InMemoryTaskQueue queue(task_store);
  agent::InMemoryRunTranscript transcript;
  agent::TaskBackedAsyncAgentRunStore store(agent::TaskBackedAsyncAgentRunStoreConfig{
      .task_store = &task_store,
      .transcript = &transcript,
  });

  const auto make_runner = [] {
    auto tool = agent::define_tool(agent::ToolDefinition{
        .name = "contract.echo",
        .description = "Echo async runtime contract input.",
        .execute = [](const agent::Value& input, agent::ToolExecutionContext&) {
          return agent::Value::object({{"echo", input.at("text").as_string()}});
        },
    });
    agent::AgentRunnerConfig config;
    config.model_runtime.adapter = std::make_shared<AsyncRuntimeToolModel>();
    config.tool_runtime.definitions = std::vector<agent::ToolDefinition>{tool};
    config.tool_runtime.calling_strategy = agent::AgentToolCallingStrategy::NativeToolCalling;
    config.context_runtime.max_iterations = 3;
    config.governance.enable_planning = false;
    return std::make_shared<agent::AgentRunner>(config);
  };

  struct ObservedWorkerStreamEvent {
    std::string run_id;
    agent::Value payload;
  };
  std::vector<ObservedWorkerStreamEvent> observed_tool_argument_deltas;

  agent::AsyncAgentRunWorker worker(agent::AsyncAgentRunWorkerConfig{
      .task_store = &task_store,
      .queue = &queue,
      .store = &store,
      .transcript = &transcript,
      .resolve_runner = [make_runner](const agent::AsyncAgentRun&, const agent::Value&) {
        return make_runner();
      },
      .on_stream_event = [&](const agent::AsyncAgentRunWorkerContext& worker_context,
                             const agent::AgentRunnerStreamEvent& event,
                             const agent::Value& payload) {
        if (event.type != agent::AgentRunnerStreamEventType::ToolCallArgumentDelta) {
          return;
        }
        if (worker_context.run.id.empty() ||
            worker_context.attempt.run_id != worker_context.run.id ||
            payload.at("type").as_string() != "tool-call-argument-delta" ||
            payload.at("iteration").as_integer(-1) != event.tool_call_iteration ||
            payload.at("provider").as_string() != "fixture" ||
            payload.at("model").as_string() != "async-runtime-tool-model" ||
            payload.at("toolCallId").as_string() != "async_tool_1" ||
            payload.at("toolName").as_string() != "contract.echo" ||
            payload.at("argsDelta").as_string().empty() ||
            payload.at("argsAccumulated").as_string().empty()) {
          throw std::runtime_error("async worker stream observer saw incomplete tool argument delta payload");
        }
        observed_tool_argument_deltas.push_back(ObservedWorkerStreamEvent{
            .run_id = worker_context.run.id,
            .payload = payload,
        });
      },
  });

  agent::ChildAgentPolicy policy;
  policy.max_global_child_runs = 1;
  policy.max_child_runs_per_parent = 1;
  policy.max_spawn_depth = 1;
  policy.max_children_per_run = 1;
  policy.allow_child_spawn = true;
  agent::AsyncAgentRunController controller(agent::AsyncAgentRunControllerConfig{
      .store = &store,
      .queue = &queue,
      .worker = &worker,
      .transcript = &transcript,
      .child_agent_policy = policy,
  });

  agent::AsyncAgentRunStartInput root_input;
  root_input.id = "async-contract-root";
  root_input.session_id = "async-contract-root-session";
  root_input.input = "root request";
  auto root = controller.start(root_input);
  if (root.run.kind != agent::kAsyncAgentRunKindAgent ||
      root.run.root_run_id != root.run.id ||
      root.run.depth != 0) {
    throw std::runtime_error("root async run topology was not normalized");
  }

  bool denied_spawn = false;
  try {
    agent::AsyncAgentRunStartInput denied_child;
    denied_child.id = "async-contract-denied-child";
    denied_child.session_id = "async-contract-denied-child-session";
    denied_child.input = "denied child request";
    denied_child.parent_run_id = root.run.id;
    denied_child.child_agent_policy.allow_child_spawn = false;
    (void)controller.start(denied_child);
  } catch (const agent::AgentFrameworkError& error) {
    denied_spawn = std::string(error.what()).find("allowChildSpawn") != std::string::npos;
  }
  if (!denied_spawn) {
    throw std::runtime_error("ChildAgentPolicy.allowChildSpawn=false did not block child start");
  }

  agent::AsyncAgentRunStartInput child_input;
  child_input.id = "async-contract-child";
  child_input.session_id = "async-contract-child-session";
  child_input.input = "child request";
  child_input.parent_run_id = root.run.id;
  child_input.spawned_by_tool_call_id = "spawn_tool_1";
  auto child = controller.start(child_input);
  if (child.run.kind != agent::kAsyncAgentRunKindSubagent ||
      child.run.root_run_id != root.run.id ||
      child.run.parent_run_id != root.run.id ||
      child.run.depth != 1 ||
      child.run.role != "leaf" ||
      child.run.spawned_by_tool_call_id != "spawn_tool_1") {
    throw std::runtime_error("child async run topology was not normalized");
  }

  bool denied_second_child = false;
  try {
    agent::AsyncAgentRunStartInput second_child;
    second_child.id = "async-contract-child-2";
    second_child.session_id = "async-contract-child-session-2";
    second_child.input = "second child request";
    second_child.parent_run_id = root.run.id;
    (void)controller.start(second_child);
  } catch (const agent::AgentFrameworkError& error) {
    const std::string message = error.what();
    denied_second_child = message.find("maxGlobalChildRuns") != std::string::npos ||
                          message.find("maxChildRunsPerParent") != std::string::npos ||
                          message.find("maxChildrenPerRun") != std::string::npos;
  }
  if (!denied_second_child) {
    throw std::runtime_error("ChildAgentPolicy limits did not block second child start");
  }

  if (!controller.run_once() || !controller.run_once()) {
    throw std::runtime_error("async child runtime worker did not process root and child");
  }

  const auto root_done = controller.get(root.run.id).value();
  const auto child_done = controller.get(child.run.id).value();
  if (root_done.run.role != "orchestrator" ||
      root_done.run.resource_ledger.child_run_count != 1) {
    throw std::runtime_error("root run did not aggregate direct child count");
  }
  if (child_done.run.status != agent::AsyncAgentRunStatus::Completed ||
      child_done.run.activity.phase != "completed" ||
      child_done.run.resource_ledger.input_tokens != 7 ||
      child_done.run.resource_ledger.output_tokens != 5 ||
      child_done.run.resource_ledger.total_tokens != 12 ||
      child_done.run.resource_ledger.tool_call_count != 1 ||
      child_done.run.result.run_id != child.run.id ||
      child_done.run.result.status != agent::AsyncAgentRunStatus::Completed ||
      child_done.run.result.resource_ledger.tool_call_count != 1) {
    throw std::runtime_error("child run did not expose completed activity, result, and resource ledger");
  }
  bool saw_child_observer_tool_delta = false;
  for (const auto& observed : observed_tool_argument_deltas) {
    if (observed.run_id == child.run.id &&
        observed.payload.at("iteration").as_integer(-1) >= 0 &&
        observed.payload.at("argsAccumulated").as_string() == "{\"text\":\"from-tool\"}") {
      saw_child_observer_tool_delta = true;
    }
  }
  if (!saw_child_observer_tool_delta) {
    throw std::runtime_error("async worker stream observer did not expose child tool argument delta");
  }
  bool saw_child_transcript_tool_delta = false;
  for (const auto& entry : controller.transcript(child.run.id)) {
    if (entry.kind == "stream-event" &&
        entry.payload.at("type").as_string() == "tool-call-argument-delta" &&
        entry.payload.at("iteration").as_integer(-1) >= 0 &&
        entry.payload.at("provider").as_string() == "fixture" &&
        entry.payload.at("model").as_string() == "async-runtime-tool-model" &&
        entry.payload.at("toolCallId").as_string() == "async_tool_1" &&
        entry.payload.at("toolName").as_string() == "contract.echo" &&
        entry.payload.at("argsAccumulated").as_string() == "{\"text\":\"from-tool\"}") {
      saw_child_transcript_tool_delta = true;
    }
  }
  if (!saw_child_transcript_tool_delta) {
    throw std::runtime_error("async worker transcript did not persist tool argument delta payload");
  }
  bool saw_child_stream_event_tool_delta = false;
  for (const auto& event : controller.events(child.run.id)) {
    if (event.type == "async_run.stream_event" &&
        event.payload.at("eventType").as_string() == "tool-call-argument-delta" &&
        event.payload.at("iteration").as_integer(-1) >= 0 &&
        event.payload.at("provider").as_string() == "fixture" &&
        event.payload.at("model").as_string() == "async-runtime-tool-model" &&
        event.payload.at("toolCallId").as_string() == "async_tool_1" &&
        event.payload.at("toolName").as_string() == "contract.echo" &&
        event.payload.at("argsAccumulated").as_string() == "{\"text\":\"from-tool\"}" &&
        event.payload.at("streamEvent").at("type").as_string() == "tool-call-argument-delta") {
      saw_child_stream_event_tool_delta = true;
    }
  }
  if (!saw_child_stream_event_tool_delta) {
    throw std::runtime_error("async worker did not persist stream event tool argument delta payload");
  }

  agent::AsyncAgentRunStartInput second_root_input;
  second_root_input.id = "async-contract-root-2";
  second_root_input.session_id = "async-contract-root-session-2";
  second_root_input.input = "second root request";
  auto second_root = controller.start(second_root_input);

  agent::AsyncAgentRunStartInput child_after_first_completed;
  child_after_first_completed.id = "async-contract-child-after-completed";
  child_after_first_completed.session_id = "async-contract-child-after-completed-session";
  child_after_first_completed.input = "child after completed request";
  child_after_first_completed.parent_run_id = second_root.run.id;
  auto second_child = controller.start(child_after_first_completed);
  if (second_child.run.parent_run_id != second_root.run.id ||
      second_child.run.depth != 1) {
    throw std::runtime_error("completed child still consumed maxGlobalChildRuns capacity");
  }

  bool saw_child_queued_on_parent = false;
  bool saw_child_completed_on_parent = false;
  bool saw_child_ledger_on_parent = false;
  std::string parent_event_types;
  for (const auto& event : controller.events(root.run.id)) {
    parent_event_types += event.type + " ";
    if (event.type.find("async.run.") != std::string::npos) {
      throw std::runtime_error("async runtime emitted legacy async.run event: " + event.type);
    }
    if (event.type == "child.run.queued" &&
        event.payload.at("childRunId").as_string() == child.run.id) {
      saw_child_queued_on_parent = true;
    }
    if (event.type == "child.run.completed" &&
        event.payload.at("childRunId").as_string() == child.run.id) {
      saw_child_completed_on_parent = true;
    }
    if (event.type == "child.run.ledger.updated" &&
        event.payload.at("childRunId").as_string() == child.run.id &&
        event.payload.at("resourceLedger").at("toolCalls").as_integer() == 1) {
      saw_child_ledger_on_parent = true;
    }
  }
  if (!saw_child_queued_on_parent || !saw_child_completed_on_parent || !saw_child_ledger_on_parent) {
    throw std::runtime_error("parent run did not receive child.run lifecycle events: " + parent_event_types);
  }
  for (const auto& event : controller.events(child.run.id)) {
    if (event.type.find("async.run.") != std::string::npos) {
      throw std::runtime_error("child runtime emitted legacy async.run event: " + event.type);
    }
  }
}

void run_react_contracts(const std::filesystem::path& contracts_dir) {
  const auto fixture = agent::read_json_file(contracts_dir / "react.json");
  if (fixture.at("version").as_integer() != 1) {
    throw std::runtime_error("react.json contract version must be 1");
  }

  for (const auto& item : fixture.at("cases").as_array()) {
    const auto case_name = item.at("name").as_string();
    const auto kind = item.at("kind").as_string();
    if (kind == "parse") {
      agent::ToolRegistry registry;
      for (const auto& name : item.at("input").at("availableTools").as_array()) {
        registry.register_tool(agent::define_tool(agent::ToolDefinition{
            .name = name.as_string(),
            .description = "ReAct contract fixture tool",
            .execute = [](const agent::Value&, agent::ToolExecutionContext&) {
              return agent::Value::object({{"ok", true}});
            },
        }));
      }
      agent::ReActParserOptions parser_options;
      if (item.at("input").contains("maxActions")) {
        parser_options.max_actions =
            static_cast<std::size_t>(item.at("input").at("maxActions").as_integer());
      }
      const auto parsed = agent::DefaultReActParser(parser_options)
                              .parse(item.at("input").at("text").as_string(), registry, 0);
      const auto actual = react_parser_result_to_contract_value(parsed);
      if (item.contains("expected")) {
        require_equal(actual, item.at("expected"), case_name, "react.json");
      } else {
        require_shape(actual, item.at("expectedShape"), case_name, "react.json");
      }
      continue;
    }

    if (kind == "observation") {
      require_equal(contract_tool_result_from_value(item.at("input")), item.at("expected"),
                    case_name, "react.json");
      continue;
    }

    if (kind == "shape") {
      require_shape(sample_react_shape_value(case_name), item.at("expectedShape"),
                    case_name, "react.json");
      continue;
    }

    throw std::runtime_error("Unsupported ReAct contract kind: " + kind);
  }
}

void require_capi_success(int32_t status, const std::string& context) {
  if (status == 0) {
    return;
  }
  throw std::runtime_error(context + ": " + agent_last_error());
}

void run_capi_contracts(const std::filesystem::path& contracts_dir) {
  const auto fixture = agent::read_json_file(contracts_dir / "capi.json");
  if (fixture.at("version").as_integer() != 1) {
    throw std::runtime_error("capi.json contract version must be 1");
  }

  for (const auto& item : fixture.at("cases").as_array()) {
    const auto case_name = item.at("name").as_string();
    const auto kind = item.at("kind").as_string();

    if (kind == "metadata") {
      const auto contract_json = agent_capi_contract_json()
                                     ? agent::parse_json(agent_capi_contract_json())
                                     : agent::Value::object({});
      const auto actual = agent::Value::object({
          {"abiVersion", agent_capi_abi_version()},
          {"version", agent_version() ? agent::Value(agent_version()) : agent::Value()},
          {"versionNegotiation", contract_json.at("versionNegotiation").at("negotiate")},
          {"errorObject", contract_json.at("errorObject").at("last")},
          {"hostRuntime", contract_json.at("hostRuntime").at("create")},
          {"asyncRunHandle", contract_json.at("asyncRunHandle").at("startText")},
          {"asyncIteratorCancel", contract_json.at("asyncIterator").at("cancel")},
          {"asyncIteratorNext", contract_json.at("asyncIterator").at("next")},
          {"toolRunStart", contract_json.at("toolRun").at("startJson")},
      });
      require_shape(actual, item.at("expectedShape"), case_name, "capi.json");
      continue;
    }

    if (kind == "toolRunJson") {
      agent_tool_run_runtime_t* runtime = nullptr;
      require_capi_success(agent_tool_run_runtime_create(&runtime), "agent_tool_run_runtime_create");
      if (runtime == nullptr) {
        throw std::runtime_error("agent_tool_run_runtime_create returned null runtime");
      }

      const auto start_json = agent::safe_json_stringify(item.at("startJson"));
      CApiOwnedString started_json;
      require_capi_success(
          agent_tool_run_start_json(runtime, start_json.c_str(), &started_json.value),
          "agent_tool_run_start_json");
      const auto started = agent::parse_json(started_json.value ? started_json.value : "");

      const auto event_json = agent::safe_json_stringify(item.at("eventJson"));
      CApiOwnedString appended_json;
      require_capi_success(
          agent_tool_run_append_event_json(runtime, "contract-tool-run", event_json.c_str(), &appended_json.value),
          "agent_tool_run_append_event_json");

      CApiOwnedString read_json;
      require_capi_success(
          agent_tool_run_read_json(runtime, "contract-tool-run", "{\"cursor\":0,\"limit\":10}", &read_json.value),
          "agent_tool_run_read_json");
      const auto read = agent::parse_json(read_json.value ? read_json.value : "");

      const auto update_json = agent::safe_json_stringify(item.at("updateJson"));
      CApiOwnedString updated_json;
      require_capi_success(
          agent_tool_run_update_json(runtime, "contract-tool-run", update_json.c_str(), &updated_json.value),
          "agent_tool_run_update_json");

      CApiOwnedString waited_json;
      require_capi_success(
          agent_tool_run_wait_json(runtime, "contract-tool-run", "{\"until\":\"ready\",\"timeoutMs\":10}", &waited_json.value),
          "agent_tool_run_wait_json");
      const auto waited = agent::parse_json(waited_json.value ? waited_json.value : "");

      CApiOwnedString list_json;
      require_capi_success(
          agent_tool_run_list_json(runtime, "{\"kind\":\"custom\"}", &list_json.value),
          "agent_tool_run_list_json");
      const auto listed = agent::parse_json(list_json.value ? list_json.value : "");

      const auto cancel_json = agent::safe_json_stringify(item.at("cancelJson"));
      CApiOwnedString cancelled_json;
      require_capi_success(
          agent_tool_run_cancel_json(runtime, "contract-tool-run", cancel_json.c_str(), &cancelled_json.value),
          "agent_tool_run_cancel_json");
      const auto cancelled = agent::parse_json(cancelled_json.value ? cancelled_json.value : "");
      agent_tool_run_runtime_release(runtime);

      const auto started_subset = agent::Value::object({
          {"runId", started.at("runId")},
          {"toolCallId", started.at("toolCallId")},
          {"toolName", started.at("toolName")},
          {"kind", started.at("kind")},
          {"label", started.at("label")},
          {"status", started.at("status")},
          {"startedAt", started.at("startedAt")},
          {"updatedAt", started.at("updatedAt")},
          {"ready", started.at("ready")},
          {"metadata", started.at("metadata")},
      });
      const auto read_subset = agent::Value::object({
          {"run", agent::Value::object({{"runId", read.at("run").at("runId")}})},
          {"cursor", read.at("cursor")},
          {"nextCursor", read.at("nextCursor")},
          {"events", read.at("events")},
      });
      const auto waited_subset = agent::Value::object({
          {"runId", waited.at("runId")},
          {"status", waited.at("status")},
          {"ready", waited.at("ready")},
          {"metadata", waited.at("metadata")},
      });
      const auto cancelled_subset = agent::Value::object({
          {"runId", cancelled.at("runId")},
          {"status", cancelled.at("status")},
          {"error", cancelled.at("error")},
      });
      const auto actual = agent::Value::object({
          {"started", started_subset},
          {"read", read_subset},
          {"waited", waited_subset},
          {"listCount", static_cast<long long>(listed.as_array().size())},
          {"cancelled", cancelled_subset},
      });
      require_shape(actual, item.at("expectedShape"), case_name, "capi.json");
      continue;
    }

    if (kind == "runJson") {
      agent_runner_t* runner = nullptr;
      const auto config_json = agent::safe_json_stringify(item.at("configJson"));
      const auto input_json = agent::safe_json_stringify(item.at("inputJson"));
      const auto agent_id = item.at("agentId").as_string();
      const char* requested_agent_id = agent_id.empty() ? nullptr : agent_id.c_str();
      require_capi_success(
          agent_runner_create_from_config_json(config_json.c_str(), requested_agent_id, &runner),
          "agent_runner_create_from_config_json");
      if (runner == nullptr) {
        throw std::runtime_error("agent_runner_create_from_config_json returned null runner");
      }

      CApiOwnedString result_json;
      require_capi_success(
          agent_runner_run_json(runner,
                                input_json.c_str(),
                                item.at("sessionId").as_string().c_str(),
                                &result_json.value),
          "agent_runner_run_json");
      const auto actual = agent::parse_json(result_json.value ? result_json.value : "");
      agent_runner_release(runner);
      require_shape(actual, item.at("expectedShape"), case_name, "capi.json");
      continue;
    }

    if (kind == "streamJson") {
      agent_runner_t* runner = nullptr;
      const auto config_json = agent::safe_json_stringify(item.at("configJson"));
      const auto input_json = agent::safe_json_stringify(item.at("inputJson"));
      const auto agent_id = item.at("agentId").as_string();
      const char* requested_agent_id = agent_id.empty() ? nullptr : agent_id.c_str();
      require_capi_success(
          agent_runner_create_from_config_json(config_json.c_str(), requested_agent_id, &runner),
          "agent_runner_create_from_config_json");
      if (runner == nullptr) {
        throw std::runtime_error("agent_runner_create_from_config_json returned null runner");
      }

      CApiStreamCapture capture;
      require_capi_success(
          agent_runner_stream_json(runner,
                                   input_json.c_str(),
                                   item.at("sessionId").as_string().c_str(),
                                   capture_capi_stream_event,
                                   &capture),
          "agent_runner_stream_json");
      agent_runner_release(runner);

      agent::Value done_result;
      for (const auto& event : capture.events) {
        if (event.at("type").as_string() == "done") {
          done_result = event.at("result");
        }
      }
      const auto actual = agent::Value::object({
          {"eventCount", static_cast<long long>(capture.events.size())},
          {"firstType", capture.events.empty() ? agent::Value() : capture.events.front().at("type")},
          {"lastType", capture.events.empty() ? agent::Value() : capture.events.back().at("type")},
          {"doneResult", done_result},
      });
      require_shape(actual, item.at("expectedShape"), case_name, "capi.json");
      continue;
    }

    if (kind == "streamEventsJson") {
      agent_runner_t* runner = nullptr;
      const auto config_json = agent::safe_json_stringify(item.at("configJson"));
      const auto input_json = agent::safe_json_stringify(item.at("inputJson"));
      const auto agent_id = item.at("agentId").as_string();
      const char* requested_agent_id = agent_id.empty() ? nullptr : agent_id.c_str();
      require_capi_success(
          agent_runner_create_from_config_json(config_json.c_str(), requested_agent_id, &runner),
          "agent_runner_create_from_config_json");
      if (runner == nullptr) {
        throw std::runtime_error("agent_runner_create_from_config_json returned null runner");
      }

      agent_runner_event_stream_t* stream = nullptr;
      require_capi_success(
          agent_runner_stream_events_json(runner,
                                          input_json.c_str(),
                                          item.at("sessionId").as_string().c_str(),
                                          static_cast<std::size_t>(item.at("capacity").as_integer(1)),
                                          &stream),
          "agent_runner_stream_events_json");
      if (stream == nullptr) {
        throw std::runtime_error("agent_runner_stream_events_json returned null stream");
      }

      agent::Value::Array events;
      while (true) {
        CApiOwnedString event_json;
        int32_t has_event = 0;
        require_capi_success(
            agent_runner_event_stream_next_json(stream, &event_json.value, &has_event),
            "agent_runner_event_stream_next_json");
        if (!has_event) {
          break;
        }
        events.push_back(agent::parse_json(event_json.value ? event_json.value : ""));
      }
      agent_runner_event_stream_release(stream);
      agent_runner_release(runner);

      agent::Value done_result;
      for (const auto& event : events) {
        if (event.at("type").as_string() == "done") {
          done_result = event.at("result");
        }
      }
      const auto actual = agent::Value::object({
          {"eventCount", static_cast<long long>(events.size())},
          {"firstType", events.empty() ? agent::Value() : events.front().at("type")},
          {"lastType", events.empty() ? agent::Value() : events.back().at("type")},
          {"firstSchemaVersion", events.empty() ? agent::Value() : events.front().at("schemaVersion")},
          {"firstSequence", events.empty() ? agent::Value() : events.front().at("sequence")},
          {"doneResult", done_result},
      });
      require_shape(actual, item.at("expectedShape"), case_name, "capi.json");
      continue;
    }

    throw std::runtime_error("Unsupported C API contract kind: " + kind);
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      std::cerr << "Usage: agent_contract_tests <contracts/observable dir>\n";
      return 2;
    }

    const std::filesystem::path contracts_dir = argv[1];
    run_message_contracts(contracts_dir);
    run_schema_contracts(contracts_dir);
    run_stream_schema_contracts(contracts_dir);
    run_tool_result_contracts(contracts_dir);
    run_event_contracts(contracts_dir);
    run_config_default_contracts(contracts_dir);
    run_tool_governance_contracts(contracts_dir);
    run_async_agent_run_contracts(contracts_dir);
    run_async_child_runtime_contracts();
    run_react_contracts(contracts_dir);
    run_react_prompt_mode_contracts();
    run_agent_runtime_builder_contracts();
    run_preset_contracts();
    run_capi_contracts(contracts_dir);
    std::cout << "agent_contract_tests OK\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "agent_contract_tests failed: " << error.what() << "\n";
    return 1;
  }
}
