#include "agent/agent.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>

namespace agent {

namespace {

std::string workflow_edge_state_id(const WorkflowEdge& edge, std::size_t index) {
  return edge.id.empty() ? edge.from + "->" + edge.to + ":" + std::to_string(index + 1) : edge.id;
}

void apply_builder_node_options(WorkflowNode& node,
                                std::map<std::string, WorkflowValue> inputs,
                                std::string wait_for,
                                std::size_t max_visits) {
  node.inputs = std::move(inputs);
  node.wait_for = wait_for.empty() ? "all" : std::move(wait_for);
  node.max_visits = std::max<std::size_t>(1, max_visits);
}

std::map<std::string, Value> map_from_object(const Value& value) {
  std::map<std::string, Value> result;
  for (const auto& [key, child] : value.as_object()) {
    result[key] = child;
  }
  return result;
}

Value merge_object_values(const Value& left, const Value& right) {
  if (!left.is_object() || !right.is_object()) {
    return right;
  }
  Value::Object merged = left.as_object();
  for (const auto& [key, value] : right.as_object()) {
    merged[key] = value;
  }
  return Value(std::move(merged));
}

std::string workflow_child_agent_input_text(const Value& input) {
  if (input.at("prompt").is_string()) {
    return input.at("prompt").as_string();
  }
  return input.is_string() ? input.as_string() : safe_json_stringify(input);
}

Value workflow_agent_runner_result_to_value(const AgentRunnerRunResult& result) {
  Value::Array messages;
  for (const auto& message : result.messages) {
    messages.push_back(agent_message_to_value(message));
  }
  return Value::object({
      {"sessionId", result.session_id},
      {"iterationCount", result.iteration_count},
      {"text", result.text},
      {"terminationReason", to_string(result.termination_reason)},
      {"response", Value::object({
                       {"provider", result.response.provider},
                       {"model", result.response.model},
                       {"text", result.response.text},
                       {"finishReason", result.response.finish_reason},
                       {"raw", result.response.raw},
                   })},
      {"messages", Value(std::move(messages))},
  });
}

Value workflow_retry_scheduled_payload(const RetryScheduledContext& retry, const WorkflowNode& node) {
  Value payload = Value::object({
      {"attempt", retry.attempt},
      {"delayMs", retry.delay_ms},
      {"error", retry.error},
      {"target", to_string(retry.target)},
      {"nodeId", node.id},
      {"nodeType", node.type},
  });
  if (retry.metadata.is_object() && !retry.metadata.as_object().empty()) {
    payload["metadata"] = retry.metadata;
  }
  return payload;
}

std::string webhook_url_from_input(const Value& input) {
  const auto& url = input.at("url");
  if (url.is_string()) {
    return url.as_string();
  }
  if (url.is_number() || url.is_bool()) {
    return url.stringify(0);
  }
  return {};
}

}  // namespace

WorkflowWaitSignal create_workflow_wait_signal(std::string wait_type, std::string prompt, Value metadata) {
  return WorkflowWaitSignal{std::move(wait_type), std::move(prompt),
                            metadata.is_object() ? std::move(metadata) : Value::object({})};
}

WorkflowNodeHandler create_human_workflow_node_handler(HumanWorkflowResponseProvider provider) {
  if (!provider) {
    throw ConfigurationError("Human workflow node handler requires a response provider.");
  }
  return WorkflowNodeHandler{
      .type = "human",
      .execute = [provider = std::move(provider)](const WorkflowNode& node,
                                                  WorkflowNodeHandlerContext& context) -> WorkflowNodeHandlerResult {
        if (!context.state) {
          throw ConfigurationError("Human workflow node handler requires workflow state.");
        }
        return provider(HumanWorkflowNodeProviderParams{node, context.input, *context.state});
      },
  };
}

WorkflowNodeHandler create_webhook_workflow_node_handler(HttpTransport transport) {
  if (!transport) {
    throw ConfigurationError("Webhook workflow node handler requires an HttpTransport.");
  }
  return WorkflowNodeHandler{
      .type = "webhook",
      .execute = [transport = std::move(transport)](const WorkflowNode& node,
                                                    WorkflowNodeHandlerContext& context) -> WorkflowNodeHandlerResult {
        const auto url = node.url.empty() ? webhook_url_from_input(context.input) : node.url;
        if (url.empty()) {
          throw ConfigurationError("Webhook workflow node requires a url.");
        }

        const auto method = node.method.empty() ? std::string("POST") : node.method;
        HttpRequest request;
        request.url = url;
        request.method = method;
        request.headers["content-type"] = "application/json";
        if (method != "GET") {
          request.body = context.input.stringify(0);
        }
        const auto response = transport(request);
        return Value::object({{"status", response.status}, {"body", response.body}});
      },
  };
}

WorkflowNodeRegistry::WorkflowNodeRegistry(std::vector<WorkflowNodeHandler> handlers) {
  for (auto& handler : handlers) {
    register_handler(std::move(handler));
  }
}

WorkflowNodeRegistry::WorkflowNodeRegistry(const WorkflowNodeRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  handlers_ = other.handlers_;
}

WorkflowNodeRegistry& WorkflowNodeRegistry::operator=(const WorkflowNodeRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  handlers_ = other.handlers_;
  return *this;
}

WorkflowNodeRegistry::WorkflowNodeRegistry(WorkflowNodeRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  handlers_ = std::move(other.handlers_);
}

WorkflowNodeRegistry& WorkflowNodeRegistry::operator=(WorkflowNodeRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  handlers_ = std::move(other.handlers_);
  return *this;
}

WorkflowNodeHandler& WorkflowNodeRegistry::register_handler(WorkflowNodeHandler handler) {
  if (handler.type.empty()) {
    throw ConfigurationError("Workflow node handler requires a type.");
  }
  if (!handler.execute) {
    throw ConfigurationError("Workflow node handler \"" + handler.type + "\" requires execute.");
  }
  const auto type = handler.type;
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_[type] = std::move(handler);
  return handlers_.at(type);
}

const WorkflowNodeHandler* WorkflowNodeRegistry::get(const std::string& type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = handlers_.find(type);
  return found == handlers_.end() ? nullptr : &found->second;
}

std::optional<WorkflowNodeHandler> WorkflowNodeRegistry::find(const std::string& type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = handlers_.find(type);
  return found == handlers_.end() ? std::nullopt : std::optional<WorkflowNodeHandler>(found->second);
}

std::vector<WorkflowNodeHandler> WorkflowNodeRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<WorkflowNodeHandler> handlers;
  handlers.reserve(handlers_.size());
  for (const auto& [_, handler] : handlers_) {
    handlers.push_back(handler);
  }
  return handlers;
}

WorkflowAgentRegistry::WorkflowAgentRegistry(std::vector<WorkflowAgentDefinition> agents) {
  for (auto& agent : agents) {
    register_agent(std::move(agent));
  }
}

WorkflowAgentRegistry::WorkflowAgentRegistry(const WorkflowAgentRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  agents_ = other.agents_;
}

WorkflowAgentRegistry& WorkflowAgentRegistry::operator=(const WorkflowAgentRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  agents_ = other.agents_;
  return *this;
}

WorkflowAgentRegistry::WorkflowAgentRegistry(WorkflowAgentRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  agents_ = std::move(other.agents_);
}

WorkflowAgentRegistry& WorkflowAgentRegistry::operator=(WorkflowAgentRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  agents_ = std::move(other.agents_);
  return *this;
}

WorkflowAgentDefinition& WorkflowAgentRegistry::register_agent(WorkflowAgentDefinition agent) {
  if (agent.id.empty()) {
    throw ConfigurationError("Workflow agent definition requires id.");
  }
  if (!agent.run && !agent.runner) {
    throw ConfigurationError("Workflow agent \"" + agent.id + "\" requires a run handler or AgentRunner.");
  }
  const auto id = agent.id;
  std::lock_guard<std::mutex> lock(mutex_);
  agents_[id] = std::move(agent);
  return agents_.at(id);
}

WorkflowAgentDefinition& WorkflowAgentRegistry::register_runner(std::string id,
                                                                std::shared_ptr<AgentRunner> runner,
                                                                std::string description) {
  return register_agent(WorkflowAgentDefinition{
      .id = std::move(id),
      .runner = std::move(runner),
      .description = std::move(description),
  });
}

const WorkflowAgentDefinition* WorkflowAgentRegistry::get(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = agents_.find(id);
  return found == agents_.end() ? nullptr : &found->second;
}

std::optional<WorkflowAgentDefinition> WorkflowAgentRegistry::find(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = agents_.find(id);
  return found == agents_.end() ? std::nullopt : std::optional<WorkflowAgentDefinition>(found->second);
}

std::string to_string(WorkflowStatus status) {
  switch (status) {
    case WorkflowStatus::Pending:
      return "pending";
    case WorkflowStatus::Running:
      return "running";
    case WorkflowStatus::Waiting:
      return "waiting";
    case WorkflowStatus::Completed:
      return "completed";
    case WorkflowStatus::Failed:
      return "failed";
    case WorkflowStatus::Skipped:
      return "skipped";
  }
  return "pending";
}

WorkflowStatus workflow_status_from_string(const std::string& status) {
  if (status == "running") return WorkflowStatus::Running;
  if (status == "waiting") return WorkflowStatus::Waiting;
  if (status == "completed") return WorkflowStatus::Completed;
  if (status == "failed") return WorkflowStatus::Failed;
  if (status == "skipped") return WorkflowStatus::Skipped;
  return WorkflowStatus::Pending;
}

WorkflowValue workflow_literal(Value value) {
  WorkflowValue workflow_value;
  workflow_value.kind = WorkflowValueKind::Literal;
  workflow_value.value = std::move(value);
  return workflow_value;
}

