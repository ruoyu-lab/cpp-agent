#include "agent/agent.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
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

bool matches_permission_rule(const PermissionRequest& request, const ToolPermissionMatcher& matcher) {
  return matches_scalar(request.tool_name, matcher.tool_names) &&
         matches_any(request.capabilities, matcher.capabilities) &&
         matches_any(request.tags, matcher.tags) &&
         matches_scalar(request.risk_level, matcher.risk_levels) &&
         (!matcher.builtin || request.builtin == *matcher.builtin) &&
         matches_scalar(request.bundle, matcher.bundles);
}

Value tool_call_hook_value(const ToolCall& tool_call) {
  return Value::object({
      {"id", tool_call.id},
      {"name", tool_call.name},
      {"arguments", tool_call.arguments},
  });
}

}  // namespace

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
  if (overlay.session) {
    merged.session = overlay.session;
  }
  if (overlay.scratch_store) {
    merged.scratch_store = overlay.scratch_store;
  }
  if (overlay.long_term_memory) {
    merged.long_term_memory = overlay.long_term_memory;
  }
  if (overlay.knowledge_base) {
    merged.knowledge_base = overlay.knowledge_base;
  }
  if (overlay.knowledge_base_manager) {
    merged.knowledge_base_manager = overlay.knowledge_base_manager;
  }
  if (overlay.workflow_engine) {
    merged.workflow_engine = overlay.workflow_engine;
  }
  if (overlay.workflow_definition) {
    merged.workflow_definition = overlay.workflow_definition;
  }
  if (overlay.web_search_registry) {
    merged.web_search_registry = overlay.web_search_registry;
  }
  if (!overlay.default_search_provider.empty()) {
    merged.default_search_provider = overlay.default_search_provider;
  }
  if (overlay.web_fetcher) {
    merged.web_fetcher = overlay.web_fetcher;
  }
  if (overlay.web_crawler) {
    merged.web_crawler = overlay.web_crawler;
  }
  if (overlay.default_crawler_profile.is_object() && !overlay.default_crawler_profile.as_object().empty()) {
    merged.default_crawler_profile = overlay.default_crawler_profile;
  }
  if (overlay.browser_renderer) {
    merged.browser_renderer = overlay.browser_renderer;
  }
  if (overlay.artifact_lookup) {
    merged.artifact_lookup = overlay.artifact_lookup;
  }
  if (overlay.ocr_registry) {
    merged.ocr_registry = overlay.ocr_registry;
  }
  if (!overlay.default_ocr_provider.empty()) {
    merged.default_ocr_provider = overlay.default_ocr_provider;
  }
  if (overlay.document_rasterizers) {
    merged.document_rasterizers = overlay.document_rasterizers;
  }
  if (overlay.document_preprocessors) {
    merged.document_preprocessors = overlay.document_preprocessors;
  }
  if (overlay.registry) {
    merged.registry = overlay.registry;
  }
  if (overlay.hooks) {
    merged.hooks = overlay.hooks;
  }
  if (overlay.sandbox) {
    merged.sandbox = overlay.sandbox;
  }
  if (overlay.sandbox_scope) {
    merged.sandbox_scope = overlay.sandbox_scope;
  }
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
                        {"riskLevel", to_string(request.risk_level)},
                        {"bundle", request.bundle},
                        {"builtin", request.builtin}});
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

