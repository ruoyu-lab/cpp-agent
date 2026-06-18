#pragma once

#include "agent/react.hpp"
#include "agent/runtime.hpp"
#include "../detail/helpers.hpp"
#include "../function_calling_loop.hpp"

#include <cstddef>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agent {

struct AgentRunnerResolvedConfig {
  std::shared_ptr<ChatModelAdapter> adapter;
  std::shared_ptr<ChatModelAdapter> thinking_adapter;
  std::shared_ptr<ChatModelAdapter> critique_adapter;
  std::vector<ToolDefinition> tools;
  std::vector<ContextSource> contexts;
  std::string system_prompt;
  int max_iterations = 8;
  ModelSettings model_settings;
  std::shared_ptr<SessionStore> memory_store;
  std::shared_ptr<ScratchStore> scratch_store;
  std::shared_ptr<LongTermMemoryPort> long_term_memory;
  KnowledgeContextProvider* knowledge_provider = nullptr;
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
  SkillConflictPolicy skill_model_conflict_policy = SkillConflictPolicy::Error;
  SkillConflictPolicy skill_effort_conflict_policy = SkillConflictPolicy::Error;
  bool advertise_skills = true;
  SkillForkHandler skill_fork_handler;
  std::map<std::string, AgentRunner*> skill_subagents;
  bool lazy_tool_mode = false;
  std::vector<std::string> forced_visible_tools;
  AgentToolCallingStrategy tool_calling_strategy = AgentToolCallingStrategy::TextReAct;
  int max_parse_errors = 2;
  ReActPromptMode react_prompt_mode = ReActPromptMode::Managed;
  ReActParserOptions react_parser_options;
  std::shared_ptr<ReActPromptBuilder> react_prompt_builder;
  std::vector<ContextStatsBucketInput> prompt_stats_buckets;
  bool persist_react_visible_messages = false;
  std::optional<std::size_t> context_window_tokens;
  ContextTokenCounter context_token_counter;
};

struct SkillForkExecution {
  ResolvedSkillUse use;
  SkillForkResult result;
  std::string agent;
  std::string session_id;
};

struct SkillPrefaceBuildResult {
  std::vector<AgentMessage> messages;
  std::vector<SkillForkExecution> fork_executions;
};

struct RunnerMemoryRetrievalResult {
  std::vector<RetrievedMemory> hits;
  std::optional<AgentMessage> message;
};

struct RunnerKnowledgeRetrievalResult {
  std::vector<KnowledgeSearchHit> hits;
  std::optional<AgentMessage> message;
  Value debug = Value::object({});
};

struct RunnerKnowledgeQuery {
  enum class Kind {
    None,
    Text,
    Image,
  };

  Kind kind = Kind::None;
  std::string text;
  ImageEmbeddingInput image;
};

struct RunnerResolvedInvocation {
  AgentMessage input_message;
  Value input_value;
  std::string input;
  std::vector<MessageContentPart> input_parts;
  Value run_input_value;
  std::string session_id;
  std::string run_id;
  Value run_hook_context_value;
  TraceContext run_trace;
  ModelSettings base_model_settings;
  ResolvedSkillsState skill_state;
  ModelSettings effective_model_settings;
  std::string effective_input;
  Value effective_input_value;
  AgentMessage effective_input_message;
  std::string input_text;
  RunnerKnowledgeQuery knowledge_query;
};

struct RunnerPreparedState {
  SkillPrefaceBuildResult skill_preface;
  std::vector<AgentMessage> preface_messages;
  std::vector<RetrievedMemory> memory_hits;
  std::vector<KnowledgeSearchHit> knowledge_hits;
  Value knowledge_debug = Value::object({});
  std::optional<ExecutionPlan> plan;
};

struct RunnerLoopSetup {
  Value loop_context = Value::object({});
  ToolExecutionContext tool_context;
};

bool model_settings_empty(const ModelSettings& settings);
TraceContext child_or_root_trace_context(const TraceContext& parent,
                                         std::string span_name = {},
                                         TraceContext overrides = {});
