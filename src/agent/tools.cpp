#include "agent/tools_api.hpp"
#include "agent/shell_classifier.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>

namespace agent {

namespace {

std::string lowercase_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool wildcard_match(std::string pattern, std::string text) {
  pattern = lowercase_copy(std::move(pattern));
  text = lowercase_copy(std::move(text));
  std::size_t pattern_index = 0;
  std::size_t text_index = 0;
  std::size_t star_index = std::string::npos;
  std::size_t match_index = 0;

  while (text_index < text.size()) {
    if (pattern_index < pattern.size() && pattern[pattern_index] == text[text_index]) {
      ++pattern_index;
      ++text_index;
      continue;
    }
    if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
      star_index = pattern_index++;
      match_index = text_index;
      continue;
    }
    if (star_index != std::string::npos) {
      pattern_index = star_index + 1;
      text_index = ++match_index;
      continue;
    }
    return false;
  }

  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
    ++pattern_index;
  }
  return pattern_index == pattern.size();
}

bool has_trace_values(const TraceContext& trace) {
  return !trace.trace_id.empty() || !trace.span_id.empty() || !trace.parent_span_id.empty() ||
         !trace.span_name.empty() || !trace.run_id.empty() || !trace.workflow_run_id.empty();
}

TraceContext tool_execution_trace_context(const TraceContext& parent) {
  const TraceContext* parent_ptr = has_trace_values(parent) ? &parent : nullptr;
  return derive_child_trace_context(parent_ptr);
}

bool tool_allowed_by_active_skills(const ToolCall& tool_call, const ToolExecutionContext& context) {
  const auto& allowed = context.services.at("skills").at("allowedTools").as_array();
  if (allowed.empty()) {
    return false;
  }

  const std::string serialized_arguments = safe_json_stringify(tool_call.arguments);
  for (const auto& item : allowed) {
    const std::string pattern = trim_copy(item.as_string());
    if (pattern.empty()) {
      continue;
    }
    const auto open = pattern.find('(');
    if (open != std::string::npos && pattern.back() == ')') {
      const std::string tool_pattern = trim_copy(pattern.substr(0, open));
      const std::string argument_pattern = trim_copy(pattern.substr(open + 1, pattern.size() - open - 2));
      if (!wildcard_match(tool_pattern, tool_call.name)) {
        continue;
      }
      if (argument_pattern.empty() || argument_pattern == "*" ||
          wildcard_match(argument_pattern, serialized_arguments)) {
        return true;
      }
      continue;
    }
    if (wildcard_match(pattern, tool_call.name)) {
      return true;
    }
  }
  return false;
}

