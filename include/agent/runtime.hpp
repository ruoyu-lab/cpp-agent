#pragma once

#include "agent/skills.hpp"
#include "agent/workflow.hpp"

#include <limits>
#include <mutex>
#include <optional>

namespace agent {

struct AgentTraceEntry {
  std::string type;
  int iteration = 0;
  ModelResponse response;
  std::vector<ToolExecutionResult> tool_results;
};

enum class AgentLoopTerminationReason {
  Completed,
  MaxIterations,
  IncompleteResponse,
};

std::string to_string(AgentLoopTerminationReason reason);

enum class AgentLoopDurablePhase {
  BeforeModel,
  ModelCompleted,
  ToolsCompleted,
  Completed,
};

std::string to_string(AgentLoopDurablePhase phase);

struct AgentLoopRunResult {
  std::string session_id;
  int iteration_count = 0;
  std::string text;
  ModelResponse response;
  std::vector<AgentTraceEntry> trace;
  std::vector<AgentMessage> messages;
  AgentLoopTerminationReason termination_reason = AgentLoopTerminationReason::Completed;
  ModelUsage usage;
};

struct AgentLoopDurableState {
  int version = 1;
  AgentLoopDurablePhase phase = AgentLoopDurablePhase::BeforeModel;
  std::string session_id;
  int next_iteration = 0;
  std::string input_text;
  std::vector<MessageContentPart> input_parts;
  AgentMessage input_message;
  Value input_value;
  SessionMemorySnapshot session;
  std::vector<AgentTraceEntry> trace;
  std::optional<ModelResponse> last_response;
  std::string last_assistant_text;
  std::optional<AgentLoopTerminationReason> termination_reason;
};

using AgentLoopCheckpointHandler = std::function<void(const AgentLoopDurableState&)>;

struct AgentLoopDurableOptions {
  std::optional<AgentLoopDurableState> resume_state;
  AgentLoopCheckpointHandler on_checkpoint;
};

Value agent_loop_durable_state_to_value(const AgentLoopDurableState& state);

enum class AgentLoopStreamEventType {
  IterationStart,
  ModelStart,
  ModelTextDelta,
  ModelReasoningDelta,
  ModelResponse,
  ToolCallArgumentDelta,
  ToolStart,
  ToolComplete,
  Tools,
  Done,
};

std::string to_string(AgentLoopStreamEventType type);

struct AgentLoopStreamEvent {
  AgentLoopStreamEventType type = AgentLoopStreamEventType::IterationStart;
  int iteration = 0;
  std::string provider;
  std::string model;
  std::string delta;
  std::string text;
  std::string reasoning;
  ModelResponse response;
  ToolCall tool_call;
  ToolExecutionResult tool_result;
  std::vector<ToolExecutionResult> tool_results;
  AgentLoopRunResult result;
  // Populated only when type == ToolCallArgumentDelta.
  std::string tool_call_id;
  std::string tool_call_name;
  std::string tool_call_args_delta;
  std::string tool_call_args_accumulated;
};

struct AgentLoopStreamResult {
  std::vector<AgentLoopStreamEvent> events;
  AgentLoopRunResult result;
};

struct AgentLoopConfig {
  std::shared_ptr<ChatModelAdapter> model;
  ToolRegistry* tool_registry = nullptr;
  ToolExecutor* tool_executor = nullptr;
  EmbeddedContextManager* context_manager = nullptr;
  std::string system_prompt;
  int max_iterations = 8;
  EventBus* event_bus = nullptr;
  ExecutionPolicies execution_policies;
  HookSet hooks;
  TraceContext trace_context;
};

class AgentLoop {
 public:
  explicit AgentLoop(AgentLoopConfig config);
  AgentLoopRunResult run(SessionMemory& session, const std::string& input,
                         const ModelSettings& model_settings = {},
                         const std::vector<AgentMessage>& preface_messages = {},
                         ToolExecutionContext tool_context = {},
                         Value runtime_context = Value::object({}),
                         AgentLoopDurableOptions durable_options = {},
                         CancellationToken* cancellation = nullptr);
  AgentLoopRunResult run(SessionMemory& session, std::vector<MessageContentPart> input_parts,
                         const ModelSettings& model_settings = {},
                         const std::vector<AgentMessage>& preface_messages = {},
                         ToolExecutionContext tool_context = {},
                         Value runtime_context = Value::object({}),
                         AgentLoopDurableOptions durable_options = {},
                         CancellationToken* cancellation = nullptr);
  AgentLoopRunResult run(SessionMemory& session, AgentMessage input_message,
                         const ModelSettings& model_settings = {},
                         const std::vector<AgentMessage>& preface_messages = {},
                         ToolExecutionContext tool_context = {},
                         Value runtime_context = Value::object({}),
                         AgentLoopDurableOptions durable_options = {},
                         CancellationToken* cancellation = nullptr);
  AgentLoopStreamResult stream(SessionMemory& session, const std::string& input,
                               const ModelSettings& model_settings = {},
                               const std::vector<AgentMessage>& preface_messages = {},
                               ToolExecutionContext tool_context = {},
                               Value runtime_context = Value::object({}),
                               CancellationToken* cancellation = nullptr);
  AgentLoopStreamResult stream(SessionMemory& session, std::vector<MessageContentPart> input_parts,
                               const ModelSettings& model_settings = {},
                               const std::vector<AgentMessage>& preface_messages = {},
                               ToolExecutionContext tool_context = {},
                               Value runtime_context = Value::object({}),
                               CancellationToken* cancellation = nullptr);
  AgentLoopStreamResult stream(SessionMemory& session, AgentMessage input_message,
                               const ModelSettings& model_settings = {},
                               const std::vector<AgentMessage>& preface_messages = {},
                               ToolExecutionContext tool_context = {},
                               Value runtime_context = Value::object({}),
                               CancellationToken* cancellation = nullptr);

