#pragma once

#include "agent/context.hpp"
#include "agent/http.hpp"
#include "agent/tools.hpp"
#include "agent/tool_services_modules.hpp"

#include <mutex>

namespace agent {

class AgentRunner;

enum class WorkflowValueKind {
  Literal,
  Ref,
  Template,
};

struct WorkflowValue {
  WorkflowValueKind kind = WorkflowValueKind::Literal;
  Value value;
  std::string path;
  std::string templ;
};

enum class WorkflowConditionKind {
  Truthy,
  Equals,
  NotEmpty,
};

struct WorkflowCondition {
  WorkflowConditionKind kind = WorkflowConditionKind::Truthy;
  WorkflowValue left;
  WorkflowValue right;
};

struct WorkflowNodeRoute {
  std::string key;
  WorkflowCondition condition;
  bool has_condition = false;
};

struct WorkflowNode {
  std::string id;
  std::string type;
  std::string wait_for = "all";
  std::map<std::string, WorkflowValue> inputs;
  std::size_t max_visits = 1;
  std::string tool_name;
  std::string agent_id;
  WorkflowCondition condition;
  bool has_condition = false;
  WorkflowValue result;
  bool has_result = false;
  std::string prompt;
  std::string message;
  std::string response_key = "response";
  std::string event_key = "payload";
  std::string url;
  std::string method;
  Value metadata = Value::object({});
  std::string artifact_mode = "get";
  std::string artifact_key;
  WorkflowValue artifact_value;
  bool has_artifact_value = false;
  std::string output_key = "value";
  std::vector<WorkflowNodeRoute> routes;
  std::string default_route;
};

struct WorkflowEdge {
  std::string id;
  std::string from;
  std::string to;
  WorkflowCondition condition;
  bool has_condition = false;
};

struct WorkflowDefinition {
  std::string id;
  std::string title;
  std::vector<WorkflowNode> nodes;
  std::vector<WorkflowEdge> edges;
};

enum class WorkflowStatus {
  Pending,
  Running,
  Waiting,
  Completed,
  Failed,
  Skipped,
};

std::string to_string(WorkflowStatus status);
WorkflowStatus workflow_status_from_string(const std::string& status);

struct WorkflowNodeWaitState {
  std::string kind;
  std::string prompt;
  Value metadata = Value::object({});
};

struct WorkflowNodeState {
  std::string node_id;
  WorkflowStatus status = WorkflowStatus::Pending;
  std::size_t visits = 0;
  Value input = Value::object({});
  Value output;
  std::string error;
  std::string queued_at;
  std::string started_at;
  std::string completed_at;
  std::optional<WorkflowNodeWaitState> waiting;
};

struct WorkflowCheckpoint {
  std::string id;
  std::string at;
  std::string workflow_run_id;
  std::string type;
  std::string workflow_id;
  std::string node_id;
  std::string edge_id;
  Value payload = Value::object({});
};

struct WorkflowRunState {
  std::string workflow_run_id;
  std::string workflow_id;
  WorkflowDefinition definition_snapshot;
  WorkflowStatus status = WorkflowStatus::Pending;
  Value input;
  Value output;
  Value context = Value::object({});
  std::map<std::string, Value> human_responses;
  std::map<std::string, Value> webhook_payloads;
  std::map<std::string, WorkflowNodeState> node_states;
  std::map<std::string, std::string> edge_states;
  std::vector<WorkflowCheckpoint> checkpoints;
  std::vector<Value> event_log;
  std::string started_at;
  std::string updated_at;
  std::string completed_at;
};

struct WorkflowExecutionResult {
  std::string workflow_run_id;
  WorkflowStatus status = WorkflowStatus::Pending;
  Value output;
  WorkflowRunState state;
};

struct WorkflowWaitSignal {
  std::string wait_type;
  std::string prompt;
  Value metadata = Value::object({});
};

WorkflowWaitSignal create_workflow_wait_signal(std::string wait_type, std::string prompt = {},
                                               Value metadata = Value::object({}));

struct WorkflowNodeHandlerContext {
  const WorkflowDefinition* definition = nullptr;
  WorkflowRunState* state = nullptr;
  WorkflowNodeState* node_state = nullptr;
  Value input = Value::object({});
  Value source = Value::object({});
  std::function<Value(const std::string&, const Value&)> run_tool;
  std::function<Value(const std::string&, const Value&)> run_child_agent;
  std::function<Value(const WorkflowValue&)> resolve_value;
  std::function<bool(const WorkflowCondition&)> evaluate_condition;
  std::function<Value(const std::string&)> get_artifact;
  std::function<Value(const std::string&, const Value&)> set_artifact;
  std::function<std::optional<Value>(const std::string&, const std::string&)> consume_signal;
  std::function<WorkflowWaitSignal(const std::string&, const std::string&, const Value&)> wait_for_signal;
  std::function<void(const std::string&, const Value&)> append_event;
};

using WorkflowNodeHandlerResult = std::variant<Value, WorkflowWaitSignal>;
using WorkflowNodeHandlerFn = std::function<WorkflowNodeHandlerResult(const WorkflowNode&,
                                                                      WorkflowNodeHandlerContext&)>;

struct WorkflowNodeHandler {
  std::string type;
  WorkflowNodeHandlerFn execute;
};

struct HumanWorkflowNodeProviderParams {
  const WorkflowNode& node;
  const Value& input;
  const WorkflowRunState& state;
};

using HumanWorkflowResponseProvider = std::function<Value(const HumanWorkflowNodeProviderParams&)>;

WorkflowNodeHandler create_human_workflow_node_handler(HumanWorkflowResponseProvider provider);
WorkflowNodeHandler create_webhook_workflow_node_handler(HttpTransport transport);

class WorkflowNodeRegistry {
 public:
  explicit WorkflowNodeRegistry(std::vector<WorkflowNodeHandler> handlers = {});
  WorkflowNodeRegistry(const WorkflowNodeRegistry& other);
  WorkflowNodeRegistry& operator=(const WorkflowNodeRegistry& other);
  WorkflowNodeRegistry(WorkflowNodeRegistry&& other) noexcept;
  WorkflowNodeRegistry& operator=(WorkflowNodeRegistry&& other) noexcept;
  WorkflowNodeHandler& register_handler(WorkflowNodeHandler handler);
  [[nodiscard]] const WorkflowNodeHandler* get(const std::string& type) const;
  [[nodiscard]] std::optional<WorkflowNodeHandler> find(const std::string& type) const;
  [[nodiscard]] std::vector<WorkflowNodeHandler> list() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, WorkflowNodeHandler> handlers_;
};

WorkflowNodeRegistry create_default_workflow_node_registry();

using WorkflowAgentHandler = std::function<Value(const Value&, const WorkflowRunState&)>;

struct WorkflowAgentDefinition {
  std::string id;
  WorkflowAgentHandler run;
  std::shared_ptr<AgentRunner> runner;
  std::string description;
};

class WorkflowAgentRegistry {
 public:
  explicit WorkflowAgentRegistry(std::vector<WorkflowAgentDefinition> agents = {});
  WorkflowAgentRegistry(const WorkflowAgentRegistry& other);
  WorkflowAgentRegistry& operator=(const WorkflowAgentRegistry& other);
  WorkflowAgentRegistry(WorkflowAgentRegistry&& other) noexcept;
  WorkflowAgentRegistry& operator=(WorkflowAgentRegistry&& other) noexcept;
  WorkflowAgentDefinition& register_agent(WorkflowAgentDefinition agent);
  WorkflowAgentDefinition& register_runner(std::string id,
                                           std::shared_ptr<AgentRunner> runner,
                                           std::string description = {});
  [[nodiscard]] const WorkflowAgentDefinition* get(const std::string& id) const;
  [[nodiscard]] std::optional<WorkflowAgentDefinition> find(const std::string& id) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, WorkflowAgentDefinition> agents_;
};

Value workflow_value_to_value(const WorkflowValue& value);
WorkflowValue workflow_value_from_value(const Value& value);
Value workflow_condition_to_value(const WorkflowCondition& condition);
WorkflowCondition workflow_condition_from_value(const Value& value);
Value workflow_node_route_to_value(const WorkflowNodeRoute& route);
WorkflowNodeRoute workflow_node_route_from_value(const Value& value);
Value workflow_node_wait_state_to_value(const WorkflowNodeWaitState& waiting);
WorkflowNodeWaitState workflow_node_wait_state_from_value(const Value& value);
Value workflow_node_to_value(const WorkflowNode& node);
WorkflowNode workflow_node_from_value(const Value& value);
Value workflow_edge_to_value(const WorkflowEdge& edge);
WorkflowEdge workflow_edge_from_value(const Value& value);
Value workflow_definition_to_value(const WorkflowDefinition& definition);
WorkflowDefinition workflow_definition_from_value(const Value& value);
Value workflow_node_state_to_value(const WorkflowNodeState& state);
WorkflowNodeState workflow_node_state_from_value(const Value& value);
Value workflow_checkpoint_to_value(const WorkflowCheckpoint& checkpoint);
WorkflowCheckpoint workflow_checkpoint_from_value(const Value& value);
Value workflow_run_state_to_value(const WorkflowRunState& state);
WorkflowRunState workflow_run_state_from_value(const Value& value);
Value workflow_execution_result_to_value(const WorkflowExecutionResult& result);
WorkflowRunState initialize_workflow_run_state(const WorkflowDefinition& definition,
                                               std::string workflow_run_id,
                                               Value input = Value::object({}),
                                               Value context = Value::object({}));

class WorkflowStore {
 public:
  virtual ~WorkflowStore() = default;
  virtual WorkflowRunState create_run(const WorkflowDefinition& definition,
                                      Value input = Value::object({}),
                                      Value context = Value::object({})) = 0;
  virtual std::optional<WorkflowRunState> get_run(const std::string& workflow_run_id) = 0;
  virtual void save_run(const WorkflowRunState& state) = 0;
};

class InMemoryWorkflowStore : public WorkflowStore {
 public:
  WorkflowRunState create_run(const WorkflowDefinition& definition,
                              Value input = Value::object({}),
                              Value context = Value::object({})) override;
  std::optional<WorkflowRunState> get_run(const std::string& workflow_run_id) override;
  void save_run(const WorkflowRunState& state) override;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, WorkflowRunState> runs_;
};

struct FileWorkflowStoreConfig {
  std::filesystem::path base_dir;
};

class FileWorkflowStore : public WorkflowStore {
 public:
  explicit FileWorkflowStore(std::filesystem::path base_dir);
  explicit FileWorkflowStore(FileWorkflowStoreConfig config);
  WorkflowRunState create_run(const WorkflowDefinition& definition,
                              Value input = Value::object({}),
                              Value context = Value::object({})) override;
  std::optional<WorkflowRunState> get_run(const std::string& workflow_run_id) override;
  void save_run(const WorkflowRunState& state) override;
  [[nodiscard]] std::filesystem::path file_path(const std::string& workflow_run_id) const;

