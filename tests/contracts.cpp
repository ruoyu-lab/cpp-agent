#include "agent/agent.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

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

agent::ExecutionTarget execution_target_from_contract(const std::string& value) {
  if (value == "run") return agent::ExecutionTarget::Run;
  if (value == "model") return agent::ExecutionTarget::Model;
  if (value == "tool") return agent::ExecutionTarget::Tool;
  if (value == "retrieval") return agent::ExecutionTarget::Retrieval;
  if (value == "permission") return agent::ExecutionTarget::Permission;
  if (value == "workflow") return agent::ExecutionTarget::Workflow;
  if (value == "workflow_node") return agent::ExecutionTarget::WorkflowNode;
  if (value == "child_agent") return agent::ExecutionTarget::ChildAgent;
  throw std::runtime_error("Unsupported event target in contract: " + value);
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
  agent::Value result_kind;
  if (result.result) {
    result_kind = std::holds_alternative<agent::ToolResultEnvelope>(*result.result)
                      ? agent::Value("envelope")
                      : agent::Value("value");
  }
  return agent::Value::object({
      {"ok", result.ok},
      {"resultKind", result_kind},
      {"error", result.error},
      {"output", result.output},
      {"message", agent::agent_message_to_value(result.message)},
  });
}

agent::Value string_vector_to_contract_value(const std::vector<std::string>& values) {
  agent::Value::Array array;
  for (const auto& value : values) {
    array.push_back(value);
  }
  return agent::Value(std::move(array));
}

agent::Value agent_runner_config_defaults_value() {
  agent::AgentRunnerConfig config;
  return agent::Value::object({
      {"maxIterations", config.max_iterations},
      {"enablePlanning", config.enable_planning},
      {"advertiseSkills", config.advertise_skills},
      {"lazyToolMode", config.lazy_tool_mode},
      {"defaultSkills", string_vector_to_contract_value(config.default_skills)},
      {"forcedVisibleTools", string_vector_to_contract_value(config.forced_visible_tools)},
      {"skillModelConflictPolicy", agent::to_string(config.skill_model_conflict_policy)},
      {"skillEffortConflictPolicy", agent::to_string(config.skill_effort_conflict_policy)},
      {"hasAdapter", static_cast<bool>(config.adapter)},
      {"hasThinkingAdapter", static_cast<bool>(config.thinking_adapter)},
      {"hasCritiqueAdapter", static_cast<bool>(config.critique_adapter)},
      {"hasMemoryStore", static_cast<bool>(config.memory_store)},
      {"hasScratchStore", static_cast<bool>(config.scratch_store)},
      {"hasPlanner", static_cast<bool>(config.planner)},
  });
}

agent::Value agent_loop_config_defaults_value() {
  agent::AgentLoopConfig config;
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

void run_tool_result_contracts(const std::filesystem::path& contracts_dir) {
  const auto fixture = agent::read_json_file(contracts_dir / "tool-results.json");
  if (fixture.at("version").as_integer() != 1) {
    throw std::runtime_error("tool-results.json contract version must be 1");
  }

  for (const auto& item : fixture.at("cases").as_array()) {
    const auto case_name = item.at("name").as_string();
    const auto tool_call = tool_call_from_contract(item.at("toolCall"));
    agent::ToolRegistry registry;
    registry.register_tool(agent::define_tool(agent::ToolDefinition{
        .name = tool_call.name,
        .description = "Observable contract fixture tool",
        .execute = [handler = item.at("handler"), tool_call](const agent::Value&, agent::ToolExecutionContext&) {
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
    const auto& publish = item.at("publish");
    agent::EventBus bus;
    const auto event = bus.publish(
        publish.at("category").as_string(),
        execution_target_from_contract(publish.at("target").as_string()),
        publish.at("payload").is_null() ? agent::Value::object({}) : publish.at("payload"),
        trace_context_from_value(publish.at("trace")));
    require_shape(agent::framework_event_to_value(event), item.at("expectedShape"),
                  case_name, "events.json");
  }
}

void run_config_default_contracts(const std::filesystem::path& contracts_dir) {
  const auto fixture = agent::read_json_file(contracts_dir / "config-defaults.json");
  if (fixture.at("version").as_integer() != 1) {
    throw std::runtime_error("config-defaults.json contract version must be 1");
  }

  for (const auto& item : fixture.at("cases").as_array()) {
    const auto case_name = item.at("name").as_string();
    const auto actual = config_defaults_for_target(item.at("target").as_string());
    require_equal(actual, item.at("expected"), case_name, "config-defaults.json");
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
    run_tool_result_contracts(contracts_dir);
    run_event_contracts(contracts_dir);
    run_config_default_contracts(contracts_dir);
    std::cout << "agent_contract_tests OK\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "agent_contract_tests failed: " << error.what() << "\n";
    return 1;
  }
}