WorkflowValue workflow_ref(std::string path) {
  WorkflowValue value;
  value.kind = WorkflowValueKind::Ref;
  value.path = std::move(path);
  return value;
}

WorkflowValue workflow_template(std::string templ) {
  WorkflowValue value;
  value.kind = WorkflowValueKind::Template;
  value.templ = std::move(templ);
  return value;
}

WorkflowCondition workflow_truthy(WorkflowValue value) {
  WorkflowCondition condition;
  condition.kind = WorkflowConditionKind::Truthy;
  condition.left = std::move(value);
  return condition;
}

WorkflowCondition workflow_equals(WorkflowValue left, WorkflowValue right) {
  WorkflowCondition condition;
  condition.kind = WorkflowConditionKind::Equals;
  condition.left = std::move(left);
  condition.right = std::move(right);
  return condition;
}

WorkflowCondition workflow_not_empty(WorkflowValue value) {
  WorkflowCondition condition;
  condition.kind = WorkflowConditionKind::NotEmpty;
  condition.left = std::move(value);
  return condition;
}

WorkflowNodeRegistry create_default_workflow_node_registry() {
  return WorkflowNodeRegistry({
      WorkflowNodeHandler{
          .type = "start",
          .execute = [](const WorkflowNode&, WorkflowNodeHandlerContext& context) -> WorkflowNodeHandlerResult {
            return context.state ? context.state->input : Value{};
          },
      },
      WorkflowNodeHandler{
          .type = "transform",
          .execute = [](const WorkflowNode&, WorkflowNodeHandlerContext& context) -> WorkflowNodeHandlerResult {
            return context.input;
          },
      },
      WorkflowNodeHandler{
          .type = "join",
          .execute = [](const WorkflowNode&, WorkflowNodeHandlerContext& context) -> WorkflowNodeHandlerResult {
            return context.input;
          },
      },
      WorkflowNodeHandler{
          .type = "condition",
          .execute = [](const WorkflowNode& node, WorkflowNodeHandlerContext& context) -> WorkflowNodeHandlerResult {
            return Value::object({{"passed", node.has_condition && context.evaluate_condition(node.condition)}});
          },
      },
      WorkflowNodeHandler{
          .type = "tool",
          .execute = [](const WorkflowNode& node, WorkflowNodeHandlerContext& context) -> WorkflowNodeHandlerResult {
            return context.run_tool(node.tool_name, context.input);
          },
      },
      WorkflowNodeHandler{
          .type = "agent",
          .execute = [](const WorkflowNode& node, WorkflowNodeHandlerContext& context) -> WorkflowNodeHandlerResult {
            return Value::object({{"result", context.run_child_agent(node.agent_id, context.input)}});
          },
      },
      WorkflowNodeHandler{
          .type = "human-wait",
          .execute = [](const WorkflowNode& node, WorkflowNodeHandlerContext& context) -> WorkflowNodeHandlerResult {
            auto response = context.consume_signal("human", node.id);
            if (response) {
              return Value::object({{"key", node.response_key.empty() ? "response" : node.response_key},
                                    {"value", *response}});
            }
            return context.wait_for_signal("human", node.prompt, node.metadata);
          },
      },
      WorkflowNodeHandler{
          .type = "webhook-wait",
          .execute = [](const WorkflowNode& node, WorkflowNodeHandlerContext& context) -> WorkflowNodeHandlerResult {
            auto payload = context.consume_signal("webhook", node.id);
            if (payload) {
              return Value::object({{"key", node.event_key.empty() ? "payload" : node.event_key},
                                    {"value", *payload}});
            }
            return context.wait_for_signal("webhook", node.prompt, node.metadata);
          },
      },
      WorkflowNodeHandler{
          .type = "artifact",
          .execute = [](const WorkflowNode& node, WorkflowNodeHandlerContext& context) -> WorkflowNodeHandlerResult {
            const auto mode = node.artifact_mode.empty() ? std::string("get") : node.artifact_mode;
            if (mode == "get") {
              return Value::object({{node.output_key.empty() ? "value" : node.output_key,
                                    context.get_artifact(node.artifact_key)}});
            }
            const auto value = node.has_artifact_value ? context.resolve_value(node.artifact_value) : Value{};
            if (mode == "set") {
              context.set_artifact(node.artifact_key, value);
              return Value::object({{"key", node.artifact_key}, {"value", value}});
            }
            const auto merged = merge_object_values(context.get_artifact(node.artifact_key), value);
            context.set_artifact(node.artifact_key, merged);
            return Value::object({{"key", node.artifact_key}, {"value", merged}});
          },
      },
      WorkflowNodeHandler{
          .type = "router",
          .execute = [](const WorkflowNode& node, WorkflowNodeHandlerContext& context) -> WorkflowNodeHandlerResult {
            std::string route = node.default_route;
            for (const auto& candidate : node.routes) {
              if (!candidate.has_condition || context.evaluate_condition(candidate.condition)) {
                route = candidate.key;
                break;
              }
            }
            return Value::object({{"route", route.empty() ? Value() : Value(route)}});
          },
      },
      WorkflowNodeHandler{
          .type = "end",
          .execute = [](const WorkflowNode& node, WorkflowNodeHandlerContext& context) -> WorkflowNodeHandlerResult {
            return node.has_result ? context.resolve_value(node.result) : context.input;
          },
      },
  });
}

Value workflow_value_to_value(const WorkflowValue& value) {
  if (value.kind == WorkflowValueKind::Ref) {
    return Value::object({{"kind", "ref"}, {"path", value.path}});
  }
  if (value.kind == WorkflowValueKind::Template) {
    return Value::object({{"kind", "template"}, {"template", value.templ}});
  }
  return Value::object({{"kind", "literal"}, {"value", value.value}});
}

WorkflowValue workflow_value_from_value(const Value& value) {
  if (!value.is_object()) {
    return workflow_literal(value);
  }
  const std::string kind = value.at("kind").as_string("literal");
  if (kind == "ref") {
    return workflow_ref(value.at("path").as_string());
  }
  if (kind == "template") {
    return workflow_template(value.at("template").as_string(value.at("templ").as_string()));
  }
  return workflow_literal(value.at("value"));
}

Value workflow_condition_to_value(const WorkflowCondition& condition) {
  switch (condition.kind) {
    case WorkflowConditionKind::Equals:
      return Value::object({{"kind", "equals"},
                            {"left", workflow_value_to_value(condition.left)},
                            {"right", workflow_value_to_value(condition.right)}});
    case WorkflowConditionKind::NotEmpty:
      return Value::object({{"kind", "notEmpty"}, {"value", workflow_value_to_value(condition.left)}});
    case WorkflowConditionKind::Truthy:
      return Value::object({{"kind", "truthy"}, {"value", workflow_value_to_value(condition.left)}});
  }
  return Value::object({{"kind", "truthy"}, {"value", workflow_value_to_value(condition.left)}});
}

WorkflowCondition workflow_condition_from_value(const Value& value) {
  const std::string kind = value.at("kind").as_string("truthy");
  if (kind == "equals") {
    return workflow_equals(workflow_value_from_value(value.at("left")),
                           workflow_value_from_value(value.at("right")));
  }
  if (kind == "notEmpty") {
    return workflow_not_empty(workflow_value_from_value(value.at("value")));
  }
  return workflow_truthy(workflow_value_from_value(value.at("value")));
}

Value workflow_node_route_to_value(const WorkflowNodeRoute& route) {
  Value::Object object{{"key", route.key}};
  if (route.has_condition) {
    object["condition"] = workflow_condition_to_value(route.condition);
  }
  return Value(std::move(object));
}

WorkflowNodeRoute workflow_node_route_from_value(const Value& value) {
  WorkflowNodeRoute route;
  route.key = value.at("key").as_string();
  if (value.at("condition").is_object()) {
    route.condition = workflow_condition_from_value(value.at("condition"));
    route.has_condition = true;
  }
  return route;
}

Value workflow_node_wait_state_to_value(const WorkflowNodeWaitState& waiting) {
  return Value::object({{"kind", waiting.kind},
                        {"prompt", waiting.prompt},
                        {"metadata", waiting.metadata.is_object() ? waiting.metadata : Value::object({})}});
}

WorkflowNodeWaitState workflow_node_wait_state_from_value(const Value& value) {
  WorkflowNodeWaitState waiting;
  waiting.kind = value.at("kind").as_string();
  waiting.prompt = value.at("prompt").as_string();
  waiting.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return waiting;
}

Value workflow_node_to_value(const WorkflowNode& node) {
  Value::Object inputs;
  for (const auto& [key, input] : node.inputs) {
    inputs[key] = workflow_value_to_value(input);
  }
  Value::Array routes;
  for (const auto& route : node.routes) {
    routes.push_back(workflow_node_route_to_value(route));
  }

  Value::Object object{{"id", node.id},
                       {"type", node.type},
                       {"waitFor", node.wait_for},
                       {"inputs", Value(inputs)},
                       {"maxVisits", node.max_visits}};
  if (!node.tool_name.empty()) {
    object["toolName"] = node.tool_name;
  }
  if (!node.agent_id.empty()) {
    object["agentId"] = node.agent_id;
  }
  if (node.has_condition) {
    object["condition"] = workflow_condition_to_value(node.condition);
  }
  if (node.has_result) {
    object["result"] = workflow_value_to_value(node.result);
  }
  if (!node.prompt.empty()) {
    object["prompt"] = node.prompt;
  }
  if (!node.message.empty()) {
    object["message"] = node.message;
  }
  if (!node.response_key.empty() && node.response_key != "response") {
    object["responseKey"] = node.response_key;
  }
  if (!node.event_key.empty() && node.event_key != "payload") {
    object["eventKey"] = node.event_key;
  }
  if (!node.url.empty()) {
    object["url"] = node.url;
  }
  if (!node.method.empty()) {
    object["method"] = node.method;
  }
  if (node.metadata.is_object() && !node.metadata.as_object().empty()) {
    object["metadata"] = node.metadata;
  }
  if (!node.artifact_key.empty()) {
    object["key"] = node.artifact_key;
    object["mode"] = node.artifact_mode;
    object["outputKey"] = node.output_key;
  }
  if (node.has_artifact_value) {
    object["value"] = workflow_value_to_value(node.artifact_value);
  }
  if (!routes.empty()) {
    object["routes"] = Value(routes);
  }
  if (!node.default_route.empty()) {
    object["defaultRoute"] = node.default_route;
  }
  return Value(object);
}