 private:
  mutable std::mutex mutex_;
  std::filesystem::path base_dir_;
};

WorkflowValue workflow_literal(Value value);
WorkflowValue workflow_ref(std::string path);
WorkflowValue workflow_template(std::string templ);
WorkflowCondition workflow_truthy(WorkflowValue value);
WorkflowCondition workflow_equals(WorkflowValue left, WorkflowValue right);
WorkflowCondition workflow_not_empty(WorkflowValue value);

struct WorkflowTransformOptions {
  std::map<std::string, WorkflowValue> inputs;
  std::string wait_for = "all";
  std::size_t max_visits = 1;
};

struct WorkflowToolOptions {
  std::map<std::string, WorkflowValue> inputs;
  std::string wait_for = "all";
  std::size_t max_visits = 1;
};

struct WorkflowAgentOptions {
  std::map<std::string, WorkflowValue> inputs;
  std::string wait_for = "all";
  std::size_t max_visits = 1;
};

struct WorkflowConditionOptions {
  std::map<std::string, WorkflowValue> inputs;
  std::string wait_for = "all";
  std::size_t max_visits = 1;
};

struct WorkflowHumanWaitOptions {
  std::map<std::string, WorkflowValue> inputs;
  std::string wait_for = "all";
  std::size_t max_visits = 1;
  std::string prompt;
  std::string response_key = "response";
  Value metadata = Value::object({});
};

struct WorkflowArtifactOptions {
  std::map<std::string, WorkflowValue> inputs;
  std::string wait_for = "all";
  std::size_t max_visits = 1;
  std::string mode = "get";
  std::optional<WorkflowValue> value;
  std::string output_key = "value";
};

struct WorkflowRouterOptions {
  std::map<std::string, WorkflowValue> inputs;
  std::string wait_for = "all";
  std::size_t max_visits = 1;
  std::vector<WorkflowNodeRoute> routes;
  std::string default_route;
};

struct WorkflowJoinNodeOptions {
  std::map<std::string, WorkflowValue> inputs;
  std::string wait_for = "all";
  std::size_t max_visits = 1;
};

struct WorkflowWebhookWaitOptions {
  std::map<std::string, WorkflowValue> inputs;
  std::string wait_for = "all";
  std::size_t max_visits = 1;
  std::string prompt;
  std::string event_key = "payload";
  Value metadata = Value::object({});
};

struct WorkflowEndOptions {
  std::map<std::string, WorkflowValue> inputs;
  std::string wait_for = "all";
  std::size_t max_visits = 1;
  std::optional<WorkflowValue> result;
};

struct WorkflowEngineConfig {
  ToolRegistry* tools = nullptr;
  ToolExecutor* tool_executor = nullptr;
  ToolExecutionServices tool_services;
  WorkflowAgentRegistry* agent_registry = nullptr;
  WorkflowStore* store = nullptr;
  WorkflowNodeRegistry* node_registry = nullptr;
  HookSet hooks;
  EventBus* event_bus = nullptr;
  ExecutionPolicies execution_policies;
};

struct WorkflowRunInput {
  WorkflowDefinition definition;
  Value input;
  Value context = Value::object({});
};

struct WorkflowResumeInput {
  std::string workflow_run_id;
  std::optional<WorkflowDefinition> definition;
};

struct WorkflowHumanResponseInput {
  std::string workflow_run_id;
  std::string node_id;
  Value payload;
};

struct WorkflowWebhookPayloadInput {
  std::string workflow_run_id;
  std::string node_id;
  Value payload;
};

class WorkflowBuilder {
 public:
  explicit WorkflowBuilder(std::string id, std::string title = {});
  WorkflowBuilder& add_node(WorkflowNode node);
  WorkflowBuilder& add_edge(WorkflowEdge edge);
  WorkflowBuilder& start(std::string id = "start");
  WorkflowBuilder& transform(std::string id, std::map<std::string, WorkflowValue> inputs = {});
  WorkflowBuilder& transform(std::string id, WorkflowTransformOptions options);
  WorkflowBuilder& tool(std::string id, std::string tool_name, std::map<std::string, WorkflowValue> inputs = {});
  WorkflowBuilder& tool(std::string id, std::string tool_name, WorkflowToolOptions options);
  WorkflowBuilder& agent(std::string id, std::string agent_id, std::map<std::string, WorkflowValue> inputs = {});
  WorkflowBuilder& agent(std::string id, std::string agent_id, WorkflowAgentOptions options);
  WorkflowBuilder& condition(std::string id, WorkflowCondition condition);
  WorkflowBuilder& condition(std::string id, WorkflowCondition condition, WorkflowConditionOptions options);
  WorkflowBuilder& human_wait(std::string id, std::string prompt = {}, std::string response_key = "response",
                              Value metadata = Value::object({}));
  WorkflowBuilder& human_wait(std::string id, WorkflowHumanWaitOptions options);
  WorkflowBuilder& artifact(std::string id, std::string key, std::string mode = "get",
                            std::optional<WorkflowValue> value = std::nullopt,
                            std::string output_key = "value");
  WorkflowBuilder& artifact(std::string id, std::string key, WorkflowArtifactOptions options);
  WorkflowBuilder& router(std::string id, std::vector<WorkflowNodeRoute> routes = {},
                          std::string default_route = {});
  WorkflowBuilder& router(std::string id, WorkflowRouterOptions options);
  WorkflowBuilder& join_node(std::string id, std::string wait_for = "all");
  WorkflowBuilder& join_node(std::string id, WorkflowJoinNodeOptions options);
  WorkflowBuilder& webhook_wait(std::string id, std::string prompt = {}, std::string event_key = "payload",
                                Value metadata = Value::object({}));
  WorkflowBuilder& webhook_wait(std::string id, WorkflowWebhookWaitOptions options);
  WorkflowBuilder& end(std::string id = "end", std::optional<WorkflowValue> result = std::nullopt);
  WorkflowBuilder& end(std::string id, WorkflowEndOptions options);
  WorkflowBuilder& connect(std::string from, std::string to, std::optional<WorkflowCondition> condition = std::nullopt);
  WorkflowBuilder& branch(std::string from,
                          std::vector<std::pair<std::string, std::optional<WorkflowCondition>>> branches);
  WorkflowBuilder& parallel(std::string from, std::vector<std::string> to_node_ids);
  WorkflowBuilder& join(const std::vector<std::string>& from_node_ids, std::string to_node_id,
                        std::string wait_for = "all");
  WorkflowBuilder& sequence(const std::vector<std::string>& node_ids);
  [[nodiscard]] WorkflowDefinition build() const;