template <typename T>
bool contains_value(const std::vector<T>& values, const T& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool matches_any(const std::vector<std::string>& actual, const std::vector<std::string>& expected) {
  if (expected.empty()) {
    return true;
  }
  return std::any_of(expected.begin(), expected.end(), [&](const std::string& value) {
    return contains_value(actual, value);
  });
}

template <typename T>
bool matches_scalar(const T& actual, const std::vector<T>& expected) {
  return expected.empty() || contains_value(expected, actual);
}

template <typename T>
bool matches_any_typed(const std::vector<T>& actual, const std::vector<T>& expected) {
  if (expected.empty()) {
    return true;
  }
  return std::any_of(expected.begin(), expected.end(), [&](const T& value) {
    return contains_value(actual, value);
  });
}

bool matches_permission_resources(const std::vector<PermissionResource>& resources,
                                  const std::vector<PermissionResourceMatcher>& matchers) {
  if (matchers.empty()) {
    return true;
  }
  return std::any_of(matchers.begin(), matchers.end(), [&](const PermissionResourceMatcher& matcher) {
    return std::any_of(resources.begin(), resources.end(), [&](const PermissionResource& resource) {
      return permission_resource_matches(resource, matcher);
    });
  });
}

bool matches_permission_rule(const PermissionRequest& request, const ToolPermissionMatcher& matcher) {
  return matches_scalar(request.tool_name, matcher.tool_names) &&
         matches_any(request.capabilities, matcher.capabilities) &&
         matches_any_typed(request.actions, matcher.actions) &&
         matches_permission_resources(request.resources, matcher.resource_matchers) &&
         matches_any(request.tags, matcher.tags) &&
         matches_scalar(request.risk_level, matcher.risk_levels) &&
         (!matcher.builtin || request.builtin == *matcher.builtin) &&
         matches_scalar(request.bundle, matcher.bundles);
}

bool matches_fine_grained_rule(const PermissionRequest& request,
                               const FineGrainedPermissionMatcher& matcher) {
  return matches_scalar(request.tool_name, matcher.tool_names) &&
         matches_any(request.capabilities, matcher.capabilities) &&
         matches_any_typed(request.actions, matcher.actions) &&
         matches_permission_resources(request.resources, matcher.resource_matchers) &&
         matches_any(request.tags, matcher.tags) &&
         matches_scalar(request.risk_level, matcher.risk_levels) &&
         (!matcher.builtin || request.builtin == *matcher.builtin) &&
         matches_scalar(request.bundle, matcher.bundles);
}

int risk_value(ToolRiskLevel risk) {
  switch (risk) {
    case ToolRiskLevel::Low:
      return 0;
    case ToolRiskLevel::Medium:
      return 1;
    case ToolRiskLevel::High:
      return 2;
  }
  return 0;
}

ToolRiskLevel max_risk(ToolRiskLevel left, ToolRiskLevel right) {
  return risk_value(left) >= risk_value(right) ? left : right;
}

std::vector<PermissionAction> actions_for_capability(const std::string& capability) {
  if (capability == "state.read") return {PermissionAction::StateRead};
  if (capability == "state.write") return {PermissionAction::StateWrite};
  if (capability == "network.search" || capability == "network.crawl" ||
      capability == "network.http.read" || capability == "network.http.write") {
    return {PermissionAction::NetworkConnect};
  }
  if (capability == "fs.read") return {PermissionAction::FilesystemRead};
  if (capability == "fs.write") return {PermissionAction::FilesystemWrite};
  if (capability == "browser.read") return {PermissionAction::BrowserRead};
  if (capability == "browser.screenshot") return {PermissionAction::BrowserScreenshot};
  if (capability == "repository.read") {
    return {PermissionAction::RepositoryRead, PermissionAction::FilesystemRead};
  }
  if (capability == "process.env.read") return {PermissionAction::EnvRead};
  if (capability == "process.exec") return {PermissionAction::ProcessExecute};
  if (capability == "memory.read") return {PermissionAction::MemoryRead};
  if (capability == "knowledge.read") return {PermissionAction::KnowledgeRead};
  if (capability == "workflow.read") return {PermissionAction::WorkflowRead};
  if (capability == "workflow.resume") return {PermissionAction::WorkflowResume};
  if (capability == "workflow.respond") return {PermissionAction::WorkflowRespond};
  if (capability == "workflow.control") return {PermissionAction::WorkflowControl};
  if (capability == "tool.discover") return {PermissionAction::ToolDiscover};
  return {PermissionAction::ToolInvoke};
}

PermissionResourceKind resource_kind_for_capability(const std::string& capability) {
  if (capability.rfind("fs.", 0) == 0) return PermissionResourceKind::Filesystem;
  if (capability.rfind("network.", 0) == 0) return PermissionResourceKind::Network;
  if (capability.rfind("process.", 0) == 0) return PermissionResourceKind::Process;
  if (capability.rfind("state.", 0) == 0) return PermissionResourceKind::State;
  if (capability.rfind("memory.", 0) == 0) return PermissionResourceKind::Memory;
  if (capability.rfind("knowledge.", 0) == 0) return PermissionResourceKind::Knowledge;
  if (capability.rfind("browser.", 0) == 0) return PermissionResourceKind::Browser;
  if (capability.rfind("repository.", 0) == 0) return PermissionResourceKind::Repository;
  if (capability.rfind("workflow.", 0) == 0) return PermissionResourceKind::Workflow;
  if (capability.rfind("tool.", 0) == 0) return PermissionResourceKind::Tool;
  return PermissionResourceKind::Unknown;
}

void push_unique_action(std::vector<PermissionAction>& target, PermissionAction action) {
  if (!contains_value(target, action)) {
    target.push_back(action);
  }
}

void append_actions(std::vector<PermissionAction>& target,
                    const std::vector<PermissionAction>& actions) {
  for (const auto action : actions) {
    push_unique_action(target, action);
  }
}

Value shell_findings_to_value(const ShellCommandClassification& classification) {
  Value::Array findings;
  for (const auto& finding : classification.findings) {
    findings.push_back(Value::object({
        {"category", finding.category},
        {"riskLevel", finding.risk_level},
        {"reason", finding.reason},
        {"command", finding.command},
    }));
  }
  return Value(std::move(findings));
}

Value strings_to_value_array(const std::vector<std::string>& values) {
  Value::Array out;
  for (const auto& value : values) {
    out.emplace_back(value);
  }
  return Value(std::move(out));
}

Value permission_actions_to_value_array(const std::vector<PermissionAction>& actions) {
  Value::Array out;
  for (const auto action : actions) {
    out.emplace_back(to_string(action));
  }
  return Value(std::move(out));
}

Value permission_resources_to_value_array(const std::vector<PermissionResource>& resources) {
  Value::Array out;
  for (const auto& resource : resources) {
    out.push_back(permission_resource_to_value(resource));
  }
  return Value(std::move(out));
}

PermissionRequest build_permission_request(const ToolDefinition& tool,
                                           const ToolCall& tool_call,
                                           const ToolExecutionContext& context) {
  PermissionRequest request;
  request.tool = PermissionToolRef{
      .name = tool.name,
      .capabilities = tool.capabilities,
      .risk_level = to_string(tool.risk_level),
      .tags = tool.tags,
      .bundle = tool.bundle,
      .builtin = tool.builtin,
  };
  request.tool_name = tool.name;
  request.capabilities = tool.capabilities;
  request.risk_level = tool.risk_level;
  request.tags = tool.tags;
  request.bundle = tool.bundle;
  request.builtin = tool.builtin;
  request.actions = {PermissionAction::ToolInvoke};
  request.resources = {PermissionResource{
      .kind = PermissionResourceKind::Tool,
      .id = tool.name,
      .actions = {PermissionAction::ToolInvoke},
      .source = PermissionDecisionSource::Default,
  }};
  request.tool_call = tool_call;
  request.context = context;

  for (const auto& capability : tool.capabilities) {
    const auto actions = actions_for_capability(capability);
    append_actions(request.actions, actions);
    request.resources.push_back(PermissionResource{
        .kind = resource_kind_for_capability(capability),
        .id = capability,
        .actions = actions,
        .source = PermissionDecisionSource::Capability,
    });
  }

  if (contains_value(tool.capabilities, std::string("process.exec")) || tool.name == "shell.exec") {
    const auto& args = tool_call.arguments;
    const auto command = args.at("command").as_string();
    if (!command.empty()) {
      std::vector<std::string> argv;
      if (args.at("args").is_array()) {
        for (const auto& value : args.at("args").as_array()) {
          argv.push_back(value.as_string());
        }
      }
      const bool exec_file_shape =
          !argv.empty() && command.find_first_of(";&|<>$`") == std::string::npos;
      auto classification = exec_file_shape ? classify_exec_file_command(command, argv)
                                            : classify_shell_command(command);
      append_actions(request.actions, classification.actions);
      for (auto& resource : classification.resources) {
        request.resources.push_back(std::move(resource));
      }
      request.risk_level = max_risk(request.risk_level,
                                    tool_risk_level_from_string(classification.risk_level,
                                                                ToolRiskLevel::Medium));
      PermissionResource classifier_resource;
      classifier_resource.kind = PermissionResourceKind::Process;
      classifier_resource.id = "shell.classification";
      classifier_resource.actions = {PermissionAction::ProcessExecute};
      classifier_resource.source = PermissionDecisionSource::Classifier;
      classifier_resource.metadata = Value::object({
          {"riskLevel", classification.risk_level},
          {"commands", strings_to_value_array(classification.commands)},
          {"findings", shell_findings_to_value(classification)},
          {"operators", strings_to_value_array(classification.ast.operators)},
          {"parseWarnings", strings_to_value_array(classification.ast.parse_warnings)},
      });
      request.resources.push_back(std::move(classifier_resource));
    }
  }

  request.tool.risk_level = to_string(request.risk_level);
  return request;
}

SandboxRequest permission_sandbox_request(const PermissionRequest& request,
                                          const PermissionDecision& decision) {
  auto resources = request.resources;
  resources.insert(resources.end(), decision.resources.begin(), decision.resources.end());
  auto from_resources = sandbox_request_from_permission_resources(
      resources, Value::object({{"source", "permission"}}));
  if (decision.sandbox_request) {
    from_resources = merge_sandbox_requests(from_resources, *decision.sandbox_request);
  }
  return from_resources;
}

ToolSandboxPolicy merge_permission_sandbox_policy(const ToolSandboxPolicy& policy,
                                                  const SandboxRequest& request) {
  ToolSandboxPolicy merged;
  merged.required = merge_sandbox_requests(policy.required, request);
  if (policy.preferred) {
    merged.preferred = merge_sandbox_requests(*policy.preferred, request);
  }
  return merged;
}

bool sandbox_request_is_empty(const SandboxRequest& request) {
  return capabilities_satisfy(SandboxCapabilities{}, request.capabilities) &&
         request.fs_read_paths.empty() &&
         request.fs_write_paths.empty() &&
         request.net_hosts.empty() &&
         request.allowed_subcommands.empty() &&
         (!request.extensions.is_object() || request.extensions.as_object().empty());
}

bool sandbox_policy_is_empty(const ToolSandboxPolicy& policy) {
  return sandbox_request_is_empty(policy.required) && !policy.preferred.has_value();
}

Value tool_call_hook_value(const ToolCall& tool_call) {
  return Value::object({
      {"id", tool_call.id},
      {"name", tool_call.name},
      {"arguments", tool_call.arguments},
  });
}

}  // namespace