WorkflowNode workflow_node_from_value(const Value& value) {
  WorkflowNode node;
  node.id = value.at("id").as_string();
  node.type = value.at("type").as_string();
  node.wait_for = value.at("waitFor").as_string(value.at("wait_for").as_string("all"));
  node.max_visits = static_cast<std::size_t>(std::max<long long>(1, value.at("maxVisits").as_integer(1)));
  node.tool_name = value.at("toolName").as_string(value.at("tool_name").as_string());
  node.agent_id = value.at("agentId").as_string(value.at("agent_id").as_string());
  node.prompt = value.at("prompt").as_string();
  node.message = value.at("message").as_string();
  node.response_key = value.at("responseKey").as_string(value.at("response_key").as_string("response"));
  node.event_key = value.at("eventKey").as_string(value.at("event_key").as_string("payload"));
  node.url = value.at("url").as_string();
  node.method = value.at("method").as_string();
  node.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  node.artifact_mode = value.at("mode").as_string(value.at("artifactMode").as_string("get"));
  node.artifact_key = value.at("key").as_string(value.at("artifactKey").as_string());
  node.output_key = value.at("outputKey").as_string(value.at("output_key").as_string("value"));
  if (value.at("inputs").is_object()) {
    for (const auto& [key, input] : value.at("inputs").as_object()) {
      node.inputs[key] = workflow_value_from_value(input);
    }
  }
  if (value.at("condition").is_object()) {
    node.condition = workflow_condition_from_value(value.at("condition"));
    node.has_condition = true;
  }
  if (value.at("result").is_object()) {
    node.result = workflow_value_from_value(value.at("result"));
    node.has_result = true;
  }
  if (value.at("value").is_object()) {
    node.artifact_value = workflow_value_from_value(value.at("value"));
    node.has_artifact_value = true;
  }
  for (const auto& route : value.at("routes").as_array()) {
    node.routes.push_back(workflow_node_route_from_value(route));
  }
  node.default_route = value.at("defaultRoute").as_string(value.at("default_route").as_string());
  return node;
}

Value workflow_edge_to_value(const WorkflowEdge& edge) {
  Value::Object object{{"id", edge.id}, {"from", edge.from}, {"to", edge.to}};
  if (edge.has_condition) {
    object["condition"] = workflow_condition_to_value(edge.condition);
  }
  return Value(object);
}

WorkflowEdge workflow_edge_from_value(const Value& value) {
  WorkflowEdge edge;
  edge.from = value.at("from").as_string();
  edge.to = value.at("to").as_string();
  edge.id = value.at("id").as_string();
  if (edge.id.empty()) {
    edge.id = edge.from + "->" + edge.to;
  }
  if (value.at("condition").is_object()) {
    edge.condition = workflow_condition_from_value(value.at("condition"));
    edge.has_condition = true;
  }
  return edge;
}

Value workflow_definition_to_value(const WorkflowDefinition& definition) {
  Value::Array nodes;
  for (const auto& node : definition.nodes) {
    nodes.push_back(workflow_node_to_value(node));
  }
  Value::Array edges;
  for (const auto& edge : definition.edges) {
    edges.push_back(workflow_edge_to_value(edge));
  }
  return Value::object({{"id", definition.id},
                        {"title", definition.title},
                        {"nodes", Value(nodes)},
                        {"edges", Value(edges)}});
}

WorkflowDefinition workflow_definition_from_value(const Value& value) {
  WorkflowDefinition definition;
  definition.id = value.at("id").as_string();
  definition.title = value.at("title").as_string();
  for (const auto& item : value.at("nodes").as_array()) {
    definition.nodes.push_back(workflow_node_from_value(item));
  }
  std::size_t index = 0;
  for (const auto& item : value.at("edges").as_array()) {
    auto edge = workflow_edge_from_value(item);
    if (edge.id == edge.from + "->" + edge.to) {
      edge.id += ":" + std::to_string(index + 1);
    }
    definition.edges.push_back(std::move(edge));
    index += 1;
  }
  return definition;
}

Value workflow_node_state_to_value(const WorkflowNodeState& state) {
  return Value::object({{"nodeId", state.node_id},
                        {"status", to_string(state.status)},
                        {"visits", state.visits},
                        {"input", state.input},
                        {"output", state.output},
                        {"error", state.error},
                        {"queuedAt", state.queued_at},
                        {"startedAt", state.started_at},
                        {"completedAt", state.completed_at},
                        {"waiting", state.waiting ? workflow_node_wait_state_to_value(*state.waiting) : Value()}});
}

WorkflowNodeState workflow_node_state_from_value(const Value& value) {
  WorkflowNodeState state;
  state.node_id = value.at("nodeId").as_string(value.at("node_id").as_string());
  state.status = workflow_status_from_string(value.at("status").as_string());
  state.visits = static_cast<std::size_t>(std::max<long long>(0, value.at("visits").as_integer()));
  state.input = value.at("input");
  state.output = value.at("output");
  state.error = value.at("error").as_string();
  state.queued_at = value.at("queuedAt").as_string(value.at("queued_at").as_string());
  state.started_at = value.at("startedAt").as_string(value.at("started_at").as_string());
  state.completed_at = value.at("completedAt").as_string(value.at("completed_at").as_string());
  if (value.at("waiting").is_object()) {
    state.waiting = workflow_node_wait_state_from_value(value.at("waiting"));
  }
  return state;
}

Value workflow_checkpoint_to_value(const WorkflowCheckpoint& checkpoint) {
  return Value::object({{"id", checkpoint.id},
                        {"at", checkpoint.at},
                        {"workflowRunId", checkpoint.workflow_run_id},
                        {"type", checkpoint.type},
                        {"workflowId", checkpoint.workflow_id},
                        {"nodeId", checkpoint.node_id.empty() ? Value() : Value(checkpoint.node_id)},
                        {"edgeId", checkpoint.edge_id.empty() ? Value() : Value(checkpoint.edge_id)},
                        {"payload", checkpoint.payload}});
}

WorkflowCheckpoint workflow_checkpoint_from_value(const Value& value) {
  WorkflowCheckpoint checkpoint;
  checkpoint.id = value.at("id").as_string();
  checkpoint.at = value.at("at").as_string();
  checkpoint.workflow_run_id = value.at("workflowRunId").as_string(value.at("workflow_run_id").as_string());
  checkpoint.type = value.at("type").as_string();
  checkpoint.workflow_id = value.at("workflowId").as_string(value.at("workflow_id").as_string());
  checkpoint.node_id = value.at("nodeId").as_string(value.at("node_id").as_string());
  checkpoint.edge_id = value.at("edgeId").as_string(value.at("edge_id").as_string());
  checkpoint.payload = value.at("payload").is_object() ? value.at("payload") : Value::object({});
  return checkpoint;
}

Value workflow_run_state_to_value(const WorkflowRunState& state) {
  Value::Object node_states;
  for (const auto& [node_id, node_state] : state.node_states) {
    node_states[node_id] = workflow_node_state_to_value(node_state);
  }
  Value::Object edge_states;
  for (const auto& [edge_id, status] : state.edge_states) {
    edge_states[edge_id] = Value::object({{"edgeId", edge_id}, {"status", status}});
  }
  Value::Object human_responses;
  for (const auto& [node_id, payload] : state.human_responses) {
    human_responses[node_id] = payload;
  }
  Value::Object webhook_payloads;
  for (const auto& [node_id, payload] : state.webhook_payloads) {
    webhook_payloads[node_id] = payload;
  }
  Value::Array checkpoints;
  for (const auto& checkpoint : state.checkpoints) {
    checkpoints.push_back(workflow_checkpoint_to_value(checkpoint));
  }
  return Value::object({{"workflowRunId", state.workflow_run_id},
                        {"workflowId", state.workflow_id},
                        {"definitionSnapshot", workflow_definition_to_value(state.definition_snapshot)},
                        {"status", to_string(state.status)},
                        {"input", state.input},
                        {"output", state.output},
                        {"context", state.context},
                        {"signals", Value::object({{"humanResponses", Value(human_responses)},
                                                   {"webhookPayloads", Value(webhook_payloads)}})},
                        {"nodeStates", Value(node_states)},
                        {"edgeStates", Value(edge_states)},
                        {"checkpoints", Value(checkpoints)},
                        {"eventLog", Value(Value::Array(state.event_log.begin(), state.event_log.end()))},
                        {"startedAt", state.started_at},
                        {"updatedAt", state.updated_at},
                        {"completedAt", state.completed_at}});
}