 private:
  WorkflowDefinition definition_;
};

WorkflowBuilder create_workflow(std::string id, std::string title = {});
WorkflowNode create_node(WorkflowNode node);

class WorkflowEngine {
 public:
  explicit WorkflowEngine(WorkflowEngineConfig config);
  explicit WorkflowEngine(ToolRegistry* tools = nullptr, ToolExecutor* tool_executor = nullptr,
                          WorkflowStore* store = nullptr,
                          WorkflowNodeRegistry* node_registry = nullptr,
                          WorkflowAgentRegistry* agent_registry = nullptr,
                          ToolExecutionServices tool_services = {},
                          HookSet hooks = {});
  [[nodiscard]] WorkflowStore* store() const noexcept;
  [[nodiscard]] EventBus* event_bus() const noexcept;
  std::size_t register_event_sink(EventBus::Sink sink);
  void unregister_event_sink(std::size_t sink_id);
  WorkflowAgentDefinition& register_agent(WorkflowAgentDefinition agent);
  WorkflowAgentDefinition& register_agent_runner(std::string id,
                                                 std::shared_ptr<AgentRunner> runner,
                                                 std::string description = {});
  WorkflowNodeHandler& register_node(WorkflowNodeHandler handler);
  WorkflowExecutionResult run(WorkflowRunInput input);
  WorkflowExecutionResult run(const WorkflowDefinition& definition, Value input = Value::object({}),
                              Value context = Value::object({}));
  WorkflowExecutionResult resume(WorkflowResumeInput input);
  WorkflowExecutionResult resume(const std::string& workflow_run_id,
                                 std::optional<WorkflowDefinition> definition = std::nullopt);
  WorkflowExecutionResult submit_human_response(WorkflowHumanResponseInput input);
  WorkflowExecutionResult submit_human_response(const std::string& workflow_run_id,
                                                const std::string& node_id,
                                                Value payload);
  WorkflowExecutionResult submit_webhook_payload(WorkflowWebhookPayloadInput input);
  WorkflowExecutionResult submit_webhook_payload(const std::string& workflow_run_id,
                                                 const std::string& node_id,
                                                 Value payload);