bool ToolServiceView::has(const std::string& name) const {
  return services_.find(name) != services_.end();
}

std::vector<std::string> ToolServiceView::names() const {
  std::vector<std::string> names;
  names.reserve(services_.size());
  for (const auto& [name, _] : services_) {
    names.push_back(name);
  }
  return names;
}

bool ToolServiceContainer::has(const std::string& name) const {
  return services_.find(name) != services_.end();
}

ToolServiceView ToolServiceContainer::view(
    const std::vector<ToolServiceRequirement>& requirements) const {
  ToolServiceView scoped;
  for (const auto& requirement : requirements) {
    if (requirement.name.empty()) {
      throw ConfigurationError("Tool service requirement name is required.");
    }
    const auto found = services_.find(requirement.name);
    if (found != services_.end()) {
      scoped.services_[requirement.name] = found->second;
      continue;
    }
    if (!requirement.optional) {
      throw ConfigurationError("Tool service \"" + requirement.name + "\" is required but was not provided.");
    }
  }
  return scoped;
}

void ToolServiceContainer::merge_from(const ToolServiceContainer& other) {
  for (const auto& [name, binding] : other.services_) {
    services_[name] = binding;
  }
}

Value merge_tool_service_values(const Value& base, const Value& overlay) {
  if (!base.is_object()) {
    return overlay.is_object() ? overlay : Value::object({});
  }
  if (!overlay.is_object()) {
    return base;
  }

  Value merged = base;
  for (const auto& [key, value] : overlay.as_object()) {
    if (merged.contains(key) && merged.at(key).is_object() && value.is_object()) {
      merged[key] = merge_tool_service_values(merged.at(key), value);
    } else {
      merged[key] = value;
    }
  }
  return merged;
}

ToolExecutionServices merge_tool_execution_services(const ToolExecutionServices& base,
                                                    const ToolExecutionServices& overlay) {
  ToolExecutionServices merged = base;
  merged.values = merge_tool_service_values(base.values, overlay.values);
  merged.service_container.merge_from(overlay.service_container);
  merged.service_view = ToolServiceView{};
  return merged;
}

ToolExecutionContext with_tool_execution_services(ToolExecutionContext context,
                                                  const ToolExecutionServices& configured_services,
                                                  const ToolExecutionServices& runtime_services) {
  ToolExecutionServices context_services = context.service_refs;
  context_services.values = merge_tool_service_values(context.services, context.service_refs.values);
  auto merged = merge_tool_execution_services(context_services, configured_services);
  merged = merge_tool_execution_services(merged, runtime_services);
  context.service_refs = merged;
  context.services = merged.values;
  return normalize_tool_execution_context(std::move(context));
}

ToolExecutionContext normalize_tool_execution_context(ToolExecutionContext context) {
  if (!context.services.is_object()) {
    context.services = Value::object({});
  }
  if (!context.attributes.is_object()) {
    context.attributes = Value::object({});
  }
  context.service_refs.values = merge_tool_service_values(context.services, context.service_refs.values);
  context.services = context.service_refs.values;
  return context;
}

void emit_tool_progress(ToolExecutionContext& context,
                        std::string kind,
                        Value payload) {
  if (!context.emit_progress || kind.empty()) {
    return;
  }
  ToolProgressEvent event;
  event.kind = std::move(kind);
  if (context.tool_call) {
    event.tool_call_id = context.tool_call->id;
    event.tool_name = context.tool_call->name;
  }
  event.iteration = context.iteration;
  event.payload = std::move(payload);
  event.trace = context.trace_context;
  context.emit_progress(event);
}

std::string to_string(ToolRiskLevel risk) {
  switch (risk) {
    case ToolRiskLevel::Low:
      return "low";
    case ToolRiskLevel::Medium:
      return "medium";
    case ToolRiskLevel::High:
      return "high";
  }
  return "low";
}

ToolRiskLevel tool_risk_level_from_string(const std::string& value, ToolRiskLevel fallback) {
  if (value == "low") {
    return ToolRiskLevel::Low;
  }
  if (value == "medium") {
    return ToolRiskLevel::Medium;
  }
  if (value == "high") {
    return ToolRiskLevel::High;
  }
  return fallback;
}

std::string to_string(PermissionDecisionKind decision) {
  switch (decision) {
    case PermissionDecisionKind::Allow:
      return "allow";
    case PermissionDecisionKind::Deny:
      return "deny";
    case PermissionDecisionKind::Ask:
      return "ask";
  }
  return "deny";
}

PermissionDecisionKind permission_decision_kind_from_string(const std::string& value,
                                                            PermissionDecisionKind fallback) {
  if (value == "allow") {
    return PermissionDecisionKind::Allow;
  }
  if (value == "deny") {
    return PermissionDecisionKind::Deny;
  }
  if (value == "ask") {
    return PermissionDecisionKind::Ask;
  }
  return fallback;
}

Value permission_event_payload(const PermissionRequest& request, const PermissionDecision& decision) {
  return Value::object({{"toolName", request.tool_name},
                        {"toolCallId", request.tool_call.id},
                        {"decision", to_string(decision.decision)},
                        {"reason", decision.reason},
                        {"source", to_string(decision.source)},
                        {"ruleId", decision.rule_id.empty() ? Value() : Value(decision.rule_id)},
                        {"priority", decision.priority},
                        {"riskLevel", to_string(request.risk_level)},
                        {"actions", permission_actions_to_value_array(request.actions)},
                        {"resources", permission_resources_to_value_array(request.resources)},
                        {"decisionResources", permission_resources_to_value_array(decision.resources)},
                        {"sandboxRequest", decision.sandbox_request ? sandbox_request_to_value(*decision.sandbox_request) : Value()},
                        {"bundle", request.bundle},
                        {"builtin", request.builtin}});
}

Value tool_progress_event_payload(const ToolProgressEvent& event) {
  return Value::object({
      {"toolCallId", event.tool_call_id},
      {"toolName", event.tool_name},
      {"iteration", event.iteration},
      {"sequence", static_cast<long long>(event.sequence)},
      {"kind", event.kind},
      {"payload", event.payload.is_null() ? Value::object({}) : event.payload},
  });
}