WorkflowRunState workflow_run_state_from_value(const Value& value) {
  WorkflowRunState state;
  state.workflow_run_id = value.at("workflowRunId").as_string(value.at("workflow_run_id").as_string());
  state.workflow_id = value.at("workflowId").as_string(value.at("workflow_id").as_string());
  state.definition_snapshot = workflow_definition_from_value(value.at("definitionSnapshot"));
  state.status = workflow_status_from_string(value.at("status").as_string());
  state.input = value.at("input");
  state.output = value.at("output");
  state.context = value.at("context").is_object() ? value.at("context") : Value::object({});
  state.human_responses = map_from_object(value.at("signals").at("humanResponses"));
  state.webhook_payloads = map_from_object(value.at("signals").at("webhookPayloads"));
  for (const auto& [node_id, node_state] : value.at("nodeStates").as_object()) {
    state.node_states[node_id] = workflow_node_state_from_value(node_state);
  }
  for (const auto& [edge_id, edge_state] : value.at("edgeStates").as_object()) {
    state.edge_states[edge_id] = edge_state.is_object() ? edge_state.at("status").as_string("pending")
                                                        : edge_state.as_string("pending");
  }
  for (const auto& checkpoint : value.at("checkpoints").as_array()) {
    state.checkpoints.push_back(workflow_checkpoint_from_value(checkpoint));
  }
  for (const auto& event : value.at("eventLog").as_array()) {
    state.event_log.push_back(event);
  }
  state.started_at = value.at("startedAt").as_string(value.at("started_at").as_string());
  state.updated_at = value.at("updatedAt").as_string(value.at("updated_at").as_string());
  state.completed_at = value.at("completedAt").as_string(value.at("completed_at").as_string());
  return state;
}

Value workflow_execution_result_to_value(const WorkflowExecutionResult& result) {
  return Value::object({{"workflowRunId", result.workflow_run_id},
                        {"status", to_string(result.status)},
                        {"output", result.output},
                        {"state", workflow_run_state_to_value(result.state)}});
}

WorkflowRunState initialize_workflow_run_state(const WorkflowDefinition& definition,
                                               std::string workflow_run_id,
                                               Value input,
                                               Value context) {
  WorkflowRunState state;
  state.workflow_run_id = workflow_run_id.empty() ? generate_uuid() : std::move(workflow_run_id);
  state.workflow_id = definition.id;
  state.definition_snapshot = definition;
  state.status = WorkflowStatus::Pending;
  state.input = std::move(input);
  state.context = context.is_object() ? std::move(context) : Value::object({});
  state.started_at = now_iso8601();
  state.updated_at = state.started_at;
  for (const auto& node : definition.nodes) {
    state.node_states[node.id] = WorkflowNodeState{.node_id = node.id};
  }
  for (std::size_t index = 0; index < definition.edges.size(); ++index) {
    state.edge_states[workflow_edge_state_id(definition.edges[index], index)] = "pending";
  }
  return state;
}

WorkflowRunState InMemoryWorkflowStore::create_run(const WorkflowDefinition& definition,
                                                   Value input,
                                                   Value context) {
  auto state = initialize_workflow_run_state(definition, generate_uuid(), std::move(input), std::move(context));
  std::lock_guard<std::mutex> lock(mutex_);
  runs_[state.workflow_run_id] = state;
  return state;
}

std::optional<WorkflowRunState> InMemoryWorkflowStore::get_run(const std::string& workflow_run_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = runs_.find(workflow_run_id);
  if (found == runs_.end()) {
    return std::nullopt;
  }
  return found->second;
}

void InMemoryWorkflowStore::save_run(const WorkflowRunState& state) {
  std::lock_guard<std::mutex> lock(mutex_);
  runs_[state.workflow_run_id] = state;
}

FileWorkflowStore::FileWorkflowStore(std::filesystem::path base_dir) : base_dir_(std::move(base_dir)) {}

FileWorkflowStore::FileWorkflowStore(FileWorkflowStoreConfig config)
    : FileWorkflowStore(std::move(config.base_dir)) {}

std::filesystem::path FileWorkflowStore::file_path(const std::string& workflow_run_id) const {
  return base_dir_ / (encode_uri_component(workflow_run_id) + ".json");
}

WorkflowRunState FileWorkflowStore::create_run(const WorkflowDefinition& definition,
                                               Value input,
                                               Value context) {
  auto state = initialize_workflow_run_state(definition, generate_uuid(), std::move(input), std::move(context));
  save_run(state);
  return state;
}

std::optional<WorkflowRunState> FileWorkflowStore::get_run(const std::string& workflow_run_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto path = file_path(workflow_run_id);
  if (!std::filesystem::exists(path)) {
    const auto legacy_path = base_dir_ / (sanitize_segment(workflow_run_id) + ".json");
    if (std::filesystem::exists(legacy_path)) {
      path = legacy_path;
    }
  }
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }
  return workflow_run_state_from_value(read_json_file(path));
}

void FileWorkflowStore::save_run(const WorkflowRunState& state) {
  std::lock_guard<std::mutex> lock(mutex_);
  write_json_file(file_path(state.workflow_run_id), workflow_run_state_to_value(state));
}

WorkflowBuilder::WorkflowBuilder(std::string id, std::string title) {
  definition_.id = std::move(id);
  definition_.title = std::move(title);
}

WorkflowBuilder& WorkflowBuilder::add_node(WorkflowNode node) {
  definition_.nodes.push_back(std::move(node));
  return *this;
}

WorkflowBuilder& WorkflowBuilder::add_edge(WorkflowEdge edge) {
  definition_.edges.push_back(std::move(edge));
  return *this;
}

WorkflowBuilder& WorkflowBuilder::start(std::string id) {
  WorkflowNode node;
  node.id = std::move(id);
  node.type = "start";
  return add_node(std::move(node));
}

WorkflowBuilder& WorkflowBuilder::transform(std::string id, std::map<std::string, WorkflowValue> inputs) {
  return transform(std::move(id), WorkflowTransformOptions{
                                      .inputs = std::move(inputs),
                                  });
}

WorkflowBuilder& WorkflowBuilder::transform(std::string id, WorkflowTransformOptions options) {
  WorkflowNode node;
  node.id = std::move(id);
  node.type = "transform";
  apply_builder_node_options(node, std::move(options.inputs), std::move(options.wait_for), options.max_visits);
  return add_node(std::move(node));
}

WorkflowBuilder& WorkflowBuilder::tool(std::string id, std::string tool_name,
                                       std::map<std::string, WorkflowValue> inputs) {
  return tool(std::move(id), std::move(tool_name), WorkflowToolOptions{
                                                .inputs = std::move(inputs),
                                            });
}

WorkflowBuilder& WorkflowBuilder::tool(std::string id, std::string tool_name,
                                       WorkflowToolOptions options) {
  WorkflowNode node;
  node.id = std::move(id);
  node.type = "tool";
  node.tool_name = std::move(tool_name);
  apply_builder_node_options(node, std::move(options.inputs), std::move(options.wait_for), options.max_visits);
  return add_node(std::move(node));
}

WorkflowBuilder& WorkflowBuilder::agent(std::string id, std::string agent_id,
                                        std::map<std::string, WorkflowValue> inputs) {
  return agent(std::move(id), std::move(agent_id), WorkflowAgentOptions{
                                                .inputs = std::move(inputs),
                                            });
}

WorkflowBuilder& WorkflowBuilder::agent(std::string id, std::string agent_id,
                                        WorkflowAgentOptions options) {
  WorkflowNode node;
  node.id = std::move(id);
  node.type = "agent";
  node.agent_id = std::move(agent_id);
  apply_builder_node_options(node, std::move(options.inputs), std::move(options.wait_for), options.max_visits);
  return add_node(std::move(node));
}

WorkflowBuilder& WorkflowBuilder::condition(std::string id, WorkflowCondition condition) {
  return this->condition(std::move(id), std::move(condition), WorkflowConditionOptions{});
}

WorkflowBuilder& WorkflowBuilder::condition(std::string id, WorkflowCondition condition,
                                            WorkflowConditionOptions options) {
  WorkflowNode node;
  node.id = std::move(id);
  node.type = "condition";
  node.condition = std::move(condition);
  node.has_condition = true;
  apply_builder_node_options(node, std::move(options.inputs), std::move(options.wait_for), options.max_visits);
  return add_node(std::move(node));
}

WorkflowBuilder& WorkflowBuilder::human_wait(std::string id, std::string prompt,
                                             std::string response_key, Value metadata) {
  return human_wait(std::move(id), WorkflowHumanWaitOptions{
                                       .prompt = std::move(prompt),
                                       .response_key = std::move(response_key),
                                       .metadata = std::move(metadata),
                                   });
}

WorkflowBuilder& WorkflowBuilder::human_wait(std::string id, WorkflowHumanWaitOptions options) {
  WorkflowNode node;
  node.id = std::move(id);
  node.type = "human-wait";
  apply_builder_node_options(node, std::move(options.inputs), std::move(options.wait_for), options.max_visits);
  node.prompt = std::move(options.prompt);
  node.response_key = options.response_key.empty() ? "response" : std::move(options.response_key);
  node.metadata = options.metadata.is_object() ? std::move(options.metadata) : Value::object({});
  return add_node(std::move(node));
}

WorkflowBuilder& WorkflowBuilder::artifact(std::string id, std::string key, std::string mode,
                                           std::optional<WorkflowValue> value,
                                           std::string output_key) {
  return artifact(std::move(id), std::move(key), WorkflowArtifactOptions{
                                                 .mode = std::move(mode),
                                                 .value = std::move(value),
                                                 .output_key = std::move(output_key),
                                             });
}

WorkflowBuilder& WorkflowBuilder::artifact(std::string id, std::string key, WorkflowArtifactOptions options) {
  WorkflowNode node;
  node.id = std::move(id);
  node.type = "artifact";
  node.artifact_key = std::move(key);
  apply_builder_node_options(node, std::move(options.inputs), std::move(options.wait_for), options.max_visits);
  node.artifact_mode = options.mode.empty() ? "get" : std::move(options.mode);
  node.output_key = options.output_key.empty() ? "value" : std::move(options.output_key);
  if (options.value) {
    node.artifact_value = std::move(*options.value);
    node.has_artifact_value = true;
  }
  return add_node(std::move(node));
}