 private:
  AgentLoopRunResult run_input(SessionMemory& session, AgentMessage input_message, Value input_value,
                               const ModelSettings& model_settings,
                               const std::vector<AgentMessage>& preface_messages,
                               ToolExecutionContext tool_context,
                               Value runtime_context,
                               AgentLoopDurableOptions durable_options,
                               CancellationToken* cancellation);
  AgentLoopStreamResult stream_input(SessionMemory& session, AgentMessage input_message, Value input_value,
                                     const ModelSettings& model_settings,
                                     const std::vector<AgentMessage>& preface_messages,
                                     ToolExecutionContext tool_context,
                                     Value runtime_context,
                                     CancellationToken* cancellation);
  std::vector<AgentMessage> build_prompt_messages(SessionMemory& session, const std::string& input,
                                                  const Value& input_value,
                                                  int iteration,
                                                  const std::vector<AgentTraceEntry>& trace,
                                                  const std::vector<AgentMessage>& preface_messages,
                                                  const Value& runtime_context) const;
  ModelResponse call_model(int iteration, const std::vector<AgentMessage>& messages,
                           const ModelSettings& model_settings,
                           CancellationToken* cancellation = nullptr);

  AgentLoopConfig config_;
};

struct RunnerRetrievalOptions {
  std::optional<bool> enabled;
  std::optional<std::size_t> top_k;
  std::optional<double> min_score;
  std::string namespace_id;
};

struct RunnerWritebackOptions {
  std::optional<bool> enabled;
  std::string namespace_id;
  Value metadata = Value::object({});
};

struct RunnerKnowledgeRetrievalOptions {
  bool enabled = true;
  std::size_t top_k = 0;
  std::size_t vector_top_k = 0;
  std::size_t lexical_top_k = 0;
  double min_score = std::numeric_limits<double>::quiet_NaN();
  double hybrid_alpha = std::numeric_limits<double>::quiet_NaN();
  std::size_t rerank_top_k = 0;
  std::string retrieval_mode;
  double oversample_factor = 0.0;
  std::string fusion;
  std::map<KnowledgeAssetType, double> modality_weights;
  std::string uri_prefix;
  std::vector<std::string> document_ids;
  std::vector<KnowledgeAssetType> asset_types;
  std::string space_id;
  std::vector<std::string> source_types;
  std::vector<std::string> chunk_ids;
  Value metadata = Value::object({});
  std::string tenant_id;
  std::vector<std::string> knowledge_base_ids;
};

struct AgentRunnerRunResult : public AgentLoopRunResult {
  std::vector<RetrievedMemory> memory_hits;
  std::vector<KnowledgeSearchHit> knowledge_hits;
  Value knowledge_debug = Value::object({});
  std::optional<ExecutionPlan> plan;
};

enum class AgentRunnerDurableStatus {
  Running,
  Completed,
  Interrupted,
};

std::string to_string(AgentRunnerDurableStatus status);

struct AgentRunnerDurableState {
  int version = 1;
  AgentRunnerDurableStatus status = AgentRunnerDurableStatus::Running;
  std::string run_id;
  std::string session_id;
  std::string input;
  Value input_value;
  AgentMessage input_message;
  ModelSettings model_settings;
  std::string effective_input;
  Value effective_input_value;
  AgentMessage effective_input_message;
  std::string input_text;
  std::vector<MessageContentPart> input_parts;
  std::optional<ExecutionPlan> plan;
  std::vector<RetrievedMemory> memory_hits;
  std::vector<KnowledgeSearchHit> knowledge_hits;
  Value knowledge_debug = Value::object({});
  std::vector<AgentMessage> preface_messages;
  std::optional<AgentLoopDurableState> loop;
  std::string updated_at;
};

using AgentRunnerCheckpointHandler = std::function<void(const AgentRunnerDurableState&)>;

struct AgentRunnerDurableOptions {
  std::optional<AgentRunnerDurableState> resume_state;
  AgentRunnerCheckpointHandler on_checkpoint;
};

Value agent_runner_durable_state_to_value(const AgentRunnerDurableState& state);

struct AgentRunnerStatus {
  std::string kind;
  std::string stage;
  std::string state;
  std::string message;
  int iteration = -1;
  std::string provider;
  std::string model;
  std::string tool_name;
  std::string tool_call_id;
  Value details = Value::object({});
};

enum class AgentRunnerStreamEventType {
  Status,
  KnowledgeRetrieval,
  MemoryRetrieval,
  Planning,
  Loop,
  ToolCallArgumentDelta,
  Done,
  Error,
};

std::string to_string(AgentRunnerStreamEventType type);

struct AgentRunnerStreamEvent {
  AgentRunnerStreamEventType type = AgentRunnerStreamEventType::Loop;
  AgentRunnerStatus status;
  std::vector<RetrievedMemory> memory_hits;
  std::optional<AgentMessage> memory_message;
  std::vector<KnowledgeSearchHit> knowledge_hits;
  std::optional<AgentMessage> knowledge_message;
  Value knowledge_debug = Value::object({});
  std::optional<ExecutionPlan> plan;
  AgentLoopStreamEvent loop_event;
  AgentRunnerRunResult result;
  Value error = Value::object({});
  // Populated only when type == ToolCallArgumentDelta.
  std::string tool_call_id;
  std::string tool_call_name;
  std::string tool_call_args_delta;
  std::string tool_call_args_accumulated;
};

struct AgentRunnerStreamResult {
  std::vector<AgentRunnerStreamEvent> events;
  AgentRunnerRunResult result;
};

class AgentRunner;

struct SkillForkRequest {
  ResolvedSkillUse skill;
  std::string input_text;
  std::string session_id;
  ModelSettings model_settings;
  Value context = Value::object({});
};

struct SkillForkResult {
  std::string text;
  Value metadata = Value::object({});
};

using SkillForkHandler = std::function<std::optional<SkillForkResult>(const SkillForkRequest&)>;

struct AgentRunnerConfig {
  std::shared_ptr<ChatModelAdapter> adapter;
  // Optional reasoning-pass adapter. Reserved for use by planners and other
  // modules that want a separate "thinking" model distinct from the main
  // generation adapter. The default loop does not invoke it directly.
  std::shared_ptr<ChatModelAdapter> thinking_adapter;
  // Optional self-review / LLM-as-judge adapter. The eval runner will pick
  // this up for LLM-judged assertions when present. Falls back to `adapter`
  // (with a self-grading warning) if unset.
  std::shared_ptr<ChatModelAdapter> critique_adapter;
  std::vector<ToolDefinition> tools;
  std::vector<ContextSource> contexts;
  std::string system_prompt;
  int max_iterations = 8;
  ModelSettings model_settings;
  std::shared_ptr<SessionStore> memory_store;
  // Optional injected scratchpad backend. When unset, the runner constructs a
  // default `InMemoryScratchStore`. Hosts can supply `FileScratchStore` for
  // restart-safe persistence or any custom `ScratchStore` implementation.
  std::shared_ptr<ScratchStore> scratch_store;
  std::shared_ptr<LongTermMemory> long_term_memory;
  KnowledgeBase* knowledge_base = nullptr;
  KnowledgeBaseManager* knowledge_base_manager = nullptr;
  RunnerKnowledgeRetrievalOptions knowledge_retrieval_options;
  RunnerRetrievalOptions retrieval_options;
  RunnerWritebackOptions writeback_options;
  EventBus* event_bus = nullptr;
  ExecutionPolicies execution_policies;
  HookSet hooks;
  std::shared_ptr<Planner> planner;
  bool enable_planning = true;
  PermissionPolicy permission_policy;
  PermissionApprovalHandler approval_handler;
  ToolExecutionServices tool_services;
  std::shared_ptr<SkillRegistry> skills;
  std::vector<std::string> default_skills;
  // Conflict policies applied when activated skills disagree about which
  // model or reasoning-effort budget to use.
  SkillConflictPolicy skill_model_conflict_policy = SkillConflictPolicy::Error;
  SkillConflictPolicy skill_effort_conflict_policy = SkillConflictPolicy::Error;
  bool advertise_skills = true;
  SkillForkHandler skill_fork_handler;
  std::map<std::string, AgentRunner*> skill_subagents;
  // Opt-in lazy tool advertising. When `lazy_tool_mode == true`, after the
  // runner constructs `tool_registry_` from `tools`, it calls
  // `tool_registry_.set_lazy_mode(true)` and then `force_visible(name)` for
  // each entry in `forced_visible_tools`. Use this to keep MCP / Skill tools
  // out of the system prompt while leaving them callable by name (the agent
  // discovers them via the built-in `tool.search` / `tool.describe`).
  bool lazy_tool_mode = false;
  std::vector<std::string> forced_visible_tools;
};

class AgentRunner {
 public:
  explicit AgentRunner(const AgentRunnerConfig& config);