std::string tool_progress_event_category(const ToolProgressEvent& event) {
  if (event.kind == "tool.delta" || event.kind.rfind("tool.delta.", 0) == 0) {
    return "tool.delta";
  }
  return "tool.progress";
}

Value retry_scheduled_tool_payload(const RetryScheduledContext& retry,
                                   const ToolDefinition& tool,
                                   const ToolCall& tool_call) {
  Value payload = Value::object({
      {"attempt", retry.attempt},
      {"delayMs", retry.delay_ms},
      {"error", retry.error},
      {"target", to_string(retry.target)},
      {"toolName", tool.name},
      {"toolCallId", tool_call.id},
  });
  if (retry.metadata.is_object() && !retry.metadata.as_object().empty()) {
    payload["metadata"] = retry.metadata;
  }
  return payload;
}

Value successful_tool_metadata(const Value& metadata) {
  Value merged = Value::object({{"ok", true}});
  if (metadata.is_object()) {
    for (const auto& [key, value] : metadata.as_object()) {
      merged[key] = value;
    }
  }
  return merged;
}

Value strings_to_value(const std::vector<std::string>& values) {
  Value::Array array;
  for (const auto& value : values) {
    array.push_back(value);
  }
  return Value(std::move(array));
}

Value tool_audit_payload(const std::string& lifecycle, const ToolCall& tool_call, Value metadata = Value::object({})) {
  Value payload = Value::object({
      {"lifecycle", lifecycle},
      {"toolName", tool_call.name},
      {"toolCallId", tool_call.id},
      {"ok", Value()},
      {"error", Value()},
  });
  if (metadata.is_object()) {
    for (const auto& [key, value] : metadata.as_object()) {
      payload[key] = value;
    }
  }
  return payload;
}

Value tool_result_payload_value(const ToolInvokeResult& result) {
  if (std::holds_alternative<Value>(result)) {
    return std::get<Value>(result);
  }
  const auto& envelope = std::get<ToolResultEnvelope>(result);
  if (envelope.value) {
    return *envelope.value;
  }
  Value::Object object{{"metadata", envelope.metadata}};
  if (envelope.content) {
    object["content"] = agent_message_to_value(
        create_message(MessageRole::Assistant, *envelope.content)).at("content");
  }
  return Value(std::move(object));
}

Value tool_result_metadata_payload(const ToolInvokeResult& result) {
  if (!std::holds_alternative<ToolResultEnvelope>(result)) {
    return Value();
  }
  const auto& metadata = std::get<ToolResultEnvelope>(result).metadata;
  return metadata.is_object() && !metadata.as_object().empty() ? metadata : Value();
}

Value tool_result_content_payload(const ToolInvokeResult& result) {
  if (!std::holds_alternative<ToolResultEnvelope>(result)) {
    return Value();
  }
  const auto& envelope = std::get<ToolResultEnvelope>(result);
  if (!envelope.content) {
    return Value();
  }
  return agent_message_to_value(create_message(MessageRole::Assistant, *envelope.content)).at("content");
}

Value tool_error_payload(const std::exception& error, const std::string& lifecycle_message) {
  std::string type = "std::exception";
  if (dynamic_cast<const ToolExecutionError*>(&error)) {
    type = "ToolExecutionError";
  } else if (dynamic_cast<const PermissionDeniedError*>(&error)) {
    type = "PermissionDeniedError";
  } else if (dynamic_cast<const ConfigurationError*>(&error)) {
    type = "ConfigurationError";
  } else if (dynamic_cast<const AgentFrameworkError*>(&error)) {
    type = "AgentFrameworkError";
  }
  return Value::object({
      {"message", error.what()},
      {"type", type},
      {"lifecycleMessage", lifecycle_message},
  });
}

Value tool_lifecycle_base_payload(const std::string& phase,
                                  const std::string& tool_name,
                                  const ToolCall& tool_call,
                                  const ToolExecutionContext& context,
                                  const std::string& at_key,
                                  const std::string& at) {
  return Value::object({
      {"phase", phase},
      {"toolName", tool_name},
      {"toolCallId", tool_call.id},
      {"iteration", context.iteration},
      {"arguments", tool_call.arguments},
      {at_key, at},
  });
}

std::string format_tool_error_output(const std::string& message) {
  return "{\n  \"ok\": false,\n  \"error\": " + Value(message).stringify(0) + "\n}";
}

std::string permission_denied_message(const std::string& tool_name) {
  return "Tool \"" + tool_name + "\" is not permitted.";
}

std::string tool_failed_message(const std::string& tool_name) {
  return "Tool \"" + tool_name + "\" failed.";
}

Value ToolDefinition::validate_input(const Value& input) const {
  assert_json_schema(input_schema, input);
  return input;
}

ToolInvokeResult ToolDefinition::invoke(const Value& input, ToolExecutionContext& context) const {
  if (!execute) {
    throw ConfigurationError("Tool \"" + name + "\" requires an execute function.");
  }
  return execute(validate_input(input), context);
}

ChatToolDescriptor ToolDefinition::descriptor() const {
  return ChatToolDescriptor{
      .name = name,
      .description = description,
      .input_schema = input_schema,
      .read_only = read_only,
      .mutates_files = mutates_files,
      .interactive = interactive,
      .long_running = long_running,
      .batchable = batchable,
      .concurrency_key = concurrency_key,
      .side_effect_level = side_effect_level,
  };
}

ToolDefinition define_tool(ToolDefinition tool) {
  if (tool.name.empty()) {
    throw ConfigurationError("Tool name is required.");
  }
  if (!tool.execute) {
    throw ConfigurationError("Tool \"" + tool.name + "\" requires an execute function.");
  }
  tool.input_schema = normalize_json_schema(tool.input_schema);
  return tool;
}

ToolRegistry::ToolRegistry(std::vector<ToolDefinition> tools) {
  for (auto& tool : tools) {
    register_tool(std::move(tool));
  }
}

ToolRegistry::ToolRegistry(const ToolRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  tools_ = other.tools_;
  lazy_mode = other.lazy_mode;
  forced_visible_ = other.forced_visible_;
}

ToolRegistry& ToolRegistry::operator=(const ToolRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  tools_ = other.tools_;
  lazy_mode = other.lazy_mode;
  forced_visible_ = other.forced_visible_;
  return *this;
}

ToolRegistry::ToolRegistry(ToolRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  tools_ = std::move(other.tools_);
  lazy_mode = other.lazy_mode;
  forced_visible_ = std::move(other.forced_visible_);
}

ToolRegistry& ToolRegistry::operator=(ToolRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  tools_ = std::move(other.tools_);
  lazy_mode = other.lazy_mode;
  forced_visible_ = std::move(other.forced_visible_);
  return *this;
}