WorkflowBuilder& WorkflowBuilder::router(std::string id, std::vector<WorkflowNodeRoute> routes,
                                         std::string default_route) {
  return router(std::move(id), WorkflowRouterOptions{
                                   .routes = std::move(routes),
                                   .default_route = std::move(default_route),
                               });
}

WorkflowBuilder& WorkflowBuilder::router(std::string id, WorkflowRouterOptions options) {
  WorkflowNode node;
  node.id = std::move(id);
  node.type = "router";
  apply_builder_node_options(node, std::move(options.inputs), std::move(options.wait_for), options.max_visits);
  node.routes = std::move(options.routes);
  node.default_route = std::move(options.default_route);
  return add_node(std::move(node));
}

WorkflowBuilder& WorkflowBuilder::join_node(std::string id, std::string wait_for) {
  return join_node(std::move(id), WorkflowJoinNodeOptions{
                                      .wait_for = std::move(wait_for),
                                  });
}

WorkflowBuilder& WorkflowBuilder::join_node(std::string id, WorkflowJoinNodeOptions options) {
  WorkflowNode node;
  node.id = std::move(id);
  node.type = "join";
  apply_builder_node_options(node, std::move(options.inputs), std::move(options.wait_for), options.max_visits);
  return add_node(std::move(node));
}

WorkflowBuilder& WorkflowBuilder::webhook_wait(std::string id, std::string prompt,
                                               std::string event_key, Value metadata) {
  return webhook_wait(std::move(id), WorkflowWebhookWaitOptions{
                                         .prompt = std::move(prompt),
                                         .event_key = std::move(event_key),
                                         .metadata = std::move(metadata),
                                     });
}

WorkflowBuilder& WorkflowBuilder::webhook_wait(std::string id, WorkflowWebhookWaitOptions options) {
  WorkflowNode node;
  node.id = std::move(id);
  node.type = "webhook-wait";
  apply_builder_node_options(node, std::move(options.inputs), std::move(options.wait_for), options.max_visits);
  node.prompt = std::move(options.prompt);
  node.event_key = options.event_key.empty() ? "payload" : std::move(options.event_key);
  node.metadata = options.metadata.is_object() ? std::move(options.metadata) : Value::object({});
  return add_node(std::move(node));
}

WorkflowBuilder& WorkflowBuilder::end(std::string id, std::optional<WorkflowValue> result) {
  return end(std::move(id), WorkflowEndOptions{
                                .result = std::move(result),
                            });
}

WorkflowBuilder& WorkflowBuilder::end(std::string id, WorkflowEndOptions options) {
  WorkflowNode node;
  node.id = std::move(id);
  node.type = "end";
  apply_builder_node_options(node, std::move(options.inputs), std::move(options.wait_for), options.max_visits);
  if (options.result) {
    node.result = std::move(*options.result);
    node.has_result = true;
  }
  return add_node(std::move(node));
}

WorkflowBuilder& WorkflowBuilder::connect(std::string from, std::string to,
                                          std::optional<WorkflowCondition> condition) {
  WorkflowEdge edge;
  edge.id = from + "->" + to + ":" + std::to_string(definition_.edges.size() + 1);
  edge.from = std::move(from);
  edge.to = std::move(to);
  if (condition) {
    edge.condition = *condition;
    edge.has_condition = true;
  }
  return add_edge(std::move(edge));
}

WorkflowBuilder& WorkflowBuilder::branch(
    std::string from,
    std::vector<std::pair<std::string, std::optional<WorkflowCondition>>> branches) {
  for (auto& [to, condition] : branches) {
    connect(from, std::move(to), std::move(condition));
  }
  return *this;
}

WorkflowBuilder& WorkflowBuilder::parallel(std::string from, std::vector<std::string> to_node_ids) {
  for (auto& to : to_node_ids) {
    connect(from, std::move(to));
  }
  return *this;
}

WorkflowBuilder& WorkflowBuilder::join(const std::vector<std::string>& from_node_ids, std::string to_node_id,
                                       std::string wait_for) {
  for (auto& node : definition_.nodes) {
    if (node.id == to_node_id) {
      node.wait_for = wait_for.empty() ? "all" : wait_for;
      break;
    }
  }
  for (const auto& from : from_node_ids) {
    connect(from, to_node_id);
  }
  return *this;
}

WorkflowBuilder& WorkflowBuilder::sequence(const std::vector<std::string>& node_ids) {
  for (std::size_t index = 0; index + 1 < node_ids.size(); ++index) {
    connect(node_ids[index], node_ids[index + 1]);
  }
  return *this;
}

WorkflowDefinition WorkflowBuilder::build() const {
  return definition_;
}

WorkflowBuilder create_workflow(std::string id, std::string title) {
  return WorkflowBuilder(std::move(id), std::move(title));
}

WorkflowNode create_node(WorkflowNode node) {
  return node;
}

WorkflowEngine::WorkflowEngine(WorkflowEngineConfig config)
    : tools_(config.tools),
      tool_executor_(config.tool_executor),
      store_(config.store),
      node_registry_(create_default_workflow_node_registry()),
      agent_registry_(config.agent_registry ? *config.agent_registry : WorkflowAgentRegistry{}),
      tool_services_(std::move(config.tool_services)),
      hooks_(std::move(config.hooks)),
      event_bus_(config.event_bus ? config.event_bus : &owned_event_bus_),
      execution_policies_(std::move(config.execution_policies)) {
  if (config.node_registry) {
    for (auto& handler : config.node_registry->list()) {
      node_registry_.register_handler(std::move(handler));
    }
  }
}

WorkflowEngine::WorkflowEngine(ToolRegistry* tools, ToolExecutor* tool_executor, WorkflowStore* store,
                               WorkflowNodeRegistry* node_registry,
                               WorkflowAgentRegistry* agent_registry,
                               ToolExecutionServices tool_services,
                               HookSet hooks)
    : WorkflowEngine(WorkflowEngineConfig{
          .tools = tools,
          .tool_executor = tool_executor,
          .tool_services = std::move(tool_services),
          .agent_registry = agent_registry,
          .store = store,
          .node_registry = node_registry,
          .hooks = std::move(hooks),
      }) {}

WorkflowStore* WorkflowEngine::store() const noexcept {
  return store_;
}

EventBus* WorkflowEngine::event_bus() const noexcept {
  return event_bus_;
}

std::size_t WorkflowEngine::register_event_sink(EventBus::Sink sink) {
  return event_bus_ ? event_bus_->register_sink(std::move(sink)) : 0;
}

void WorkflowEngine::unregister_event_sink(std::size_t sink_id) {
  if (event_bus_) {
    event_bus_->unregister_sink(sink_id);
  }
}

WorkflowAgentDefinition& WorkflowEngine::register_agent(WorkflowAgentDefinition agent) {
  return agent_registry_.register_agent(std::move(agent));
}

WorkflowAgentDefinition& WorkflowEngine::register_agent_runner(std::string id,
                                                               std::shared_ptr<AgentRunner> runner,
                                                               std::string description) {
  return agent_registry_.register_runner(std::move(id), std::move(runner), std::move(description));
}

WorkflowNodeHandler& WorkflowEngine::register_node(WorkflowNodeHandler handler) {
  return node_registry_.register_handler(std::move(handler));
}

WorkflowCheckpoint WorkflowEngine::append_checkpoint(WorkflowRunState& state, WorkflowCheckpoint checkpoint) const {
  if (checkpoint.id.empty()) {
    checkpoint.id = generate_uuid();
  }
  if (checkpoint.at.empty()) {
    checkpoint.at = now_iso8601();
  }
  if (checkpoint.workflow_run_id.empty()) {
    checkpoint.workflow_run_id = state.workflow_run_id;
  }
  if (checkpoint.workflow_id.empty()) {
    checkpoint.workflow_id = state.workflow_id;
  }
  if (!checkpoint.payload.is_object()) {
    checkpoint.payload = Value::object({});
  }
  state.checkpoints.push_back(checkpoint);
  state.updated_at = checkpoint.at;
  return checkpoint;
}

void WorkflowEngine::append_event(WorkflowRunState& state, std::string category, Value payload) const {
  state.event_log.push_back(Value::object({{"at", now_iso8601()},
                                           {"category", std::move(category)},
                                           {"payload", payload.is_object() ? payload : Value::object({})}}));
  state.updated_at = now_iso8601();
}

TraceContext WorkflowEngine::ensure_trace_context(WorkflowRunState& state) const {
  TraceContext trace;
  if (auto existing = get_trace_context(state.context)) {
    trace = *existing;
  }
  if (trace.workflow_run_id.empty()) {
    trace.workflow_run_id = state.workflow_run_id;
  }
  if (trace.span_name.empty()) {
    trace.span_name = "workflow.run";
  }
  trace = create_trace_context(std::move(trace));
  if (!state.context.is_object()) {
    state.context = Value::object({});
  }
  state.context["traceContext"] = trace_context_to_value(trace);
  return trace;
}

void WorkflowEngine::publish_event(std::string category, ExecutionTarget target, Value payload, TraceContext trace) const {
  if (event_bus_) {
    event_bus_->publish(std::move(category), target, payload.is_object() ? std::move(payload) : Value::object({}),
                        std::move(trace));
  }
}

Value WorkflowEngine::source_for_state(const WorkflowRunState& state) const {
  Value::Object nodes;
  for (const auto& [node_id, node_state] : state.node_states) {
    nodes[node_id] = Value::object({
        {"output", node_state.output},
        {"status", to_string(node_state.status)},
        {"waiting", node_state.waiting ? workflow_node_wait_state_to_value(*node_state.waiting) : Value()},
    });
  }
  return Value::object({{"input", state.input},
                        {"context", state.context},
                        {"output", state.output},
                        {"nodes", Value(nodes)},
                        {"artifacts", state.context.at("artifacts").is_object()
                                          ? state.context.at("artifacts")
                                          : Value::object({})}});
}