  ToolDefinition& register_tool(ToolDefinition tool);
  ContextSource& register_context(ContextSource source);
  void set_approval_handler(PermissionApprovalHandler approval_handler);
  std::size_t register_event_sink(EventBus::Sink sink);
  void unregister_event_sink(std::size_t sink_id);
  std::shared_ptr<SessionMemory> get_session(const std::string& session_id = "default");
  [[nodiscard]] SessionStore* session_store() const noexcept;
  [[nodiscard]] ScratchStore* scratch_store() const noexcept;
  [[nodiscard]] std::shared_ptr<ChatModelAdapter> adapter() const noexcept;
  [[nodiscard]] std::shared_ptr<ChatModelAdapter> thinking_adapter() const noexcept;
  [[nodiscard]] std::shared_ptr<ChatModelAdapter> critique_adapter() const noexcept;
  [[nodiscard]] EventBus* event_bus() const noexcept;
  AgentRunnerRunResult run(const std::string& input, const std::string& session_id = "default",
                           const ModelSettings& model_settings = {},
                           RunnerRetrievalOptions retrieval_options = {},
                           RunnerWritebackOptions writeback_options = {},
                           std::vector<SkillActivation> skill_activations = {},
                           Value context = Value::object({}),
                           std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                           AgentRunnerDurableOptions durable_options = {},
                           CancellationToken* cancellation = nullptr,
                           bool enable_planning = true);
  AgentRunnerRunResult run(std::vector<MessageContentPart> input_parts, const std::string& session_id = "default",
                           const ModelSettings& model_settings = {},
                           RunnerRetrievalOptions retrieval_options = {},
                           RunnerWritebackOptions writeback_options = {},
                           std::vector<SkillActivation> skill_activations = {},
                           Value context = Value::object({}),
                           std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                           AgentRunnerDurableOptions durable_options = {},
                           CancellationToken* cancellation = nullptr,
                           bool enable_planning = true);
  AgentRunnerRunResult run(AgentMessage input_message, const std::string& session_id = "default",
                           const ModelSettings& model_settings = {},
                           RunnerRetrievalOptions retrieval_options = {},
                           RunnerWritebackOptions writeback_options = {},
                           std::vector<SkillActivation> skill_activations = {},
                           Value context = Value::object({}),
                           std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                           AgentRunnerDurableOptions durable_options = {},
                           CancellationToken* cancellation = nullptr,
                           bool enable_planning = true);
  AgentRunnerStreamResult stream(const std::string& input, const std::string& session_id = "default",
                                 const ModelSettings& model_settings = {},
                                 RunnerRetrievalOptions retrieval_options = {},
                                 RunnerWritebackOptions writeback_options = {},
                                 std::vector<SkillActivation> skill_activations = {},
                                 Value context = Value::object({}),
                                 std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                                 CancellationToken* cancellation = nullptr,
                                 bool enable_planning = true);
  AgentRunnerStreamResult stream(std::vector<MessageContentPart> input_parts,
                                 const std::string& session_id = "default",
                                 const ModelSettings& model_settings = {},
                                 RunnerRetrievalOptions retrieval_options = {},
                                 RunnerWritebackOptions writeback_options = {},
                                 std::vector<SkillActivation> skill_activations = {},
                                 Value context = Value::object({}),
                                 std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                                 CancellationToken* cancellation = nullptr,
                                 bool enable_planning = true);
  AgentRunnerStreamResult stream(AgentMessage input_message,
                                 const std::string& session_id = "default",
                                 const ModelSettings& model_settings = {},
                                 RunnerRetrievalOptions retrieval_options = {},
                                 RunnerWritebackOptions writeback_options = {},
                                 std::vector<SkillActivation> skill_activations = {},
                                 Value context = Value::object({}),
                                 std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                                 CancellationToken* cancellation = nullptr,
                                 bool enable_planning = true);
  [[nodiscard]] std::vector<AgentRunnerStreamEvent> last_stream_events() const;