TraceContext runner_trace_context(const Value& context,
                                  const std::string& run_id,
                                  std::string span_name);
Value string_array_value(const std::vector<std::string>& values);
Value message_content_parts_value(const std::vector<MessageContentPart>& parts);
Value model_response_hook_value(const AgentOutput& response);
Value retrieved_memory_hook_value(const RetrievedMemory& memory);
Value retrieved_memories_hook_value(const std::vector<RetrievedMemory>& hits);
Value knowledge_hits_value(const std::vector<KnowledgeSearchHit>& hits);
Value execution_plan_summary_value(const ExecutionPlan& plan);
Value run_started_event_payload(Value input,
                                const std::string& session_id,
                                const Value& context,
                                const std::string& run_id = {});
Value run_completed_event_payload(const AgentRunnerRunResult& result,
                                  const std::string& run_id = {});
Value run_failed_event_payload(Value input,
                               const std::string& session_id,
                               const std::string& error,
                               const std::string& run_id = {});
Value run_cancelled_event_payload(Value input,
                                  const std::string& session_id,
                                  const CancellationError& error,
                                  const std::string& run_id = {});
std::string synthetic_tool_failure_message(const ToolCall& tool_call,
                                           const std::exception& error);
ToolExecutionResult synthetic_tool_failure_result(const ToolCall& tool_call,
                                                  std::string error_message);
Value tool_execution_structured_output_value(const ToolExecutionResult& result);
Value retry_scheduled_event_payload(const RetryScheduledContext& retry,
                                    Value extra = Value::object({}));
AgentRunnerStatus runner_status(std::string kind,
                                std::string stage,
                                std::string state,
                                std::string message,
                                Value details = Value::object({}));
std::optional<AgentRunnerStatus> status_from_loop_event(const AgentLoopStreamEvent& event);
Value runner_loop_context(Value context,
                          const std::optional<ExecutionPlan>& plan,
                          const std::vector<RetrievedMemory>& memory_hits,
                          const std::vector<KnowledgeSearchHit>& knowledge_hits,
                          const Value& knowledge_debug);
bool skill_requests_fork(const ResolvedSkillUse& entry);
AgentMessage skill_active_message(const ResolvedSkillUse& entry);
SkillPrefaceBuildResult build_skill_preface_messages(AgentRunner* owner,
                                                     const AgentRunnerResolvedConfig& config,
                                                     const ResolvedSkillsState& state,
                                                     const std::string& input_text,
                                                     const std::string& session_id,
                                                     const Value& context);
Value skill_services_value(const ResolvedSkillsState& state,
                           const std::vector<SkillForkExecution>& fork_executions = {});
ToolExecutionServices runner_runtime_services(SessionMemory* session,
                                               const std::string& session_id,
                                               LongTermMemoryPort* long_term_memory,
                                               KnowledgeContextProvider* knowledge_provider,
                                               const Value& context,
                                               const std::optional<Value>& skills);
RunnerRetrievalOptions merge_runner_retrieval_options(const RunnerRetrievalOptions& base,
                                                      const RunnerRetrievalOptions& override);
bool runner_retrieval_enabled(const RunnerRetrievalOptions& options);
RunnerKnowledgeQuery text_knowledge_query(std::string text);
RunnerKnowledgeQuery resolve_runner_knowledge_query(const AgentMessage& message);
std::string runner_knowledge_query_text(const RunnerKnowledgeQuery& query);
bool runner_knowledge_query_empty(const RunnerKnowledgeQuery& query);
RunnerMemoryRetrievalResult retrieve_long_term_memory_context(LongTermMemoryPort& memory,
                                                              const std::string& query,
                                                              const RunnerRetrievalOptions& options,
                                                              const HookSet& hooks,
                                                              EventBus* event_bus,
                                                              const ExecutionPolicies& execution_policies,
                                                              CancellationToken* cancellation);