Value WorkflowEngine::resolve_value(const WorkflowValue& value, const WorkflowRunState& state) const {
  if (value.kind == WorkflowValueKind::Literal) {
    return value.value;
  }

  auto get_path = [&](const std::string& path) {
    Value source = source_for_state(state);
    std::string normalized = path;
    if (normalized.rfind("$.", 0) == 0) {
      normalized = normalized.substr(2);
    }
    return select_json_path(source, normalized);
  };

  if (value.kind == WorkflowValueKind::Ref) {
    return get_path(value.path);
  }

  std::string rendered = value.templ;
  std::size_t start = 0;
  while ((start = rendered.find("{{", start)) != std::string::npos) {
    const auto end = rendered.find("}}", start + 2);
    if (end == std::string::npos) {
      break;
    }
    std::string path = trim_copy(rendered.substr(start + 2, end - start - 2));
    const auto resolved = get_path(path);
    const std::string replacement = resolved.is_string() ? resolved.as_string() : safe_json_stringify(resolved);
    rendered.replace(start, end - start + 2, replacement);
    start += replacement.size();
  }
  return rendered;
}

bool WorkflowEngine::evaluate_condition(const WorkflowCondition& condition, const WorkflowRunState& state) const {
  const auto left = resolve_value(condition.left, state);
  switch (condition.kind) {
    case WorkflowConditionKind::Truthy:
      if (left.is_bool()) {
        return left.as_bool();
      }
      if (left.is_null()) {
        return false;
      }
      if (left.is_string()) {
        return !left.as_string().empty();
      }
      if (left.is_array()) {
        return !left.as_array().empty();
      }
      if (left.is_object()) {
        return !left.as_object().empty();
      }
      return left.as_number() != 0;
    case WorkflowConditionKind::NotEmpty:
      if (left.is_array()) {
        return !left.as_array().empty();
      }
      if (left.is_object()) {
        return !left.as_object().empty();
      }
      return !left.as_string().empty();
    case WorkflowConditionKind::Equals:
      return left == resolve_value(condition.right, state);
  }
  return false;
}

Value WorkflowEngine::get_artifact(WorkflowRunState& state, const std::string& key) const {
  if (!state.context.at("artifacts").is_object()) {
    state.context["artifacts"] = Value::object({});
  }
  return state.context.at("artifacts").at(key);
}

Value WorkflowEngine::set_artifact(WorkflowRunState& state, const std::string& key, const Value& value) const {
  if (!state.context.at("artifacts").is_object()) {
    state.context["artifacts"] = Value::object({});
  }
  state.context["artifacts"][key] = value;
  append_checkpoint(state, WorkflowCheckpoint{.type = "workflow.artifact.updated",
                                              .payload = Value::object({{"key", key}, {"value", value}})});
  return value;
}

std::optional<Value> WorkflowEngine::consume_signal(WorkflowRunState& state, const std::string& type,
                                                    const std::string& node_id) const {
  auto& bucket = type == "webhook" ? state.webhook_payloads : state.human_responses;
  const auto found = bucket.find(node_id);
  if (found == bucket.end()) {
    return std::nullopt;
  }
  auto value = found->second;
  bucket.erase(found);
  return value;
}