ToolDefinition& ToolRegistry::register_tool(ToolDefinition tool) {
  tool = define_tool(std::move(tool));
  std::lock_guard<std::mutex> lock(mutex_);
  const auto name = tool.name;
  tools_[name] = std::move(tool);
  return tools_.at(name);
}

const ToolDefinition* ToolRegistry::get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = tools_.find(name);
  return found == tools_.end() ? nullptr : &found->second;
}

std::optional<ToolDefinition> ToolRegistry::find(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = tools_.find(name);
  return found == tools_.end() ? std::nullopt : std::optional<ToolDefinition>(found->second);
}

std::vector<ToolDefinition> ToolRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ToolDefinition> tools;
  tools.reserve(tools_.size());
  for (const auto& [name, tool] : tools_) {
    if (lazy_mode) {
      const bool tool_namespace = name.size() >= 5 && name.compare(0, 5, "tool.") == 0;
      const bool forced = forced_visible_.count(name) > 0;
      if (!tool_namespace && !forced) {
        continue;
      }
    }
    tools.push_back(tool);
  }
  return tools;
}

std::vector<ToolDefinition> ToolRegistry::list_all() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ToolDefinition> tools;
  tools.reserve(tools_.size());
  for (const auto& [_, tool] : tools_) {
    tools.push_back(tool);
  }
  return tools;
}

std::vector<ChatToolDescriptor> ToolRegistry::descriptors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ChatToolDescriptor> result;
  result.reserve(tools_.size());
  for (const auto& [name, tool] : tools_) {
    if (lazy_mode) {
      const bool tool_namespace = name.size() >= 5 && name.compare(0, 5, "tool.") == 0;
      const bool forced = forced_visible_.count(name) > 0;
      if (!tool_namespace && !forced) {
        continue;
      }
    }
    result.push_back(tool.descriptor());
  }
  return result;
}

void ToolRegistry::set_lazy_mode(bool lazy) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  lazy_mode = lazy;
}

void ToolRegistry::force_visible(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  forced_visible_.insert(name);
}

bool ToolRegistry::is_visible(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!lazy_mode) {
    return tools_.count(name) > 0;
  }
  if (forced_visible_.count(name) > 0) return true;
  if (name.size() >= 5 && name.compare(0, 5, "tool.") == 0) return true;
  return false;
}

MemoryApprovalRecorder::MemoryApprovalRecorder(PermissionDecision response)
    : response_(std::move(response)) {}

PermissionDecision MemoryApprovalRecorder::approve(const PermissionRequest& request,
                                                   const PermissionDecision& decision) {
  records_.push_back(MemoryApprovalRecord{request, decision});
  return response_;
}

PermissionApprovalHandler MemoryApprovalRecorder::handler() {
  return [this](const PermissionRequest& request, const PermissionDecision& decision) {
    return approve(request, decision);
  };
}

const std::vector<MemoryApprovalRecord>& MemoryApprovalRecorder::records() const noexcept {
  return records_;
}

PermissionPolicy create_rule_based_permission_policy(RuleBasedPermissionPolicyConfig config) {
  return [config = std::move(config)](const PermissionRequest& request) -> PermissionDecision {
    for (const auto& rule : config.rules) {
      if (matches_permission_rule(request, rule.match)) {
        return PermissionDecision{.decision = rule.decision,
                                  .reason = rule.reason,
                                  .source = PermissionDecisionSource::Rule};
      }
    }
    if (request.capabilities.empty()) {
      return PermissionDecision{.decision = PermissionDecisionKind::Allow};
    }
    return PermissionDecision{
        .decision = config.default_decision,
        .reason = config.default_reason.empty() ? "No permission rule matched the tool request." : config.default_reason,
        .source = PermissionDecisionSource::Default,
    };
  };
}

PermissionPolicy create_capability_policy(CapabilityPermissionPolicyConfig config) {
  return [config = std::move(config)](const PermissionRequest& request) -> PermissionDecision {
    const auto has_capability = [&](const std::vector<std::string>& expected) {
      return std::any_of(request.capabilities.begin(), request.capabilities.end(), [&](const std::string& capability) {
        return contains_value(expected, capability);
      });
    };

    if (has_capability(config.deny)) {
      return PermissionDecision{.decision = PermissionDecisionKind::Deny,
                                .reason = "Denied by capability policy.",
                                .source = PermissionDecisionSource::Capability};
    }
    if (has_capability(config.ask)) {
      return PermissionDecision{.decision = PermissionDecisionKind::Ask,
                                .reason = "Capability requires approval.",
                                .source = PermissionDecisionSource::Capability};
    }
    if (request.risk_level == ToolRiskLevel::High) {
      return PermissionDecision{.decision = config.high_risk_mode,
                                .reason = "High-risk tool execution.",
                                .source = PermissionDecisionSource::Capability};
    }
    if (!request.capabilities.empty() &&
        std::all_of(request.capabilities.begin(), request.capabilities.end(), [&](const std::string& capability) {
          return contains_value(config.allow, capability);
        })) {
      return PermissionDecision{.decision = PermissionDecisionKind::Allow,
                                .source = PermissionDecisionSource::Capability};
    }
    if (request.capabilities.empty()) {
      return PermissionDecision{.decision = PermissionDecisionKind::Allow,
                                .source = PermissionDecisionSource::Capability};
    }
    return PermissionDecision{.decision = PermissionDecisionKind::Deny,
                              .reason = "Capability is not allowed.",
                              .source = PermissionDecisionSource::Capability};
  };
}

PermissionPolicy create_fine_grained_permission_policy(FineGrainedPermissionPolicyConfig config) {
  return [config = std::move(config)](const PermissionRequest& request) -> PermissionDecision {
    const FineGrainedPermissionRule* best = nullptr;
    std::size_t best_index = 0;
    for (std::size_t index = 0; index < config.rules.size(); ++index) {
      const auto& rule = config.rules[index];
      if (!matches_fine_grained_rule(request, rule.match)) {
        continue;
      }
      if (!best || rule.priority > best->priority ||
          (rule.priority == best->priority && index < best_index)) {
        best = &rule;
        best_index = index;
      }
    }
    if (best) {
      return PermissionDecision{
          .decision = best->decision,
          .reason = best->reason,
          .source = best->source,
          .rule_id = best->id,
          .priority = best->priority,
          .resources = best->resources,
          .sandbox_request = best->sandbox_request,
      };
    }
    if (request.capabilities.empty() && request.actions.size() <= 1) {
      return PermissionDecision{.decision = PermissionDecisionKind::Allow,
                                .source = PermissionDecisionSource::Default};
    }
    return PermissionDecision{
        .decision = config.default_decision,
        .reason = config.default_reason.empty() ? "No fine-grained permission rule matched the tool request."
                                                : config.default_reason,
        .source = PermissionDecisionSource::Default,
    };
  };
}