RunnerKnowledgeRetrievalResult retrieve_knowledge_context(KnowledgeContextProvider* knowledge_provider,
                                                          const RunnerKnowledgeQuery& query,
                                                          const RunnerKnowledgeRetrievalOptions& options,
                                                          const HookSet& hooks,
                                                          EventBus* event_bus,
                                                          const ExecutionPolicies& execution_policies,
                                                          CancellationToken* cancellation);
std::optional<ExecutionPlan> create_runner_execution_plan(const AgentRunnerResolvedConfig& config,
                                                          const std::string& input,
                                                          SessionMemory* session,
                                                          const Value& context,
                                                          ToolRegistry* tool_registry,
                                                          const std::vector<RetrievedMemory>& memory_hits,
                                                          const std::vector<KnowledgeSearchHit>& knowledge_hits,
                                                          EventBus* event_bus,
                                                          CancellationToken* cancellation,
                                                          bool enable_planning);
AgentRunnerRunResult runner_result_from_loop(AgentLoopRunResult base,
                                             std::vector<RetrievedMemory> memory_hits,
                                             std::vector<KnowledgeSearchHit> knowledge_hits = {},
                                             Value knowledge_debug = Value::object({}),
                                             std::optional<ExecutionPlan> plan = std::nullopt);

AgentMessage user_input_message(std::vector<MessageContentPart> input_parts);
AgentMessage input_message_from_durable_state(const AgentLoopDurableState& state);
RunnerResolvedInvocation resolve_runner_invocation(const AgentRunnerResolvedConfig& config,
                                                   AgentMessage input_message,
                                                   Value input_value,
                                                   bool allow_skill_input_rewrite,
                                                   const std::string& session_id,
                                                   const ModelSettings& model_settings,
                                                   std::vector<SkillActivation> skill_activations,
                                                   Value context,
                                                   EventBus* event_bus,
                                                   const AgentRunnerDurableState* resume_state,
                                                   std::string span_name);

Value stream_error_payload(const AgentFrameworkError& error);
Value stream_cancellation_payload(const CancellationError& error);
Value stream_error_payload(const std::exception& error);

RunnerPreparedState prepare_runner_state(AgentRunner* owner,
                                         const AgentRunnerResolvedConfig& config,
                                         ToolRegistry& tool_registry,
                                         SessionMemory& session,
                                         const RunnerResolvedInvocation& invocation,
                                         const RunnerRetrievalOptions& retrieval,
                                         const RunnerKnowledgeRetrievalOptions& knowledge_retrieval,
                                         EventBus* event_bus,
                                         CancellationToken* cancellation,
                                         bool enable_planning,
                                         const AgentRunnerDurableState* resume_state = nullptr,
                                         AgentRunnerStreamEventHandler on_event = {});
RunnerLoopSetup build_runner_loop_setup(const AgentRunnerResolvedConfig& config,
                                        ScratchStore* scratch_store,
                                        SessionMemory& session,
                                        const RunnerResolvedInvocation& invocation,
                                        const RunnerPreparedState& prepared,
                                        CancellationToken* cancellation);
ReActLoop create_text_react_runner_loop(const AgentRunnerResolvedConfig& config,
                                        ToolRegistry& tool_registry,
                                        ToolExecutor& executor,
                                        EmbeddedContextManager& context_manager,
                                        EventBus* event_bus,
                                        const TraceContext& trace,
                                        std::function<void(const ContextStatsSnapshot&)> on_context_stats = {});
internal::AgentLoop create_native_tool_calling_runner_loop(
    const AgentRunnerResolvedConfig& config,
    ToolRegistry& tool_registry,
    ToolExecutor& executor,
    EmbeddedContextManager& context_manager,
    EventBus* event_bus,
    const TraceContext& trace,
    std::function<void(const ContextStatsSnapshot&)> on_context_stats = {});
void emit_runner_stream_event(const AgentRunnerStreamEventHandler& on_event,
                              AgentRunnerStreamEvent event);
void append_runner_loop_stream_event(const AgentRunnerStreamEventHandler& on_event,
                                     AgentLoopStreamEvent event);

}  // namespace agent