 private:
  AgentRunnerRunResult run_input(AgentMessage input_message, Value input_value,
                                 bool allow_skill_input_rewrite,
                                 const std::string& session_id,
                                 const ModelSettings& model_settings,
                                 RunnerRetrievalOptions retrieval_options,
                                 RunnerWritebackOptions writeback_options,
                                 std::vector<SkillActivation> skill_activations,
                                 Value context,
                                 std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                 AgentRunnerDurableOptions durable_options,
                                 CancellationToken* cancellation,
                                 bool enable_planning);
  AgentRunnerStreamResult stream_input(AgentMessage input_message, Value input_value,
                                       bool allow_skill_input_rewrite,
                                       const std::string& session_id,
                                       const ModelSettings& model_settings,
                                       RunnerRetrievalOptions retrieval_options,
                                       RunnerWritebackOptions writeback_options,
                                       std::vector<SkillActivation> skill_activations,
                                       Value context,
                                       std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                       CancellationToken* cancellation,
                                       bool enable_planning);
  void clear_last_stream_events();
  void store_last_stream_events(std::vector<AgentRunnerStreamEvent> events);

  AgentRunnerConfig config_;
  ToolRegistry tool_registry_;
  EmbeddedContextManager context_manager_;
  std::shared_ptr<SessionStore> memory_store_;
  std::shared_ptr<ScratchStore> scratch_store_;
  EventBus owned_event_bus_;
  mutable std::mutex last_stream_events_mutex_;
  std::vector<AgentRunnerStreamEvent> last_stream_events_;
};

}  // namespace agent