Value WorkflowEngine::execute_node(const WorkflowNode& node, WorkflowRunState& state) {
  Value::Object input_object;
  for (const auto& [key, workflow_value] : node.inputs) {
    input_object[key] = resolve_value(workflow_value, state);
  }
  Value input(input_object);
  const auto workflow_trace = ensure_trace_context(state);
  const auto node_trace = derive_child_trace_context(
      workflow_trace,
      TraceSpanDescriptor{.name = "workflow.node." + node.type},
      TraceContext{.workflow_run_id = state.workflow_run_id});

  auto& node_state = state.node_states[node.id];
  node_state.queued_at = now_iso8601();
  append_checkpoint(state, WorkflowCheckpoint{.type = "workflow.node.queued",
                                              .node_id = node.id,
                                              .payload = Value::object({{"nodeType", node.type}})});
  node_state.status = WorkflowStatus::Running;
  node_state.started_at = now_iso8601();
  node_state.input = input;
  node_state.waiting = std::nullopt;
  node_state.visits += 1;
  append_checkpoint(state, WorkflowCheckpoint{.type = "workflow.node.started",
                                              .node_id = node.id,
                                              .payload = Value::object({{"nodeType", node.type},
                                                                       {"visits", node_state.visits}})});
  if (hooks_.before_workflow_node) {
    WorkflowNodeHookContext hook_context;
    hook_context.target = ExecutionTarget::WorkflowNode;
    hook_context.trace_id = node_trace.trace_id;
    hook_context.run_id = node_trace.run_id;
    hook_context.workflow_run_id = state.workflow_run_id;
    hook_context.workflow_id = state.definition_snapshot.id;
    hook_context.node_id = node.id;
    hook_context.node_type = node.type;
    hook_context.input = input;
    hooks_.before_workflow_node(hook_context);
  }
  publish_event("workflow.node.started", ExecutionTarget::WorkflowNode,
                Value::object({{"workflowId", state.definition_snapshot.id},
                               {"nodeId", node.id},
                               {"nodeType", node.type}}),
                node_trace);

  const auto handler = node_registry_.find(node.type);
  if (!handler) {
    throw ConfigurationError("No workflow node handler registered for type: " + node.type);
  }

  WorkflowNodeHandlerContext context;
  context.definition = &state.definition_snapshot;
  context.state = &state;
  context.node_state = &node_state;
  context.input = input;
  context.source = source_for_state(state);
  context.resolve_value = [&](const WorkflowValue& workflow_value) {
    return resolve_value(workflow_value, state);
  };
  context.evaluate_condition = [&](const WorkflowCondition& condition) {
    return evaluate_condition(condition, state);
  };
  context.get_artifact = [&](const std::string& key) {
    return get_artifact(state, key);
  };
  context.set_artifact = [&](const std::string& key, const Value& value) {
    return set_artifact(state, key, value);
  };
  context.consume_signal = [&](const std::string& type, const std::string& node_id) {
    return consume_signal(state, type, node_id);
  };
  context.wait_for_signal = [](const std::string& type, const std::string& prompt, const Value& metadata) {
    return create_workflow_wait_signal(type, prompt, metadata);
  };
  context.append_event = [&](const std::string& category, const Value& payload) {
    append_event(state, category, payload);
  };
  context.run_tool = [&](const std::string& tool_name, const Value& tool_input) {
    if (!tool_executor_) {
      throw ConfigurationError("Workflow tool node requires a ToolExecutor.");
    }
    if (tools_ && !tools_->get(tool_name)) {
      throw ConfigurationError("Unknown workflow tool: " + tool_name);
    }
    ToolCall call{generate_uuid(), tool_name, tool_input};
    ToolExecutionContext tool_context;
    tool_context.trace_context = node_trace;
    tool_context.attributes = Value::object({
        {"workflowRunId", state.workflow_run_id},
        {"workflowId", state.definition_snapshot.id},
        {"nodeId", node.id},
        {"nodes", workflow_run_state_to_value(state).at("nodes")},
    });
    ToolExecutionServices runtime_services;
    runtime_services.workflow_engine = this;
    runtime_services.workflow_definition = &state.definition_snapshot;
    runtime_services.values = Value::object({
        {"workflowRunId", state.workflow_run_id},
        {"workflowId", state.definition_snapshot.id},
        {"workflow", Value::object({
                         {"workflowRunId", state.workflow_run_id},
                         {"workflowId", state.definition_snapshot.id},
                         {"nodeId", node.id},
                         {"definition", workflow_definition_to_value(state.definition_snapshot)},
                     })},
    });
    tool_context = with_tool_execution_services(std::move(tool_context), tool_services_, runtime_services);
    auto result = tool_executor_->execute_tool_call(call, tool_context);
    if (!result.ok) {
      throw ToolExecutionError(result.error, tool_name, call.id);
    }
    return Value(result.output);
  };
  context.run_child_agent = [&](const std::string& agent_id, const Value& child_input) {
    const auto agent = agent_registry_.find(agent_id);
    if (!agent) {
      throw ConfigurationError("Unknown workflow agent: " + agent_id);
    }
    const auto child_trace = derive_child_trace_context(
        node_trace,
        TraceSpanDescriptor{.name = "workflow.child-agent"},
        TraceContext{.workflow_run_id = state.workflow_run_id});
    if (hooks_.before_child_agent) {
      ChildAgentHookContext hook_context;
      hook_context.target = ExecutionTarget::ChildAgent;
      hook_context.trace_id = child_trace.trace_id;
      hook_context.run_id = child_trace.run_id;
      hook_context.workflow_run_id = state.workflow_run_id;
      hook_context.workflow_id = state.definition_snapshot.id;
      hook_context.agent_id = agent_id;
      hook_context.input = child_input;
      hooks_.before_child_agent(hook_context);
    }
    publish_event("agent.child.started", ExecutionTarget::ChildAgent,
                  Value::object({{"workflowId", state.definition_snapshot.id},
                                 {"nodeId", node.id},
                                 {"agentId", agent_id}}),
                  child_trace);
    Value result;
    try {
      if (agent->run) {
        result = agent->run(child_input, state);
      } else {
        Value child_context = state.context.is_object() ? state.context : Value::object({});
        child_context["workflowRunId"] = state.workflow_run_id;
        child_context["workflowId"] = state.definition_snapshot.id;
        child_context["nodeId"] = node.id;
        child_context["agentId"] = agent_id;
        child_context["traceContext"] = trace_context_to_value(derive_child_trace_context(
            child_trace,
            TraceSpanDescriptor{.name = "workflow.child-agent.run"},
            TraceContext{.workflow_run_id = state.workflow_run_id}));
        const std::string child_session_id = state.workflow_run_id + ":agent:" + agent_id;
        result = workflow_agent_runner_result_to_value(
            agent->runner->run(workflow_child_agent_input_text(child_input),
                               child_session_id,
                               {},
                               {},
                               {},
                               {},
                               child_context));
      }
    } catch (const std::exception& error) {
      if (hooks_.on_child_agent_error) {
        ChildAgentHookContext hook_context;
        hook_context.target = ExecutionTarget::ChildAgent;
        hook_context.trace_id = child_trace.trace_id;
        hook_context.run_id = child_trace.run_id;
        hook_context.workflow_run_id = state.workflow_run_id;
        hook_context.workflow_id = state.definition_snapshot.id;
        hook_context.agent_id = agent_id;
        hook_context.input = child_input;
        hook_context.error = error.what();
        hooks_.on_child_agent_error(hook_context);
      }
      throw;
    }
    append_checkpoint(state, WorkflowCheckpoint{.type = "workflow.child-agent.completed",
                                                .node_id = node.id,
                                                .payload = Value::object({{"agentId", agent_id}})});
    publish_event("agent.child.completed", ExecutionTarget::ChildAgent,
                  Value::object({{"workflowId", state.definition_snapshot.id},
                                 {"nodeId", node.id},
                                 {"agentId", agent_id}}),
                  child_trace);
    append_event(state, "workflow.child-agent.completed",
                 Value::object({{"nodeId", node.id}, {"agentId", agent_id}}));
    if (hooks_.after_child_agent) {
      ChildAgentHookContext hook_context;
      hook_context.target = ExecutionTarget::ChildAgent;
      hook_context.trace_id = child_trace.trace_id;
      hook_context.run_id = child_trace.run_id;
      hook_context.workflow_run_id = state.workflow_run_id;
      hook_context.workflow_id = state.definition_snapshot.id;
      hook_context.agent_id = agent_id;
      hook_context.input = child_input;
      hook_context.result = result;
      hooks_.after_child_agent(hook_context);
    }
    return result;
  };

  WorkflowNodeHandlerResult result;
  try {
    const auto target = node.type == "agent" ? ExecutionTarget::ChildAgent : ExecutionTarget::WorkflowNode;
    result = execute_with_policies(
        target,
        execution_policies_,
        Value::object({{"workflowId", state.definition_snapshot.id},
                       {"nodeId", node.id},
                       {"nodeType", node.type}}),
        nullptr,
        [&]() { return handler->execute(node, context); },
        [&](const RetryScheduledContext& retry) {
          publish_event("retry.scheduled", target, workflow_retry_scheduled_payload(retry, node), node_trace);
        });
  } catch (const std::exception& error) {
    if (hooks_.on_workflow_node_error) {
      WorkflowNodeHookContext hook_context;
      hook_context.target = ExecutionTarget::WorkflowNode;
      hook_context.trace_id = node_trace.trace_id;
      hook_context.run_id = node_trace.run_id;
      hook_context.workflow_run_id = state.workflow_run_id;
      hook_context.workflow_id = state.definition_snapshot.id;
      hook_context.node_id = node.id;
      hook_context.node_type = node.type;
      hook_context.input = input;
      hook_context.error = error.what();
      hooks_.on_workflow_node_error(hook_context);
    }
    node_state.status = WorkflowStatus::Failed;
    node_state.error = error.what();
    node_state.completed_at = now_iso8601();
    append_checkpoint(state, WorkflowCheckpoint{.type = "workflow.node.failed",
                                                .node_id = node.id,
                                                .payload = Value::object({{"nodeType", node.type},
                                                                         {"error", error.what()}})});
    publish_event("workflow.node.failed", ExecutionTarget::WorkflowNode,
                  Value::object({{"workflowId", state.definition_snapshot.id},
                                 {"nodeId", node.id},
                                 {"nodeType", node.type},
                                 {"error", error.what()}}),
                  node_trace);
    append_event(state, "workflow.node.failed",
                 Value::object({{"nodeId", node.id}, {"nodeType", node.type}, {"error", error.what()}}));
    throw;
  }
  if (std::holds_alternative<WorkflowWaitSignal>(result)) {
    const auto& wait = std::get<WorkflowWaitSignal>(result);
    node_state.status = WorkflowStatus::Waiting;
    node_state.waiting = WorkflowNodeWaitState{
        .kind = wait.wait_type,
        .prompt = wait.prompt,
        .metadata = wait.metadata.is_object() ? wait.metadata : Value::object({}),
    };
    append_checkpoint(state, WorkflowCheckpoint{.type = "workflow.node.waiting",
                                                .node_id = node.id,
                                                .payload = Value::object({{"nodeType", node.type},
                                                                         {"waitType", wait.wait_type},
                                                                         {"prompt", wait.prompt}})});
    publish_event("workflow.node.waiting", ExecutionTarget::WorkflowNode,
                  Value::object({{"workflowId", state.definition_snapshot.id},
                                 {"nodeId", node.id},
                                 {"nodeType", node.type},
                                 {"waitType", wait.wait_type}}),
                  node_trace);
    append_event(state, "workflow.node.waiting",
                 Value::object({{"nodeId", node.id}, {"nodeType", node.type}, {"waitType", wait.wait_type}}));
    return Value::object({{"kind", "workflow-wait"}, {"waitType", wait.wait_type}});
  }

  Value output = std::get<Value>(result);

  node_state.output = output;
  node_state.status = WorkflowStatus::Completed;
  node_state.completed_at = now_iso8601();
  state.output = output;
  append_checkpoint(state, WorkflowCheckpoint{.type = "workflow.node.completed",
                                              .node_id = node.id,
                                              .payload = Value::object({{"nodeType", node.type}})});
  publish_event("workflow.node.completed", ExecutionTarget::WorkflowNode,
                Value::object({{"workflowId", state.definition_snapshot.id},
                               {"nodeId", node.id},
                               {"nodeType", node.type}}),
                node_trace);
  append_event(state, "workflow.node.completed", Value::object({{"nodeId", node.id},
                                                               {"nodeType", node.type}}));
  if (hooks_.after_workflow_node) {
    WorkflowNodeHookContext hook_context;
    hook_context.target = ExecutionTarget::WorkflowNode;
    hook_context.trace_id = node_trace.trace_id;
    hook_context.run_id = node_trace.run_id;
    hook_context.workflow_run_id = state.workflow_run_id;
    hook_context.workflow_id = state.definition_snapshot.id;
    hook_context.node_id = node.id;
    hook_context.node_type = node.type;
    hook_context.input = input;
    hook_context.result = output;
    hooks_.after_workflow_node(hook_context);
  }
  return output;
}

std::vector<const WorkflowNode*> WorkflowEngine::ready_nodes(const WorkflowDefinition& definition,
                                                             const WorkflowRunState& state) const {
  std::vector<const WorkflowNode*> ready;
  for (const auto& node : definition.nodes) {
    const auto state_it = state.node_states.find(node.id);
    if (state_it == state.node_states.end() || state_it->second.status != WorkflowStatus::Pending) {
      continue;
    }
    if (node.type == "start") {
      ready.push_back(&node);
      continue;
    }

    std::vector<std::string> inbound_statuses;
    for (std::size_t index = 0; index < definition.edges.size(); ++index) {
      const auto& edge = definition.edges[index];
      if (edge.to == node.id) {
        const auto edge_state = state.edge_states.find(workflow_edge_state_id(edge, index));
        inbound_statuses.push_back(edge_state == state.edge_states.end() ? "pending" : edge_state->second);
      }
    }
    if (inbound_statuses.empty()) {
      ready.push_back(&node);
      continue;
    }
    const auto activated = std::count(inbound_statuses.begin(), inbound_statuses.end(), "activated");
    const auto pending = std::count(inbound_statuses.begin(), inbound_statuses.end(), "pending");
    if (node.wait_for == "any" && activated > 0) {
      ready.push_back(&node);
    } else if (node.wait_for != "any" && pending == 0 && activated > 0) {
      ready.push_back(&node);
    }
  }
  return ready;
}

void WorkflowEngine::activate_outbound_edges(const WorkflowDefinition& definition, WorkflowRunState& state,
                                             const WorkflowNode& node) {
  const auto edge_trace = derive_child_trace_context(
      ensure_trace_context(state),
      TraceSpanDescriptor{.name = "workflow.edge"},
      TraceContext{.workflow_run_id = state.workflow_run_id});
  for (std::size_t index = 0; index < definition.edges.size(); ++index) {
    const auto& edge = definition.edges[index];
    if (edge.from != node.id) {
      continue;
    }
    const bool activated = !edge.has_condition || evaluate_condition(edge.condition, state);
    const std::string edge_id = workflow_edge_state_id(edge, index);
    state.edge_states[edge_id] = activated ? "activated" : "skipped";
    publish_event("workflow.edge.activated", ExecutionTarget::Workflow,
                  Value::object({{"workflowId", definition.id},
                                 {"edgeId", edge_id},
                                 {"from", edge.from},
                                 {"to", edge.to},
                                 {"status", state.edge_states[edge_id]}}),
                  edge_trace);
    append_event(state, "workflow.edge.activated",
                 Value::object({{"edgeId", edge_id}, {"status", state.edge_states[edge_id]}}));
    append_checkpoint(state, WorkflowCheckpoint{.type = "workflow.edge.activated",
                                                .edge_id = edge_id,
                                                .payload = Value::object({{"from", edge.from},
                                                                         {"to", edge.to},
                                                                         {"status", state.edge_states[edge_id]}})});
  }
}