PermissionApprovalHandler create_static_approval_handler(PermissionDecision decision) {
  return [decision = std::move(decision)](const PermissionRequest&, const PermissionDecision&) {
    return decision;
  };
}

PermissionApprovalHandler create_cli_approval_handler(CliApprovalHandlerConfig config) {
  return [config = std::move(config)](const PermissionRequest& request,
                                      const PermissionDecision& decision) mutable -> PermissionDecision {
    std::istream* input = config.input ? config.input : &std::cin;
    std::ostream* output = config.output ? config.output : &std::cout;
    if (!input || !output || !(*input) || !(*output)) {
      return PermissionDecision{.decision = config.default_decision,
                                .reason = "CLI approval streams are not available.",
                                .source = PermissionDecisionSource::Approval};
    }

    const std::string prompt = config.prompt_formatter
                                   ? config.prompt_formatter(request, decision)
                                   : "Allow tool \"" + request.tool_name + "\" from bundle \"" +
                                         (request.bundle.empty() ? "unscoped" : request.bundle) + "\"? [y/N] ";
    (*output) << prompt;
    output->flush();

    std::string answer;
    if (!std::getline(*input, answer)) {
      return PermissionDecision{.decision = config.default_decision,
                                .reason = decision.reason,
                                .source = PermissionDecisionSource::Approval};
    }
    std::transform(answer.begin(), answer.end(), answer.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    answer = trim_copy(std::move(answer));
    if (answer == "y" || answer == "yes") {
      return PermissionDecision{.decision = PermissionDecisionKind::Allow,
                                .source = PermissionDecisionSource::Approval};
    }
    return PermissionDecision{.decision = config.default_decision,
                              .reason = decision.reason,
                              .source = PermissionDecisionSource::Approval};
  };
}

ToolExecutor::ToolExecutor(ToolRegistry& registry, PermissionPolicy permission_policy,
                           PermissionApprovalHandler approval_handler, EventBus* event_bus,
                           ExecutionPolicies execution_policies, HookSet hooks)
    : registry_(registry),
      permission_policy_(std::move(permission_policy)),
      approval_handler_(std::move(approval_handler)),
      event_bus_(event_bus),
      execution_policies_(std::move(execution_policies)),
      hooks_(std::move(hooks)) {}

ToolExecutionResult ToolExecutor::execute_tool_call(const ToolCall& tool_call, ToolExecutionContext context) {
  context = normalize_tool_execution_context(std::move(context));
  context.trace_context = tool_execution_trace_context(context.trace_context);
  context.tool_call = tool_call;
  auto progress_sequence = std::make_shared<std::atomic<std::uint64_t>>(0);
  auto forwarded_progress = context.emit_progress;
  context.emit_progress = [this, tool_call, iteration = context.iteration,
                           trace = context.trace_context, progress_sequence,
                           forwarded_progress](ToolProgressEvent event) mutable {
    if (event.kind.empty()) {
      return;
    }
    event.tool_call_id = tool_call.id;
    event.tool_name = tool_call.name;
    event.iteration = iteration;
    event.sequence = progress_sequence->fetch_add(1, std::memory_order_relaxed);
    event.trace = trace;
    if (event.payload.is_null()) {
      event.payload = Value::object({});
    }
    if (event_bus_) {
      event_bus_->publish(tool_progress_event_category(event), ExecutionTarget::Tool,
                          tool_progress_event_payload(event),
                          trace);
    }
    if (forwarded_progress) {
      forwarded_progress(event);
    }
  };
  // Expose the registry to tools that need it (e.g. tool.search / tool.describe).
  if (!context.service_refs.service_container.has(kToolServiceToolRegistry.name)) {
    context.service_refs.service_container.set(kToolServiceToolRegistry, &registry_);
  }
  if (!context.service_refs.service_container.has(kToolServiceHooks.name)) {
    context.service_refs.service_container.set(kToolServiceHooks, &hooks_);
  }
  auto tool_snapshot = registry_.find(tool_call.name);
  if (!tool_snapshot) {
    const std::string error_message = "Unknown tool: " + tool_call.name;
    const std::string output = format_tool_error_output(error_message);
    if (event_bus_) {
      event_bus_->publish("tool.audit", ExecutionTarget::Tool,
                          tool_audit_payload("failed", tool_call,
                                             Value::object({{"ok", false},
                                                           {"error", error_message}})),
                          context.trace_context);
      auto failed_payload = tool_lifecycle_base_payload("failed", tool_call.name, tool_call, context,
                                                        "failedAt", now_iso8601());
      failed_payload["ok"] = false;
      failed_payload["error"] = Value::object({
          {"message", error_message},
          {"type", "ToolExecutionError"},
          {"lifecycleMessage", error_message},
      });
      failed_payload["output"] = output;
      failed_payload["durationMs"] = 0;
      event_bus_->publish("tool.failed", ExecutionTarget::Tool, failed_payload, context.trace_context);
    }
    return ToolExecutionResult{
        .tool_call = tool_call,
        .ok = false,
        .error = error_message,
        .output = output,
        .message = create_tool_result_message(tool_call.id, tool_call.name, output, Value::object({{"ok", false}})),
    };
  }
  const ToolDefinition& tool = *tool_snapshot;
  PermissionRequest permission_request = build_permission_request(tool, tool_call, context);
  PermissionDecision permission_decision{.decision = PermissionDecisionKind::Allow,
                                         .source = PermissionDecisionSource::Default};
  const bool skill_allowed = tool_allowed_by_active_skills(tool_call, context);

  if (skill_allowed) {
    permission_decision.source = PermissionDecisionSource::Skill;
  }

  if (!skill_allowed && permission_policy_) {
    if (event_bus_) {
      event_bus_->publish("tool.audit", ExecutionTarget::Tool,
                          tool_audit_payload(
                              "requested", tool_call,
                              Value::object({{"riskLevel", to_string(tool.risk_level)},
                                            {"effectiveRiskLevel", to_string(permission_request.risk_level)},
                                            {"capabilities", strings_to_value(tool.capabilities)},
                                            {"actions", [&]() {
                                               Value::Array out;
                                               for (const auto action : permission_request.actions) {
                                                 out.emplace_back(to_string(action));
                                               }
                                               return Value(std::move(out));
                                             }()},
                                            {"bundle", tool.bundle.empty() ? Value() : Value(tool.bundle)}})),
                          context.trace_context);
    }
    if (hooks_.before_permission_check) {
      PermissionHookContext hook_context;
      hook_context.target = ExecutionTarget::Permission;
      hook_context.trace_id = context.trace_context.trace_id;
      hook_context.run_id = context.trace_context.run_id;
      hook_context.workflow_run_id = context.trace_context.workflow_run_id;
      hook_context.tool_name = tool.name;
      hooks_.before_permission_check(hook_context);
    }
    PermissionDecision decision = permission_policy_(permission_request);
    if (decision.decision == PermissionDecisionKind::Ask) {
      const PermissionDecision proposed_decision = decision;
      if (event_bus_) {
        event_bus_->publish("permission.approval_requested", ExecutionTarget::Permission,
                            permission_event_payload(permission_request, decision), context.trace_context);
      }
      decision = approval_handler_ ? approval_handler_(permission_request, decision)
                                   : PermissionDecision{
                                         .decision = PermissionDecisionKind::Deny,
                                         .reason = "Approval handler is not configured.",
                                         .source = PermissionDecisionSource::Approval,
                                     };
      if (decision.decision == PermissionDecisionKind::Allow) {
        if (decision.resources.empty()) {
          decision.resources = proposed_decision.resources;
        }
        if (!decision.sandbox_request && proposed_decision.sandbox_request) {
          decision.sandbox_request = proposed_decision.sandbox_request;
        }
        if (decision.source == PermissionDecisionSource::Default) {
          decision.source = PermissionDecisionSource::Approval;
        }
        if (decision.rule_id.empty()) {
          decision.rule_id = proposed_decision.rule_id;
        }
        if (decision.priority == 0) {
          decision.priority = proposed_decision.priority;
        }
      }
    }
    permission_decision = decision;
    if (decision.decision != PermissionDecisionKind::Allow) {
      const std::string reason = decision.reason.empty() ? "Tool is not permitted." : decision.reason;
      const std::string error_message = permission_denied_message(tool.name);
      if (hooks_.after_permission_check) {
        PermissionHookContext hook_context;
        hook_context.target = ExecutionTarget::Permission;
        hook_context.trace_id = context.trace_context.trace_id;
        hook_context.run_id = context.trace_context.run_id;
        hook_context.workflow_run_id = context.trace_context.workflow_run_id;
        hook_context.tool_name = tool.name;
        hook_context.decision = to_string(decision.decision);
        hook_context.reason = reason;
        hooks_.after_permission_check(hook_context);
      }
      if (event_bus_) {
        auto denied_decision = decision;
        denied_decision.reason = reason;
        event_bus_->publish("permission.denied", ExecutionTarget::Permission,
                            permission_event_payload(permission_request, denied_decision),
                            context.trace_context);
        event_bus_->publish("tool.audit", ExecutionTarget::Tool,
                            tool_audit_payload("denied", tool_call,
                                               Value::object({{"ok", false}, {"error", error_message},
                                                             {"reason", reason}})),
                            context.trace_context);
      }
      const std::string output = format_tool_error_output(error_message);
      if (event_bus_) {
        auto failed_payload = tool_lifecycle_base_payload("failed", tool.name, tool_call, context,
                                                          "failedAt", now_iso8601());
        failed_payload["ok"] = false;
        failed_payload["error"] = Value::object({
            {"message", error_message},
            {"type", "PermissionDeniedError"},
            {"reason", reason},
            {"lifecycleMessage", error_message},
        });
        failed_payload["output"] = output;
        failed_payload["durationMs"] = 0;
        event_bus_->publish("tool.failed", ExecutionTarget::Tool, failed_payload, context.trace_context);
      }
      return ToolExecutionResult{
          .tool_call = tool_call,
          .ok = false,
          .error = error_message,
          .output = output,
          .message = create_tool_result_message(tool_call.id, tool_call.name, output, Value::object({{"ok", false}})),
      };
    }
    if (hooks_.after_permission_check) {
      PermissionHookContext hook_context;
      hook_context.target = ExecutionTarget::Permission;
      hook_context.trace_id = context.trace_context.trace_id;
      hook_context.run_id = context.trace_context.run_id;
      hook_context.workflow_run_id = context.trace_context.workflow_run_id;
      hook_context.tool_name = tool.name;
      hook_context.decision = to_string(decision.decision);
      hook_context.reason = decision.reason;
      hooks_.after_permission_check(hook_context);
    }
    if (event_bus_) {
      event_bus_->publish("permission.allowed", ExecutionTarget::Permission,
                          permission_event_payload(permission_request, decision), context.trace_context);
      event_bus_->publish("permission.checked", ExecutionTarget::Permission,
                          Value::object({{"toolName", tool.name}, {"decision", "allow"}}),
                          context.trace_context);
      event_bus_->publish("tool.audit", ExecutionTarget::Tool,
                          tool_audit_payload("approved", tool_call, Value::object({{"ok", true}})),
                          context.trace_context);
    }
  }

  std::string lifecycle_started_at;
  auto lifecycle_start = std::chrono::steady_clock::now();
  bool lifecycle_started = false;
  try {
    if (hooks_.before_tool) {
      ToolHookContext hook_context;
      hook_context.target = ExecutionTarget::Tool;
      hook_context.trace_id = context.trace_context.trace_id;
      hook_context.run_id = context.trace_context.run_id;
      hook_context.workflow_run_id = context.trace_context.workflow_run_id;
      hook_context.tool_name = tool.name;
      hook_context.tool_call = tool_call_hook_value(tool_call);
      hook_context.input = tool_call.arguments;
      hooks_.before_tool(hook_context);
    }
    if (event_bus_) {
      event_bus_->publish("tool.audit", ExecutionTarget::Tool,
                          tool_audit_payload("started", tool_call), context.trace_context);
      lifecycle_started_at = now_iso8601();
      lifecycle_start = std::chrono::steady_clock::now();
      lifecycle_started = true;
      event_bus_->publish("tool.started", ExecutionTarget::Tool,
                          tool_lifecycle_base_payload("started", tool.name, tool_call, context,
                                                      "startedAt", lifecycle_started_at),
                          context.trace_context);
    }
    // Sandbox contract: refuse to dispatch if the installed sandbox cannot
    // satisfy the tool's policy. `enforce_sandbox_contract` returns the
    // effective request (preferred when supported, else required) and emits
    // `sandbox.entered` / `sandbox.violated`. On rejection it throws
    // `SandboxContractError`, which bubbles up through the executor's normal
    // error path and is surfaced as a tool failure.
    Sandbox* active_sandbox = context.service_refs.service_container.get(kToolServiceSandbox);
    const bool should_apply_permission_sandbox =
        active_sandbox != nullptr ||
        !sandbox_policy_is_empty(tool.sandbox_policy) ||
        permission_decision.sandbox_request.has_value() ||
        !permission_decision.resources.empty();
    const auto effective_policy =
        should_apply_permission_sandbox
            ? merge_permission_sandbox_policy(tool.sandbox_policy,
                                              permission_sandbox_request(permission_request,
                                                                        permission_decision))
            : tool.sandbox_policy;
    auto effective_request =
        enforce_sandbox_contract(effective_policy, active_sandbox, event_bus_);
    std::unique_ptr<SandboxScope> sandbox_scope;
    if (active_sandbox) {
      sandbox_scope = active_sandbox->enter(effective_request);
    }
    context.service_refs.service_container.set(kToolServiceSandboxScope, sandbox_scope.get());
    // RAII: clear the borrowed scope pointer on every path (success, throw,
    // return), and emit `sandbox.exited` if a scope was actually entered.
    struct SandboxScopeGuard {
      ToolExecutionContext* context_ptr;
      EventBus* event_bus;
      bool entered;
      std::chrono::steady_clock::time_point start;
      const TraceContext* trace;
      ~SandboxScopeGuard() {
        context_ptr->service_refs.service_container.set(kToolServiceSandboxScope,
                                                        static_cast<SandboxScope*>(nullptr));
        if (event_bus && entered) {
          const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start).count();
          event_bus->publish("sandbox.exited", ExecutionTarget::Tool,
                             Value::object({{"durationMs", static_cast<long long>(duration_ms)}}),
                             *trace);
        }
      }
    } scope_guard{&context, event_bus_, static_cast<bool>(sandbox_scope),
                  std::chrono::steady_clock::now(), &context.trace_context};
    context.service_refs.service_view =
        context.service_refs.service_container.view(tool.service_requirements);
    context.service_refs.service_container = ToolServiceContainer{};
    ToolInvokeResult result = execute_with_policies(
        ExecutionTarget::Tool, execution_policies_,
        Value::object({{"toolName", tool.name}, {"toolCallId", tool_call.id}}), context.cancellation,
        [&]() { return tool.invoke(tool_call.arguments, context); },
        [&](const RetryScheduledContext& retry) {
          if (event_bus_) {
            event_bus_->publish("retry.scheduled", ExecutionTarget::Tool,
                                retry_scheduled_tool_payload(retry, tool, tool_call),
                                context.trace_context);
          }
        });

    std::string output;
    AgentMessage message;
    if (std::holds_alternative<ToolResultEnvelope>(result)) {
      const auto& envelope = std::get<ToolResultEnvelope>(result);
      const Value value = envelope.value.value_or(Value{});
      const Value metadata = successful_tool_metadata(envelope.metadata);
      output = safe_json_stringify(value);
      if (envelope.content) {
        message = AgentMessage{.role = MessageRole::Tool,
                               .content = *envelope.content,
                               .name = tool_call.name,
                               .tool_call_id = tool_call.id,
                               .metadata = metadata};
      } else {
        message = create_tool_result_message(tool_call.id, tool_call.name, output, metadata);
      }
    } else {
      const auto& value = std::get<Value>(result);
      output = value.is_string() ? value.as_string() : safe_json_stringify(value);
      message = create_tool_result_message(tool_call.id, tool_call.name, output, Value::object({{"ok", true}}));
    }

    if (event_bus_) {
      event_bus_->publish("tool.audit", ExecutionTarget::Tool,
                          tool_audit_payload("completed", tool_call, Value::object({{"ok", true}})),
                          context.trace_context);
      const auto completed_at = now_iso8601();
      const auto duration_ms = lifecycle_started
                                   ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - lifecycle_start).count()
                                   : 0;
      auto completed_payload = tool_lifecycle_base_payload("completed", tool.name, tool_call, context,
                                                           "completedAt", completed_at);
      completed_payload["ok"] = true;
      completed_payload["result"] = tool_result_payload_value(result);
      completed_payload["output"] = output;
      completed_payload["durationMs"] = static_cast<long long>(duration_ms);
      const auto result_metadata = tool_result_metadata_payload(result);
      if (!result_metadata.is_null()) {
        completed_payload["metadata"] = result_metadata;
      }
      const auto result_content = tool_result_content_payload(result);
      if (!result_content.is_null()) {
        completed_payload["content"] = result_content;
      }
      event_bus_->publish("tool.completed", ExecutionTarget::Tool,
                          completed_payload,
                          context.trace_context);
    }
    if (hooks_.after_tool) {
      ToolHookContext hook_context;
      hook_context.target = ExecutionTarget::Tool;
      hook_context.trace_id = context.trace_context.trace_id;
      hook_context.run_id = context.trace_context.run_id;
      hook_context.workflow_run_id = context.trace_context.workflow_run_id;
      hook_context.tool_name = tool.name;
      hook_context.tool_call = tool_call_hook_value(tool_call);
      hook_context.input = tool_call.arguments;
      hook_context.result = output;
      hooks_.after_tool(hook_context);
    }
    return ToolExecutionResult{.tool_call = tool_call,
                               .ok = true,
                               .result = result,
                               .output = output,
                               .message = message};
  } catch (const std::exception& error) {
    if (context.cancellation && context.cancellation->cancelled()) {
      throw;
    }
    const std::string error_message = tool_failed_message(tool_call.name);
    const std::string output = format_tool_error_output(error.what());
    if (hooks_.on_tool_error) {
      ToolHookContext hook_context;
      hook_context.target = ExecutionTarget::Tool;
      hook_context.trace_id = context.trace_context.trace_id;
      hook_context.run_id = context.trace_context.run_id;
      hook_context.workflow_run_id = context.trace_context.workflow_run_id;
      hook_context.tool_name = tool.name;
      hook_context.tool_call = tool_call_hook_value(tool_call);
      hook_context.input = tool_call.arguments;
      hook_context.error = error_message;
      hooks_.on_tool_error(hook_context);
    }
    if (event_bus_) {
      event_bus_->publish("tool.audit", ExecutionTarget::Tool,
                          tool_audit_payload("failed", tool_call,
                                             Value::object({{"ok", false}, {"error", error_message}})),
                          context.trace_context);
      const auto failed_at = now_iso8601();
      const auto duration_ms = lifecycle_started
                                   ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - lifecycle_start).count()
                                   : 0;
      auto failed_payload = tool_lifecycle_base_payload("failed", tool.name, tool_call, context,
                                                        "failedAt", failed_at);
      failed_payload["ok"] = false;
      failed_payload["error"] = tool_error_payload(error, error_message);
      failed_payload["output"] = output;
      failed_payload["durationMs"] = static_cast<long long>(duration_ms);
      event_bus_->publish("tool.failed", ExecutionTarget::Tool,
                          failed_payload,
                          context.trace_context);
    }
    return ToolExecutionResult{
        .tool_call = tool_call,
        .ok = false,
        .error = error_message,
        .output = output,
        .message = create_tool_result_message(tool_call.id, tool_call.name, output, Value::object({{"ok", false}})),
    };
  }
}

std::vector<ToolExecutionResult> ToolExecutor::execute_all(const std::vector<ToolCall>& tool_calls,
                                                           ToolExecutionContext context) {
  std::vector<ToolExecutionResult> results;
  results.reserve(tool_calls.size());
  for (const auto& tool_call : tool_calls) {
    results.push_back(execute_tool_call(tool_call, context));
  }
  return results;
}
}  // namespace agent