std::string format_tool_error_output(const std::string& message) {
  return Value::object({{"ok", false}, {"error", message}}).stringify(2);
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
  return ChatToolDescriptor{name, description, input_schema};
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
        return PermissionDecision{rule.decision, rule.reason};
      }
    }
    if (request.capabilities.empty()) {
      return PermissionDecision{PermissionDecisionKind::Allow, {}};
    }
    return PermissionDecision{
        config.default_decision,
        config.default_reason.empty() ? "No permission rule matched the tool request." : config.default_reason,
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
      return PermissionDecision{PermissionDecisionKind::Deny, "Denied by capability policy."};
    }
    if (has_capability(config.ask)) {
      return PermissionDecision{PermissionDecisionKind::Ask, "Capability requires approval."};
    }
    if (request.risk_level == ToolRiskLevel::High) {
      return PermissionDecision{config.high_risk_mode, "High-risk tool execution."};
    }
    if (!request.capabilities.empty() &&
        std::all_of(request.capabilities.begin(), request.capabilities.end(), [&](const std::string& capability) {
          return contains_value(config.allow, capability);
        })) {
      return PermissionDecision{PermissionDecisionKind::Allow, {}};
    }
    if (request.capabilities.empty()) {
      return PermissionDecision{PermissionDecisionKind::Allow, {}};
    }
    return PermissionDecision{PermissionDecisionKind::Deny, "Capability is not allowed."};
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
      return PermissionDecision{config.default_decision, "CLI approval streams are not available."};
    }

    const std::string prompt = config.prompt_formatter
                                   ? config.prompt_formatter(request, decision)
                                   : "Allow tool \"" + request.tool_name + "\" from bundle \"" +
                                         (request.bundle.empty() ? "unscoped" : request.bundle) + "\"? [y/N] ";
    (*output) << prompt;
    output->flush();

    std::string answer;
    if (!std::getline(*input, answer)) {
      return PermissionDecision{config.default_decision, decision.reason};
    }
    std::transform(answer.begin(), answer.end(), answer.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    answer = trim_copy(std::move(answer));
    if (answer == "y" || answer == "yes") {
      return PermissionDecision{PermissionDecisionKind::Allow, {}};
    }
    return PermissionDecision{config.default_decision, decision.reason};
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
  // Expose the registry to tools that need it (e.g. tool.search / tool.describe).
  if (!context.service_refs.registry) {
    context.service_refs.registry = &registry_;
  }
  if (!context.service_refs.hooks) {
    context.service_refs.hooks = &hooks_;
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

  if (!tool_allowed_by_active_skills(tool_call, context) && permission_policy_) {
    PermissionRequest request;
    request.tool_name = tool.name;
    request.capabilities = tool.capabilities;
    request.risk_level = tool.risk_level;
    request.tags = tool.tags;
    request.bundle = tool.bundle;
    request.builtin = tool.builtin;
    request.tool_call = tool_call;
    request.context = context;
    if (event_bus_) {
      event_bus_->publish("tool.audit", ExecutionTarget::Tool,
                          tool_audit_payload(
                              "requested", tool_call,
                              Value::object({{"riskLevel", to_string(tool.risk_level)},
                                            {"capabilities", strings_to_value(tool.capabilities)},
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
    PermissionDecision decision = permission_policy_(request);
    if (decision.decision == PermissionDecisionKind::Ask) {
      if (event_bus_) {
        event_bus_->publish("permission.approval_requested", ExecutionTarget::Permission,
                            permission_event_payload(request, decision), context.trace_context);
      }
      decision = approval_handler_ ? approval_handler_(request, decision)
                                   : PermissionDecision{PermissionDecisionKind::Deny,
                                                        "Approval handler is not configured."};
    }
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
        event_bus_->publish("permission.denied", ExecutionTarget::Permission,
                            permission_event_payload(request, PermissionDecision{decision.decision, reason}),
                            context.trace_context);
        event_bus_->publish("tool.audit", ExecutionTarget::Tool,
                            tool_audit_payload("denied", tool_call,
                                               Value::object({{"ok", false}, {"error", error_message},
                                                             {"reason", reason}})),
                            context.trace_context);
      }
      const std::string output = format_tool_error_output(error_message);
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
                          permission_event_payload(request, decision), context.trace_context);
      event_bus_->publish("permission.checked", ExecutionTarget::Permission,
                          Value::object({{"toolName", tool.name}, {"decision", "allow"}}),
                          context.trace_context);
      event_bus_->publish("tool.audit", ExecutionTarget::Tool,
                          tool_audit_payload("approved", tool_call, Value::object({{"ok", true}})),
                          context.trace_context);
    }
  }

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
      event_bus_->publish("tool.started", ExecutionTarget::Tool,
                          Value::object({{"toolName", tool.name}, {"toolCallId", tool_call.id}}),
                          context.trace_context);
    }
    context.tool_call = tool_call;
    // Sandbox contract: refuse to dispatch if the installed sandbox cannot
    // satisfy the tool's policy. `enforce_sandbox_contract` returns the
    // effective request (preferred when supported, else required) and emits
    // `sandbox.entered` / `sandbox.violated`. On rejection it throws
    // `SandboxContractError`, which bubbles up through the executor's normal
    // error path and is surfaced as a tool failure.
    auto effective_request =
        enforce_sandbox_contract(tool.sandbox_policy, context.service_refs.sandbox, event_bus_);
    std::unique_ptr<SandboxScope> sandbox_scope;
    if (context.service_refs.sandbox) {
      sandbox_scope = context.service_refs.sandbox->enter(effective_request);
    }
    context.service_refs.sandbox_scope = sandbox_scope.get();
    // RAII: clear the borrowed scope pointer on every path (success, throw,
    // return), and emit `sandbox.exited` if a scope was actually entered.
    struct SandboxScopeGuard {
      ToolExecutionContext* context_ptr;
      EventBus* event_bus;
      bool entered;
      std::chrono::steady_clock::time_point start;
      const TraceContext* trace;
      ~SandboxScopeGuard() {
        context_ptr->service_refs.sandbox_scope = nullptr;
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
      event_bus_->publish("tool.completed", ExecutionTarget::Tool,
                          Value::object({{"toolName", tool.name}, {"toolCallId", tool_call.id}}),
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
      event_bus_->publish("tool.failed", ExecutionTarget::Tool,
                          Value::object({{"toolName", tool.name}, {"toolCallId", tool_call.id},
                                         {"error", error_message}}),
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