WorkflowExecutionResult WorkflowEngine::run(const WorkflowDefinition& definition, Value input, Value context) {
  WorkflowRunState state = store_ ? store_->create_run(definition, std::move(input), std::move(context))
                                  : initialize_workflow_run_state(definition, generate_uuid(),
                                                                  std::move(input), std::move(context));
  return execute_state(std::move(state), definition, false);
}

WorkflowExecutionResult WorkflowEngine::run(WorkflowRunInput input) {
  return run(input.definition, std::move(input.input), std::move(input.context));
}

WorkflowExecutionResult WorkflowEngine::resume(const std::string& workflow_run_id,
                                               std::optional<WorkflowDefinition> definition) {
  if (!store_) {
    throw ConfigurationError("Workflow resume requires a WorkflowStore.");
  }
  auto state = store_->get_run(workflow_run_id);
  if (!state) {
    throw ConfigurationError("Workflow run not found: " + workflow_run_id);
  }
  WorkflowDefinition resolved_definition = definition ? *definition : state->definition_snapshot;
  state->definition_snapshot = resolved_definition;
  return execute_state(std::move(*state), resolved_definition, true);
}

WorkflowExecutionResult WorkflowEngine::resume(WorkflowResumeInput input) {
  return resume(input.workflow_run_id, std::move(input.definition));
}

WorkflowExecutionResult WorkflowEngine::submit_human_response(const std::string& workflow_run_id,
                                                              const std::string& node_id,
                                                              Value payload) {
  if (!store_) {
    throw ConfigurationError("Workflow human response requires a WorkflowStore.");
  }
  auto state = store_->get_run(workflow_run_id);
  if (!state) {
    throw ConfigurationError("Workflow run not found: " + workflow_run_id);
  }
  auto node_state = state->node_states.find(node_id);
  if (node_state == state->node_states.end()) {
    throw ConfigurationError("Workflow node not found: " + node_id);
  }
  state->human_responses[node_id] = std::move(payload);
  node_state->second.status = WorkflowStatus::Pending;
  node_state->second.waiting = std::nullopt;
  node_state->second.error.clear();
  node_state->second.completed_at.clear();
  append_checkpoint(*state, WorkflowCheckpoint{.type = "workflow.human-response.received",
                                               .node_id = node_id,
                                               .payload = Value::object({{"acknowledged", true}})});
  publish_event("workflow.human-response.received", ExecutionTarget::Workflow,
                Value::object({{"workflowId", state->workflow_id}, {"nodeId", node_id}}),
                derive_child_trace_context(ensure_trace_context(*state),
                                           TraceSpanDescriptor{.name = "workflow.human-response"},
                                           TraceContext{.workflow_run_id = state->workflow_run_id}));
  store_->save_run(*state);
  return resume(workflow_run_id);
}

WorkflowExecutionResult WorkflowEngine::submit_human_response(WorkflowHumanResponseInput input) {
  return submit_human_response(input.workflow_run_id, input.node_id, std::move(input.payload));
}

WorkflowExecutionResult WorkflowEngine::submit_webhook_payload(const std::string& workflow_run_id,
                                                               const std::string& node_id,
                                                               Value payload) {
  if (!store_) {
    throw ConfigurationError("Workflow webhook payload requires a WorkflowStore.");
  }
  auto state = store_->get_run(workflow_run_id);
  if (!state) {
    throw ConfigurationError("Workflow run not found: " + workflow_run_id);
  }
  auto node_state = state->node_states.find(node_id);
  if (node_state == state->node_states.end()) {
    throw ConfigurationError("Workflow node not found: " + node_id);
  }
  state->webhook_payloads[node_id] = std::move(payload);
  node_state->second.status = WorkflowStatus::Pending;
  node_state->second.waiting = std::nullopt;
  node_state->second.error.clear();
  node_state->second.completed_at.clear();
  append_checkpoint(*state, WorkflowCheckpoint{.type = "workflow.webhook.received",
                                               .node_id = node_id,
                                               .payload = Value::object({{"acknowledged", true}})});
  publish_event("workflow.webhook.received", ExecutionTarget::Workflow,
                Value::object({{"workflowId", state->workflow_id}, {"nodeId", node_id}}),
                derive_child_trace_context(ensure_trace_context(*state),
                                           TraceSpanDescriptor{.name = "workflow.webhook"},
                                           TraceContext{.workflow_run_id = state->workflow_run_id}));
  store_->save_run(*state);
  return resume(workflow_run_id);
}

WorkflowExecutionResult WorkflowEngine::submit_webhook_payload(WorkflowWebhookPayloadInput input) {
  return submit_webhook_payload(input.workflow_run_id, input.node_id, std::move(input.payload));
}

WorkflowExecutionResult WorkflowEngine::execute_state(WorkflowRunState state, const WorkflowDefinition& definition,
                                                      bool resumed) {
  auto save_state = [&]() {
    if (store_) {
      store_->save_run(state);
    }
  };

  state.status = WorkflowStatus::Running;
  state.completed_at.clear();
  state.updated_at = now_iso8601();
  const auto workflow_trace = ensure_trace_context(state);
  const std::string lifecycle_event = resumed && !state.checkpoints.empty() ? "workflow.resumed" : "workflow.started";
  append_checkpoint(state, WorkflowCheckpoint{.type = lifecycle_event,
                                              .payload = Value::object({})});
  save_state();
  if (hooks_.before_workflow) {
    WorkflowHookContext hook_context;
    hook_context.target = ExecutionTarget::Workflow;
    hook_context.trace_id = workflow_trace.trace_id;
    hook_context.run_id = workflow_trace.run_id;
    hook_context.workflow_run_id = state.workflow_run_id;
    hook_context.workflow_id = definition.id;
    hook_context.input = state.input;
    hooks_.before_workflow(hook_context);
  }
  publish_event(lifecycle_event, ExecutionTarget::Workflow,
                Value::object({{"workflowId", definition.id}}),
                workflow_trace);

  try {
    while (true) {
      auto ready = ready_nodes(definition, state);
      if (ready.empty()) {
        break;
      }
      for (const auto* node : ready) {
        execute_node(*node, state);
        if (state.node_states.at(node->id).status == WorkflowStatus::Completed) {
          activate_outbound_edges(definition, state, *node);
        }
        state.updated_at = now_iso8601();
        save_state();
      }
    }

    const bool failed = std::any_of(state.node_states.begin(), state.node_states.end(), [](const auto& entry) {
      return entry.second.status == WorkflowStatus::Failed;
    });
    const bool waiting = std::any_of(state.node_states.begin(), state.node_states.end(), [](const auto& entry) {
      return entry.second.status == WorkflowStatus::Waiting;
    });
    if (waiting && !failed) {
      state.status = WorkflowStatus::Waiting;
      state.updated_at = now_iso8601();
      save_state();
      if (hooks_.after_workflow) {
        WorkflowHookContext hook_context;
        hook_context.target = ExecutionTarget::Workflow;
        hook_context.trace_id = workflow_trace.trace_id;
        hook_context.run_id = workflow_trace.run_id;
        hook_context.workflow_run_id = state.workflow_run_id;
        hook_context.workflow_id = definition.id;
        hook_context.input = state.input;
        hook_context.result = Value::object({{"status", to_string(state.status)}, {"output", state.output}});
        hooks_.after_workflow(hook_context);
      }
      return WorkflowExecutionResult{state.workflow_run_id, state.status, state.output, state};
    }

    state.status = failed ? WorkflowStatus::Failed : WorkflowStatus::Completed;
    state.completed_at = now_iso8601();
    state.updated_at = state.completed_at;
    append_checkpoint(state, WorkflowCheckpoint{.type = failed ? "workflow.failed" : "workflow.completed",
                                                .payload = Value::object({{"status", to_string(state.status)}})});
    save_state();
  } catch (const std::exception& error) {
    if (hooks_.on_workflow_error) {
      WorkflowHookContext hook_context;
      hook_context.target = ExecutionTarget::Workflow;
      hook_context.trace_id = workflow_trace.trace_id;
      hook_context.run_id = workflow_trace.run_id;
      hook_context.workflow_run_id = state.workflow_run_id;
      hook_context.workflow_id = definition.id;
      hook_context.input = state.input;
      hook_context.error = error.what();
      hooks_.on_workflow_error(hook_context);
    }
    state.status = WorkflowStatus::Failed;
    state.output = Value::object({{"error", error.what()}});
    state.completed_at = now_iso8601();
    state.updated_at = state.completed_at;
    append_checkpoint(state, WorkflowCheckpoint{.type = "workflow.failed",
                                                .payload = Value::object({{"error", error.what()}})});
    save_state();
  }

  WorkflowExecutionResult result{state.workflow_run_id, state.status, state.output, state};
  if (hooks_.after_workflow) {
    WorkflowHookContext hook_context;
    hook_context.target = ExecutionTarget::Workflow;
    hook_context.trace_id = workflow_trace.trace_id;
    hook_context.run_id = workflow_trace.run_id;
    hook_context.workflow_run_id = state.workflow_run_id;
    hook_context.workflow_id = definition.id;
    hook_context.input = state.input;
    hook_context.result = Value::object({{"status", to_string(state.status)}, {"output", state.output}});
    hooks_.after_workflow(hook_context);
  }
  Value workflow_event_payload = Value::object({{"workflowId", definition.id}});
  if (state.status == WorkflowStatus::Failed && state.output.at("error").is_string()) {
    workflow_event_payload["error"] = state.output.at("error");
  }
  publish_event(state.status == WorkflowStatus::Completed ? "workflow.completed" : "workflow.failed",
                ExecutionTarget::Workflow,
                workflow_event_payload,
                workflow_trace);
  return result;
}
}  // namespace agent