 private:
  WorkflowCheckpoint append_checkpoint(WorkflowRunState& state, WorkflowCheckpoint checkpoint) const;
  void append_event(WorkflowRunState& state, std::string category, Value payload = Value::object({})) const;
  TraceContext ensure_trace_context(WorkflowRunState& state) const;
  void publish_event(std::string category, ExecutionTarget target, Value payload, TraceContext trace = {}) const;
  WorkflowExecutionResult execute_state(WorkflowRunState state, const WorkflowDefinition& definition,
                                        bool resumed);
  Value source_for_state(const WorkflowRunState& state) const;
  Value resolve_value(const WorkflowValue& value, const WorkflowRunState& state) const;
  bool evaluate_condition(const WorkflowCondition& condition, const WorkflowRunState& state) const;
  Value get_artifact(WorkflowRunState& state, const std::string& key) const;
  Value set_artifact(WorkflowRunState& state, const std::string& key, const Value& value) const;
  std::optional<Value> consume_signal(WorkflowRunState& state, const std::string& type,
                                      const std::string& node_id) const;
  Value execute_node(const WorkflowNode& node, WorkflowRunState& state);
  std::vector<const WorkflowNode*> ready_nodes(const WorkflowDefinition& definition,
                                               const WorkflowRunState& state) const;
  void activate_outbound_edges(const WorkflowDefinition& definition, WorkflowRunState& state,
                               const WorkflowNode& node);

  ToolRegistry* tools_;
  ToolExecutor* tool_executor_;
  WorkflowStore* store_;
  WorkflowNodeRegistry node_registry_;
  WorkflowAgentRegistry agent_registry_;
  ToolExecutionServices tool_services_;
  HookSet hooks_;
  EventBus owned_event_bus_;
  EventBus* event_bus_ = nullptr;
  ExecutionPolicies execution_policies_;
};

}  // namespace agent
