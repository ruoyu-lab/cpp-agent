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

bool model_settings_empty(const ModelSettings& settings) {
  return settings.model.empty() && !settings.temperature && !settings.max_output_tokens && !settings.reasoning
         && settings.extra.as_object().empty();
}

bool has_trace_values(const TraceContext& trace) {
  return !trace.trace_id.empty() || !trace.span_id.empty() || !trace.parent_span_id.empty() ||
         !trace.span_name.empty() || !trace.run_id.empty() || !trace.workflow_run_id.empty();
}

TraceContext child_or_root_trace_context(const TraceContext& parent, std::string span_name = {},
                                         TraceContext overrides = {}) {
  const TraceContext* parent_ptr = has_trace_values(parent) ? &parent : nullptr;
  return derive_child_trace_context(parent_ptr,
                                    TraceSpanDescriptor{.name = std::move(span_name)},
                                    std::move(overrides));
}

TraceContext runner_trace_context(const Value& context, const std::string& run_id, std::string span_name) {
  if (const auto parent = get_trace_context(context)) {
    return derive_child_trace_context(*parent, std::move(span_name), TraceContext{.run_id = run_id});
  }
  return create_trace_context(TraceContext{.span_name = std::move(span_name), .run_id = run_id});
}

Value string_array_value(const std::vector<std::string>& values) {
  Value::Array array;
  array.reserve(values.size());
  for (const auto& value : values) {
    array.emplace_back(value);
  }
  return Value(std::move(array));
}

AgentMessage user_input_message(std::vector<MessageContentPart> input_parts) {
  return create_message(MessageRole::User, std::move(input_parts));
}

bool value_has_fields(const Value& value) {
  return !value.is_object() || !value.as_object().empty();
}

bool has_durable_input_message(const AgentMessage& message) {
  return message.role != MessageRole::User || !message.content.empty() || !message.name.empty()
         || !message.tool_call_id.empty() || !message.tool_calls.empty() || value_has_fields(message.metadata);
}

AgentMessage input_message_from_durable_state(const AgentLoopDurableState& state) {
  if (has_durable_input_message(state.input_message)) {
    return state.input_message;
  }
  if (!state.input_parts.empty()) {
    return user_input_message(state.input_parts);
  }
  if (!state.input_text.empty()) {
    return create_message(MessageRole::User, state.input_text);
  }
  return {};
}

Value message_content_parts_value(const std::vector<MessageContentPart>& parts) {
  return agent_message_to_value(create_message(MessageRole::User, parts)).at("content");
}

Value input_value_for_effective_input(const Value& input_value,
                                      const std::string& effective_input) {
  return input_value.is_string() ? Value(effective_input) : input_value;
}

std::vector<MessageContentPart> loop_input_parts_for_effective_input(
    const std::vector<MessageContentPart>& input_parts,
    const std::string& input_text,
    const std::string& effective_input) {
  if (effective_input != input_text) {
    std::vector<MessageContentPart> rewritten_parts;
    rewritten_parts.reserve(input_parts.size() + 1);
    bool wrote_effective_text = false;
    for (const auto& part : input_parts) {
      if (part.type == ContentPartType::Text) {
        if (!wrote_effective_text && !effective_input.empty()) {
          rewritten_parts.push_back(text_part(effective_input));
          wrote_effective_text = true;
        }
        continue;
      }
      rewritten_parts.push_back(part);
    }
    if (!wrote_effective_text && !effective_input.empty()) {
      rewritten_parts.insert(rewritten_parts.begin(), text_part(effective_input));
    }
    return rewritten_parts;
  }
  return input_parts;
}

AgentMessage input_message_for_effective_input(const AgentMessage& input_message,
                                               const std::string& input_text,
                                               const std::string& effective_input) {
  AgentMessage next = input_message;
  next.content = loop_input_parts_for_effective_input(input_message.content, input_text, effective_input);
  return next;
}

Value model_response_hook_value(const ModelResponse& response) {
  return Value::object({
      {"provider", response.provider},
      {"model", response.model},
      {"text", response.text},
      {"finishReason", response.finish_reason},
      {"raw", response.raw},
  });
}

void emit_cache_stats_event(EventBus& bus,
                            const ModelResponse& response,
                            const ModelSettings& settings,
                            const TraceContext& trace) {
  const auto usage = extract_model_usage(response);
  if (usage.input_tokens <= 0 && usage.cached_input_tokens <= 0 && usage.reasoning_tokens <= 0) {
    return;
  }
  const double hit_rate = usage.input_tokens > 0
                              ? static_cast<double>(usage.cached_input_tokens) /
                                    static_cast<double>(usage.input_tokens)
                              : 0.0;
  bus.publish("model.cache_stats", ExecutionTarget::Run,
              Value::object({
                  {"provider", response.provider},
                  {"model", response.model},
                  {"totalInputTokens", static_cast<long long>(usage.input_tokens)},
                  {"cachedInputTokens", static_cast<long long>(usage.cached_input_tokens)},
                  {"hitRate", hit_rate},
                  {"strategy", to_string(settings.cache_strategy)},
                  {"reasoningTokens", static_cast<long long>(usage.reasoning_tokens)},
                  {"reasoningSource", to_string(usage.reasoning_source)},
              }),
              trace);
}

ModelUsage aggregate_trace_usage(const std::vector<AgentTraceEntry>& trace) {
  std::vector<ModelUsage> per_call;
  per_call.reserve(trace.size());
  for (const auto& entry : trace) {
    if (entry.type != "model") {
      continue;
    }
    per_call.push_back(extract_model_usage(entry.response));
  }
  return merge_model_usage(per_call);
}

Value retrieved_memory_hook_value(const RetrievedMemory& memory) {
  return retrieved_memory_to_value(memory);
}

Value retrieved_memories_hook_value(const std::vector<RetrievedMemory>& hits) {
  Value::Array values;
  values.reserve(hits.size());
  for (const auto& hit : hits) {
    values.push_back(retrieved_memory_hook_value(hit));
  }
  return Value(std::move(values));
}

Value knowledge_hits_value(const std::vector<KnowledgeSearchHit>& hits) {
  Value::Array values;
  values.reserve(hits.size());
  for (const auto& hit : hits) {
    values.push_back(knowledge_search_hit_to_value(hit));
  }
  return Value(std::move(values));
}

Value execution_plan_summary_value(const ExecutionPlan& plan) {
  return Value::object({
      {"goal", plan.goal},
      {"stepCount", plan.steps.size()},
      {"plan", execution_plan_to_value(plan)},
  });
}

Value run_started_event_payload(Value input,
                                const std::string& session_id,
                                const Value& context,
                                const std::string& run_id = {}) {
  Value payload = Value::object({
      {"sessionId", session_id},
      {"input", std::move(input)},
  });
  if (!run_id.empty()) {
    payload["runId"] = run_id;
  }
  if (context.is_object() && !context.as_object().empty()) {
    payload["context"] = context;
  }
  return payload;
}

Value run_completed_event_payload(const AgentRunnerRunResult& result,
                                  const std::string& run_id = {}) {
  Value payload = Value::object({
      {"sessionId", result.session_id},
      {"iterationCount", result.iteration_count},
      {"text", result.text},
      {"terminationReason", to_string(result.termination_reason)},
      {"memoryHitCount", result.memory_hits.size()},
      {"knowledgeHitCount", result.knowledge_hits.size()},
  });
  if (!run_id.empty()) {
    payload["runId"] = run_id;
  }
  if (result.plan) {
    payload["plan"] = execution_plan_summary_value(*result.plan);
  }
  return payload;
}

Value run_failed_event_payload(Value input,
                               const std::string& session_id,
                               const std::string& error,
                               const std::string& run_id = {}) {
  Value payload = Value::object({
      {"sessionId", session_id},
      {"input", std::move(input)},
      {"error", error},
  });
  if (!run_id.empty()) {
    payload["runId"] = run_id;
  }
  return payload;
}

std::string stream_error_name(const std::exception& error) {
  if (dynamic_cast<const TimeoutError*>(&error)) {
    return "TimeoutError";
  }
  if (dynamic_cast<const RetryExhaustedError*>(&error)) {
    return "RetryExhaustedError";
  }
  if (dynamic_cast<const ConfigurationError*>(&error)) {
    return "ConfigurationError";
  }
  if (dynamic_cast<const AdapterError*>(&error)) {
    return "AdapterError";
  }
  if (dynamic_cast<const SchemaValidationError*>(&error)) {
    return "SchemaValidationError";
  }
  if (dynamic_cast<const ToolExecutionError*>(&error)) {
    return "ToolExecutionError";
  }
  if (dynamic_cast<const PermissionDeniedError*>(&error)) {
    return "PermissionDeniedError";
  }
  if (dynamic_cast<const AgentFrameworkError*>(&error)) {
    return "AgentFrameworkError";
  }
  return "Error";
}

Value stream_error_payload(const std::exception& error) {
  return Value::object({
      {"message", std::string(error.what())},
      {"name", stream_error_name(error)},
  });
}

std::string synthetic_tool_failure_message(const ToolCall& tool_call, const std::exception& error) {
  if (dynamic_cast<const ToolExecutionError*>(&error)) {
    return error.what();
  }
  return "Tool \"" + tool_call.name + "\" threw an unexpected error.";
}

ToolExecutionResult synthetic_tool_failure_result(const ToolCall& tool_call, std::string error_message) {
  const std::string output = Value::object({{"ok", false}, {"error", error_message}}).stringify(2);
  return ToolExecutionResult{
      .tool_call = tool_call,
      .ok = false,
      .error = std::move(error_message),
      .output = output,
      .message = create_tool_result_message(tool_call.id, tool_call.name, output, Value::object({{"ok", false}})),
  };
}

Value retry_scheduled_event_payload(const RetryScheduledContext& retry,
                                    Value extra = Value::object({})) {
  Value payload = extra.is_object() ? extra : Value::object({});
  payload["attempt"] = retry.attempt;
  payload["delayMs"] = retry.delay_ms;
  payload["error"] = retry.error;
  payload["target"] = to_string(retry.target);
  if (retry.metadata.is_object() && !retry.metadata.as_object().empty()) {
    payload["metadata"] = retry.metadata;
  }
  return payload;
}

AgentRunnerStatus runner_status(std::string kind,
                                 std::string stage,
                                 std::string state,
                                 std::string message,
                                 Value details = Value::object({})) {
  return AgentRunnerStatus{
      .kind = std::move(kind),
      .stage = std::move(stage),
      .state = std::move(state),
      .message = std::move(message),
      .details = std::move(details),
  };
}

std::optional<AgentRunnerStatus> status_from_loop_event(const AgentLoopStreamEvent& event) {
  switch (event.type) {
    case AgentLoopStreamEventType::ModelStart:
      return AgentRunnerStatus{
          .kind = "thinking",
          .stage = "model",
          .state = "start",
          .message = "Model " + event.provider + "/" + event.model + " is thinking.",
          .iteration = event.iteration,
          .provider = event.provider,
          .model = event.model,
      };
    case AgentLoopStreamEventType::ModelReasoningDelta:
      return AgentRunnerStatus{
          .kind = "thinking",
          .stage = "model",
          .state = "update",
          .message = event.delta,
          .iteration = event.iteration,
          .provider = event.provider,
          .model = event.model,
          .details = Value::object({{"reasoning", event.reasoning}}),
      };
    case AgentLoopStreamEventType::ModelResponse: {
      const auto tool_count = event.response.tool_calls.size();
      return AgentRunnerStatus{
          .kind = "thinking",
          .stage = "model",
          .state = "complete",
          .message = tool_count > 0 ? "Model finished reasoning and requested tools."
                                    : "Model finished reasoning and produced a response.",
          .iteration = event.iteration,
          .provider = event.response.provider,
          .model = event.response.model,
          .details = Value::object({{"toolCalls", tool_count},
                                    {"finishReason", event.response.finish_reason}}),
      };
    }
    case AgentLoopStreamEventType::ToolStart:
      return AgentRunnerStatus{
          .kind = "working",
          .stage = "tool",
          .state = "start",
          .message = "Running tool " + event.tool_call.name + ".",
          .iteration = event.iteration,
          .tool_name = event.tool_call.name,
          .tool_call_id = event.tool_call.id,
      };
    case AgentLoopStreamEventType::ToolComplete:
      return AgentRunnerStatus{
          .kind = "working",
          .stage = "tool",
          .state = "complete",
          .message = event.tool_result.ok ? "Tool " + event.tool_result.tool_call.name + " completed."
                                          : "Tool " + event.tool_result.tool_call.name + " failed.",
          .iteration = event.iteration,
          .tool_name = event.tool_result.tool_call.name,
          .tool_call_id = event.tool_result.tool_call.id,
          .details = Value::object({{"ok", event.tool_result.ok},
                                    {"output", event.tool_result.output}}),
      };
    case AgentLoopStreamEventType::Tools:
      return std::nullopt;
    case AgentLoopStreamEventType::IterationStart:
    case AgentLoopStreamEventType::ModelTextDelta:
    case AgentLoopStreamEventType::ToolCallArgumentDelta:
    case AgentLoopStreamEventType::Done:
      return std::nullopt;
  }
  return std::nullopt;
}

KnowledgeSearchOptions knowledge_search_options_from_runner(const RunnerKnowledgeRetrievalOptions& options) {
  KnowledgeSearchOptions search;
  search.top_k = options.top_k;
  search.vector_top_k = options.vector_top_k;
  search.lexical_top_k = options.lexical_top_k;
  search.min_score = options.min_score;
  search.hybrid_alpha = options.hybrid_alpha;
  search.rerank_top_k = options.rerank_top_k;
  search.retrieval_mode = options.retrieval_mode;
  search.oversample_factor = options.oversample_factor;
  search.fusion = options.fusion;
  search.modality_weights = options.modality_weights;
  search.uri_prefix = options.uri_prefix;
  search.document_ids = options.document_ids;
  search.asset_types = options.asset_types;
  search.space_id = options.space_id;
  search.source_types = options.source_types;
  search.chunk_ids = options.chunk_ids;
  search.metadata = options.metadata;
  return search;
}

ManagerSearchOptions manager_search_options_from_runner(const RunnerKnowledgeRetrievalOptions& options) {
  ManagerSearchOptions search;
  static_cast<KnowledgeSearchOptions&>(search) = knowledge_search_options_from_runner(options);
  search.tenant_id = options.tenant_id;
  search.knowledge_base_ids = options.knowledge_base_ids;
  search.enabled = options.enabled;
  return search;
}

Value runner_loop_context(Value context,
                          const std::optional<ExecutionPlan>& plan,
                          const std::vector<RetrievedMemory>& memory_hits,
                          const std::vector<KnowledgeSearchHit>& knowledge_hits,
                          const Value& knowledge_debug) {
  if (!context.is_object()) {
    context = Value::object({});
  }
  if (plan) {
    context["plan"] = execution_plan_to_value(*plan);
  }
  context["memoryHits"] = retrieved_memories_hook_value(memory_hits);
  context["knowledgeHits"] = knowledge_hits_value(knowledge_hits);
  if (knowledge_debug.is_object() && !knowledge_debug.as_object().empty()) {
    context["knowledgeDebug"] = knowledge_debug;
  }
  return context;
}

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

bool skill_requests_fork(const ResolvedSkillUse& entry) {
  return entry.skill.manifest.context == "fork" || !entry.skill.manifest.agent.empty();
}

AgentMessage skill_active_message(const ResolvedSkillUse& entry) {
  return create_message(MessageRole::System, entry.rendered_prompt,
                        Value::object({{"source", "skill"},
                                       {"skill", entry.skill.manifest.name},
                                       {"argumentsText", entry.arguments_text}}));
}

std::string skill_fork_child_input(const ResolvedSkillUse& entry, const std::string& input_text) {
  std::string child_input = entry.rendered_prompt;
  if (!trim_copy(input_text).empty()) {
    child_input += "\n\nUser task:\n" + input_text;
  }
  return child_input;
}

std::string skill_fork_session_id(const std::string& session_id, const ResolvedSkillUse& entry,
                                  const std::string& agent_name) {
  std::string fork_session = session_id.empty() ? std::string("default") : session_id;
  fork_session += ":skill:" + entry.skill.manifest.name;
  if (!agent_name.empty()) {
    fork_session += ":" + agent_name;
  }
  return fork_session;
}

std::optional<SkillForkExecution> execute_skill_fork(AgentRunner* owner, const AgentRunnerConfig& config,
                                                     const ResolvedSkillUse& entry,
                                                     const std::string& input_text,
                                                     const std::string& session_id,
                                                     const ModelSettings& model_settings,
                                                     const Value& context) {
  const std::string agent_name = entry.skill.manifest.agent;
  const auto child_session_id = skill_fork_session_id(session_id, entry, agent_name);
  SkillForkRequest request{
      .skill = entry,
      .input_text = input_text,
      .session_id = child_session_id,
      .model_settings = model_settings,
      .context = context,
  };

  if (!agent_name.empty()) {
    const auto found = config.skill_subagents.find(agent_name);
    if (found != config.skill_subagents.end() && found->second) {
      if (found->second == owner) {
        throw ConfigurationError("Skill \"" + entry.skill.manifest.name
                                 + "\" cannot fork into the owning AgentRunner.");
      }
      auto child_result = found->second->run(skill_fork_child_input(entry, input_text), child_session_id,
                                             model_settings, {}, {}, {}, context);
      return SkillForkExecution{
          .use = entry,
          .result = SkillForkResult{
              .text = child_result.text,
              .metadata = Value::object({{"agent", agent_name},
                                         {"sessionId", child_result.session_id},
                                         {"iterations", static_cast<double>(child_result.iteration_count)}}),
          },
          .agent = agent_name,
          .session_id = child_result.session_id,
      };
    }
  }

  if (config.skill_fork_handler) {
    auto result = config.skill_fork_handler(request);
    if (result) {
      return SkillForkExecution{
          .use = entry,
          .result = std::move(*result),
          .agent = agent_name,
          .session_id = child_session_id,
      };
    }
  }

  return std::nullopt;
}

AgentMessage skill_fork_result_message(const SkillForkExecution& execution) {
  std::string text = "Forked skill \"" + execution.use.skill.manifest.name + "\" result";
  if (!execution.agent.empty()) {
    text += " from agent \"" + execution.agent + "\"";
  }
  text += ":\n" + execution.result.text;

  return create_message(MessageRole::System, text,
                        Value::object({{"source", "skill-fork-result"},
                                       {"skill", execution.use.skill.manifest.name},
                                       {"agent", execution.agent},
                                       {"sessionId", execution.session_id},
                                       {"argumentsText", execution.use.arguments_text},
                                       {"result", execution.result.metadata}}));
}

SkillPrefaceBuildResult build_skill_preface_messages(AgentRunner* owner, const AgentRunnerConfig& config,
                                                     const ResolvedSkillsState& state,
                                                     const std::string& input_text,
                                                     const std::string& session_id,
                                                     const Value& context) {
  SkillPrefaceBuildResult result;
  result.messages.reserve(state.active_skills.size());
  for (const auto& entry : state.active_skills) {
    if (skill_requests_fork(entry)) {
      auto fork_execution = execute_skill_fork(owner, config, entry, input_text, session_id,
                                               state.model_settings, context);
      if (fork_execution) {
        result.messages.push_back(skill_fork_result_message(*fork_execution));
        result.fork_executions.push_back(std::move(*fork_execution));
        continue;
      }
    }
    result.messages.push_back(skill_active_message(entry));
  }
  return result;
}

Value skill_services_value(const ResolvedSkillsState& state,
                           const std::vector<SkillForkExecution>& fork_executions = {}) {
  Value::Array active;
  active.reserve(state.active_skills.size());
  std::vector<std::string> names;
  names.reserve(state.active_skills.size());
  for (const auto& entry : state.active_skills) {
    names.push_back(entry.skill.manifest.name);
    active.emplace_back(Value::object({
        {"name", entry.skill.manifest.name},
        {"description", entry.skill.manifest.description},
        {"argumentsText", entry.arguments_text},
    }));
  }
  Value::Array fork_results;
  fork_results.reserve(fork_executions.size());
  for (const auto& execution : fork_executions) {
    fork_results.emplace_back(Value::object({
        {"name", execution.use.skill.manifest.name},
        {"agent", execution.agent},
        {"sessionId", execution.session_id},
        {"text", execution.result.text},
        {"metadata", execution.result.metadata},
    }));
  }
  return Value::object({
      {"names", string_array_value(names)},
      {"autoSelected", string_array_value(state.auto_selected_skills)},
      {"allowedTools", string_array_value(state.allowed_tools)},
      {"active", Value(std::move(active))},
      {"forkResults", Value(std::move(fork_results))},
  });
}

ToolExecutionServices runner_runtime_services(SessionMemory* session,
                                               const std::string& session_id,
                                               LongTermMemory* long_term_memory,
                                               KnowledgeBase* knowledge_base,
                                               KnowledgeBaseManager* knowledge_base_manager,
                                               const Value& context,
                                               const std::optional<Value>& skills) {
  ToolExecutionServices services;
  services.session = session;
  services.long_term_memory = long_term_memory;
  services.knowledge_base = knowledge_base;
  services.knowledge_base_manager = knowledge_base_manager;
  services.values = Value::object({
      {"sessionId", session_id},
      {"session", Value::object({
                      {"sessionId", session_id},
                      {"available", session != nullptr},
                  })},
  });
  if (long_term_memory || knowledge_base || knowledge_base_manager) {
    services.values["knowledge"] = Value::object({
        {"longTermMemoryAvailable", long_term_memory != nullptr},
        {"knowledgeBaseAvailable", knowledge_base != nullptr},
        {"knowledgeBaseManagerAvailable", knowledge_base_manager != nullptr},
    });
  }
  if (context.is_object() && !context.as_object().empty()) {
    services.values["context"] = context;
  }
  if (skills) {
    services.values["skills"] = *skills;
  }
  return services;
}

RunnerRetrievalOptions merge_runner_retrieval_options(const RunnerRetrievalOptions& base,
                                                      const RunnerRetrievalOptions& override) {
  RunnerRetrievalOptions merged = base;
  if (override.enabled.has_value()) {
    merged.enabled = override.enabled;
  }
  if (override.top_k.has_value()) {
    merged.top_k = override.top_k;
  }
  if (override.min_score.has_value()) {
    merged.min_score = override.min_score;
  }
  if (!override.namespace_id.empty()) {
    merged.namespace_id = override.namespace_id;
  }
  return merged;
}

RunnerWritebackOptions merge_runner_writeback_options(const RunnerWritebackOptions& base,
                                                      const RunnerWritebackOptions& override) {
  RunnerWritebackOptions merged = base;
  if (override.enabled.has_value()) {
    merged.enabled = override.enabled;
  }
  if (!override.namespace_id.empty()) {
    merged.namespace_id = override.namespace_id;
  }
  if (!override.metadata.is_object() || !override.metadata.as_object().empty()) {
    merged.metadata = override.metadata;
  }
  return merged;
}

bool runner_retrieval_enabled(const RunnerRetrievalOptions& options) {
  return !options.enabled.has_value() || *options.enabled;
}

bool runner_writeback_enabled(const RunnerWritebackOptions& options,
                              const LongTermMemory& memory) {
  return options.enabled.has_value() ? *options.enabled : memory.auto_remember();
}

bool should_apply_runner_writeback(const RunnerWritebackOptions& options,
                                   const LongTermMemory* memory,
                                   const std::string& input,
                                   const std::string& output) {
  return memory && runner_writeback_enabled(options, *memory)
         && !trim_copy(input).empty()
         && !trim_copy(output).empty();
}

std::vector<std::string> execution_plan_step_titles(const std::optional<ExecutionPlan>& plan) {
  std::vector<std::string> titles;
  if (!plan) {
    return titles;
  }
  titles.reserve(plan->steps.size());
  for (const auto& step : plan->steps) {
    titles.push_back(step.title);
  }
  return titles;
}

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

RunnerKnowledgeQuery text_knowledge_query(std::string text) {
  if (text.empty()) {
    return {};
  }
  RunnerKnowledgeQuery query;
  query.kind = RunnerKnowledgeQuery::Kind::Text;
  query.text = std::move(text);
  return query;
}

RunnerKnowledgeQuery image_knowledge_query(const AgentMessage& message,
                                           const MessageContentPart& part) {
  RunnerKnowledgeQuery query;
  query.kind = RunnerKnowledgeQuery::Kind::Image;
  query.image.source = part.source;
  query.image.alt_text = part.alt_text;
  query.image.title = message.metadata.at("title").as_string();
  query.image.text_hint = message.metadata.at("textHint").as_string();
  query.image.metadata = message.metadata.is_object() ? message.metadata : Value::object({});
  return query;
}

RunnerKnowledgeQuery resolve_runner_knowledge_query(const AgentMessage& message) {
  const std::string text = extract_text_content(message.content);
  if (!trim_copy(text).empty()) {
    return text_knowledge_query(text);
  }
  for (const auto& part : message.content) {
    if (part.type == ContentPartType::Image) {
      return image_knowledge_query(message, part);
    }
  }
  return {};
}

std::string runner_knowledge_query_text(const RunnerKnowledgeQuery& query) {
  if (query.kind == RunnerKnowledgeQuery::Kind::Text) {
    return query.text;
  }
  if (query.kind == RunnerKnowledgeQuery::Kind::Image) {
    std::string text;
    for (const auto& part : std::vector<std::string>{query.image.title, query.image.alt_text,
                                                     query.image.text_hint}) {
      if (part.empty()) {
        continue;
      }
      if (!text.empty()) {
        text += " ";
      }
      text += part;
    }
    return text.empty() ? "[image]" : text;
  }
  return {};
}

bool runner_knowledge_query_empty(const RunnerKnowledgeQuery& query) {
  return query.kind == RunnerKnowledgeQuery::Kind::None || runner_knowledge_query_text(query).empty();
}

RunnerMemoryRetrievalResult retrieve_long_term_memory_context(LongTermMemory& memory,
                                                              const std::string& query,
                                                              const RunnerRetrievalOptions& options,
                                                              const HookSet& hooks,
                                                              EventBus* event_bus,
                                                              const ExecutionPolicies& execution_policies,
                                                              CancellationToken* cancellation) {
  constexpr const char* source = "long-term-memory";
  if (hooks.before_knowledge_retrieval) {
    RetrievalHookContext hook_context;
    hook_context.target = ExecutionTarget::Retrieval;
    hook_context.query = query;
    hook_context.source = source;
    hooks.before_knowledge_retrieval(hook_context);
  }
  if (event_bus) {
    event_bus->publish("retrieval.started", ExecutionTarget::Retrieval,
                       Value::object({{"query", query}, {"source", source}}));
  }

  try {
    RunnerMemoryRetrievalResult result;
    result.hits = execute_with_policies(
        ExecutionTarget::Retrieval, execution_policies,
        Value::object({{"query", query}, {"source", source}}), cancellation,
        [&]() {
          return memory.search(query, options.top_k, options.min_score, options.namespace_id, cancellation);
        },
        [&](const RetryScheduledContext& retry) {
          if (event_bus) {
            event_bus->publish("retry.scheduled", ExecutionTarget::Retrieval,
                               retry_scheduled_event_payload(
                                   retry, Value::object({{"query", query}, {"source", source}})));
          }
        });
    result.message = memory.create_context_message(result.hits);
    Value hook_result = Value::object({
        {"hits", retrieved_memories_hook_value(result.hits)},
        {"hitCount", result.hits.size()},
        {"hasMessage", result.message.has_value()},
    });
    if (hooks.after_knowledge_retrieval) {
      RetrievalHookContext hook_context;
      hook_context.target = ExecutionTarget::Retrieval;
      hook_context.query = query;
      hook_context.source = source;
      hook_context.result = hook_result;
      hooks.after_knowledge_retrieval(hook_context);
    }
    if (event_bus) {
      event_bus->publish("retrieval.completed", ExecutionTarget::Retrieval,
                         Value::object({{"query", query}, {"source", source},
                                       {"hits", result.hits.size()}}));
    }
    return result;
  } catch (const std::exception& error) {
    if (hooks.on_knowledge_retrieval_error) {
      RetrievalHookContext hook_context;
      hook_context.target = ExecutionTarget::Retrieval;
      hook_context.query = query;
      hook_context.source = source;
      hook_context.error = error.what();
      hooks.on_knowledge_retrieval_error(hook_context);
    }
    if (event_bus) {
      event_bus->publish("retrieval.failed", ExecutionTarget::Retrieval,
                         Value::object({{"query", query}, {"source", source},
                                       {"error", error.what()}}));
    }
    throw;
  }
}

RunnerKnowledgeRetrievalResult retrieve_knowledge_context(KnowledgeBase* knowledge_base,
                                                          KnowledgeBaseManager* knowledge_base_manager,
                                                          const RunnerKnowledgeQuery& query,
                                                          const RunnerKnowledgeRetrievalOptions& options,
                                                          const HookSet& hooks,
                                                          EventBus* event_bus,
                                                          const ExecutionPolicies& execution_policies,
                                                          CancellationToken* cancellation) {
  RunnerKnowledgeRetrievalResult empty;
  if (runner_knowledge_query_empty(query) || !options.enabled || (!knowledge_base && !knowledge_base_manager)) {
    return empty;
  }

  const std::string query_text = runner_knowledge_query_text(query);
  const char* source = knowledge_base_manager ? "knowledge-base-manager" : "knowledge-base";
  if (hooks.before_knowledge_retrieval) {
    RetrievalHookContext hook_context;
    hook_context.target = ExecutionTarget::Retrieval;
    hook_context.query = query_text;
    hook_context.source = source;
    hooks.before_knowledge_retrieval(hook_context);
  }
  if (event_bus) {
    event_bus->publish("retrieval.started", ExecutionTarget::Retrieval,
                       Value::object({{"query", query_text}, {"source", source}}));
  }

  try {
    RunnerKnowledgeRetrievalResult result = execute_with_policies(
        ExecutionTarget::Retrieval, execution_policies,
        Value::object({{"query", query_text}, {"source", source}}), cancellation,
        [&]() {
          RunnerKnowledgeRetrievalResult current;
          if (knowledge_base_manager) {
            auto search_options = manager_search_options_from_runner(options);
            search_options.cancellation = cancellation;
            auto context = query.kind == RunnerKnowledgeQuery::Kind::Image
                               ? knowledge_base_manager->build_context_message(query.image, std::move(search_options))
                               : knowledge_base_manager->build_context_message(query.text, std::move(search_options));
            current.hits = std::move(context.hits);
            current.message = std::move(context.message);
            current.debug = std::move(context.debug);
            return current;
          }
          auto search_options = knowledge_search_options_from_runner(options);
          search_options.cancellation = cancellation;
          auto context = query.kind == RunnerKnowledgeQuery::Kind::Image
                             ? knowledge_base->build_context_message(query.image, std::move(search_options))
                             : knowledge_base->build_context_message(query.text, std::move(search_options));
          current.hits = std::move(context.hits);
          current.message = std::move(context.message);
          current.debug = std::move(context.debug);
          return current;
        },
        [&](const RetryScheduledContext& retry) {
          if (event_bus) {
            event_bus->publish("retry.scheduled", ExecutionTarget::Retrieval,
                               retry_scheduled_event_payload(
                                   retry, Value::object({{"query", query_text}, {"source", source}})));
          }
        });

    Value hook_result = Value::object({
        {"hits", knowledge_hits_value(result.hits)},
        {"hitCount", result.hits.size()},
        {"hasMessage", result.message.has_value()},
        {"debug", result.debug},
    });
    if (hooks.after_knowledge_retrieval) {
      RetrievalHookContext hook_context;
      hook_context.target = ExecutionTarget::Retrieval;
      hook_context.query = query_text;
      hook_context.source = source;
      hook_context.result = hook_result;
      hooks.after_knowledge_retrieval(hook_context);
    }
    if (event_bus) {
      event_bus->publish("retrieval.completed", ExecutionTarget::Retrieval,
                         Value::object({{"query", query_text}, {"source", source},
                                       {"hits", result.hits.size()}}));
    }
    return result;
  } catch (const std::exception& error) {
    if (hooks.on_knowledge_retrieval_error) {
      RetrievalHookContext hook_context;
      hook_context.target = ExecutionTarget::Retrieval;
      hook_context.query = query_text;
      hook_context.source = source;
      hook_context.error = error.what();
      hooks.on_knowledge_retrieval_error(hook_context);
    }
    if (event_bus) {
      event_bus->publish("retrieval.failed", ExecutionTarget::Retrieval,
                         Value::object({{"query", query_text}, {"source", source},
                                       {"error", error.what()}}));
    }
    throw;
  }
}

std::optional<ExecutionPlan> create_runner_execution_plan(const AgentRunnerConfig& config,
                                                          const std::string& input,
                                                          SessionMemory* session,
                                                          const Value& context,
                                                          ToolRegistry* tools,
                                                          const std::vector<RetrievedMemory>& memory_hits,
                                                          const std::vector<KnowledgeSearchHit>& knowledge_hits,
                                                          EventBus* event_bus,
                                                          CancellationToken* cancellation,
                                                          bool enable_planning = true) {
  if (!enable_planning || !config.enable_planning || !config.planner) {
    return std::nullopt;
  }
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }

  if (event_bus) {
    event_bus->publish("planning.started", ExecutionTarget::Run,
                       Value::object({{"input", input},
                                     {"memoryHitCount", memory_hits.size()},
                                     {"knowledgeHitCount", knowledge_hits.size()}}));
  }

  try {
    auto plan = config.planner->plan(PlannerParams{
        .input = input,
        .session = session,
        .context = context,
        .tools = tools,
        .memory_hits = memory_hits,
        .knowledge_hits = knowledge_hits,
        .cancellation = cancellation,
    });
    if (cancellation) {
      cancellation->throw_if_cancelled(ExecutionTarget::Run);
    }
    if (plan && plan->steps.empty()) {
      plan = std::nullopt;
    }
    if (event_bus) {
      Value payload = Value::object({{"hasPlan", static_cast<bool>(plan)}});
      if (plan) {
        payload["goal"] = plan->goal;
        payload["stepCount"] = plan->steps.size();
      }
      event_bus->publish("planning.completed", ExecutionTarget::Run, std::move(payload));
    }
    return plan;
  } catch (const std::exception& error) {
    if (event_bus) {
      event_bus->publish("planning.failed", ExecutionTarget::Run,
                         Value::object({{"input", input}, {"error", error.what()}}));
    }
    throw;
  }
}

AgentRunnerRunResult runner_result_from_loop(AgentLoopRunResult base,
                                             std::vector<RetrievedMemory> memory_hits,
                                             std::vector<KnowledgeSearchHit> knowledge_hits = {},
                                             Value knowledge_debug = Value::object({}),
                                             std::optional<ExecutionPlan> plan = std::nullopt) {
  AgentRunnerRunResult result;
  result.session_id = base.session_id;
  result.iteration_count = base.iteration_count;
  result.text = base.text;
  result.response = base.response;
  result.trace = base.trace;
  result.messages = base.messages;
  result.termination_reason = base.termination_reason;
  result.usage = base.usage;
  result.memory_hits = std::move(memory_hits);
  result.knowledge_hits = std::move(knowledge_hits);
  result.knowledge_debug = std::move(knowledge_debug);
  result.plan = std::move(plan);
  return result;
}

Value durable_tool_call_to_value(const ToolCall& tool_call) {
  return Value::object({
      {"id", tool_call.id},
      {"name", tool_call.name},
      {"arguments", tool_call.arguments},
  });
}

Value durable_model_response_to_value(const ModelResponse& response) {
  Value::Array tool_calls;
  tool_calls.reserve(response.tool_calls.size());
  for (const auto& tool_call : response.tool_calls) {
    tool_calls.push_back(durable_tool_call_to_value(tool_call));
  }
  Value::Array content;
  for (const auto& part : response.content) {
    auto message = agent_message_to_value(
        create_message(MessageRole::Assistant, std::vector<MessageContentPart>{part}));
    content.push_back(message.at("content").as_array().front());
  }
  return Value::object({
      {"id", response.id.empty() ? Value() : Value(response.id)},
      {"provider", response.provider},
      {"model", response.model},
      {"content", Value(std::move(content))},
      {"text", response.text},
      {"reasoning", response.reasoning ? Value::object({{"text", response.reasoning->text},
                                                        {"format", response.reasoning->format}})
                                      : Value()},
      {"toolCalls", Value(std::move(tool_calls))},
      {"finishReason", response.finish_reason},
      {"raw", response.raw},
  });
}

Value durable_tool_execution_result_to_value(const ToolExecutionResult& result) {
  Value::Object object{
      {"toolCall", durable_tool_call_to_value(result.tool_call)},
      {"ok", result.ok},
      {"error", result.error.empty() ? Value() : Value(result.error)},
      {"output", result.output},
      {"message", agent_message_to_value(result.message)},
  };
  if (result.result) {
    if (std::holds_alternative<Value>(*result.result)) {
      object["result"] = std::get<Value>(*result.result);
    } else {
      const auto& envelope = std::get<ToolResultEnvelope>(*result.result);
      Value::Object envelope_value{{"metadata", envelope.metadata}};
      if (envelope.value) {
        envelope_value["value"] = *envelope.value;
      }
      if (envelope.content) {
        auto message = agent_message_to_value(create_message(MessageRole::Assistant, *envelope.content));
        envelope_value["content"] = message.at("content");
      }
      object["result"] = Value(std::move(envelope_value));
    }
  }
  return Value(std::move(object));
}

Value durable_trace_entry_to_value(const AgentTraceEntry& entry) {
  Value::Object object{{"type", entry.type}, {"iteration", entry.iteration}};
  if (entry.type == "model") {
    object["response"] = durable_model_response_to_value(entry.response);
  }
  if (!entry.tool_results.empty()) {
    Value::Array results;
    results.reserve(entry.tool_results.size());
    for (const auto& result : entry.tool_results) {
      results.push_back(durable_tool_execution_result_to_value(result));
    }
    object["toolResults"] = Value(std::move(results));
  }
  return Value(std::move(object));
}

Value reasoning_settings_to_value(const ReasoningSettings& reasoning) {
  Value::Object value;
  if (reasoning.enabled) {
    value["enabled"] = *reasoning.enabled;
  }
  if (std::holds_alternative<std::string>(reasoning.budget)) {
    value["budget"] = std::get<std::string>(reasoning.budget);
  } else if (std::holds_alternative<double>(reasoning.budget)) {
    value["budget"] = std::get<double>(reasoning.budget);
  }
  if (reasoning.include_thoughts) {
    value["includeThoughts"] = *reasoning.include_thoughts;
  }
  if (!reasoning.tag_name.empty()) {
    value["tagName"] = reasoning.tag_name;
  }
  return Value(std::move(value));
}

Value model_settings_to_value(const ModelSettings& settings) {
  Value value = Value::object({
      {"model", settings.model},
      {"extra", settings.extra},
  });
  if (settings.temperature) {
    value["temperature"] = *settings.temperature;
  }
  if (settings.max_output_tokens) {
    value["maxOutputTokens"] = *settings.max_output_tokens;
  }
  if (settings.reasoning) {
    value["reasoning"] = reasoning_settings_to_value(*settings.reasoning);
  }
  return value;
}

Value messages_to_value(const std::vector<AgentMessage>& messages) {
  Value::Array values;
  values.reserve(messages.size());
  for (const auto& message : messages) {
    values.push_back(agent_message_to_value(message));
  }
  return Value(std::move(values));
}

Value runner_durable_memory_hits_to_value(const std::vector<RetrievedMemory>& hits) {
  Value::Array values;
  values.reserve(hits.size());
  for (const auto& hit : hits) {
    values.push_back(retrieved_memory_hook_value(hit));
  }
  return Value(std::move(values));
}

void maybe_auto_compact_session(SessionMemory& session, EventBus* event_bus,
                                const TraceContext& trace_context) {
  if (!session.should_auto_compact()) {
    return;
  }
  const std::size_t before_tokens = session.estimated_token_count();
  const std::size_t budget = session.token_budget();
  session.compact();
  if (event_bus) {
    const std::size_t after_tokens = session.estimated_token_count();
    event_bus->publish("session.auto_compact", ExecutionTarget::Run,
                       Value::object({
                           {"sessionId", session.session_id()},
                           {"tokensBefore", static_cast<long long>(before_tokens)},
                           {"tokensAfter", static_cast<long long>(after_tokens)},
                           {"tokenBudget", static_cast<long long>(budget)},
                       }),
                       trace_context);
  }
}

}  // namespace

std::string to_string(AgentLoopTerminationReason reason) {
  switch (reason) {
    case AgentLoopTerminationReason::Completed:
      return "completed";
    case AgentLoopTerminationReason::MaxIterations:
      return "max-iterations";
    case AgentLoopTerminationReason::IncompleteResponse:
      return "incomplete-response";
  }
  return "completed";
}

std::string to_string(AgentLoopDurablePhase phase) {
  switch (phase) {
    case AgentLoopDurablePhase::BeforeModel:
      return "before-model";
    case AgentLoopDurablePhase::ModelCompleted:
      return "model-completed";
    case AgentLoopDurablePhase::ToolsCompleted:
      return "tools-completed";
    case AgentLoopDurablePhase::Completed:
      return "completed";
  }
  return "before-model";
}

Value agent_loop_durable_state_to_value(const AgentLoopDurableState& state) {
  Value::Array trace;
  trace.reserve(state.trace.size());
  for (const auto& entry : state.trace) {
    trace.push_back(durable_trace_entry_to_value(entry));
  }
  Value value = Value::object({
      {"version", state.version},
      {"phase", to_string(state.phase)},
      {"sessionId", state.session_id},
      {"nextIteration", state.next_iteration},
      {"inputText", state.input_text},
      {"inputParts", message_content_parts_value(state.input_parts)},
      {"inputMessage", agent_message_to_value(state.input_message)},
      {"inputValue", state.input_value},
      {"session", session_memory_snapshot_to_value(state.session)},
      {"trace", Value(std::move(trace))},
      {"lastResponse", state.last_response ? durable_model_response_to_value(*state.last_response) : Value()},
      {"lastAssistantText", state.last_assistant_text},
  });
  if (state.termination_reason) {
    value["terminationReason"] = to_string(*state.termination_reason);
  }
  return value;
}

std::string to_string(AgentRunnerDurableStatus status) {
  switch (status) {
    case AgentRunnerDurableStatus::Running:
      return "running";
    case AgentRunnerDurableStatus::Completed:
      return "completed";
    case AgentRunnerDurableStatus::Interrupted:
      return "interrupted";
  }
  return "running";
}

Value agent_runner_durable_state_to_value(const AgentRunnerDurableState& state) {
  return Value::object({
      {"version", state.version},
      {"status", to_string(state.status)},
      {"runId", state.run_id},
      {"sessionId", state.session_id},
      {"input", state.input_value.is_null() ? Value(state.input) : state.input_value},
      {"inputMessage", agent_message_to_value(state.input_message)},
      {"modelSettings", model_settings_to_value(state.model_settings)},
      {"effectiveInput", state.effective_input_value.is_null() ? Value(state.effective_input)
                                                               : state.effective_input_value},
      {"effectiveInputMessage", agent_message_to_value(state.effective_input_message)},
      {"inputText", state.input_text},
      {"inputParts", message_content_parts_value(state.input_parts)},
      {"plan", state.plan ? execution_plan_to_value(*state.plan) : Value()},
      {"memoryHits", runner_durable_memory_hits_to_value(state.memory_hits)},
      {"knowledgeHits", knowledge_hits_value(state.knowledge_hits)},
      {"knowledgeDebug", state.knowledge_debug},
      {"prefaceMessages", messages_to_value(state.preface_messages)},
      {"loop", state.loop ? agent_loop_durable_state_to_value(*state.loop) : Value()},
      {"updatedAt", state.updated_at},
  });
}

std::string to_string(AgentLoopStreamEventType type) {
  switch (type) {
    case AgentLoopStreamEventType::IterationStart:
      return "iteration-start";
    case AgentLoopStreamEventType::ModelStart:
      return "model-start";
    case AgentLoopStreamEventType::ModelTextDelta:
      return "model-text-delta";
    case AgentLoopStreamEventType::ModelReasoningDelta:
      return "model-reasoning-delta";
    case AgentLoopStreamEventType::ModelResponse:
      return "model-response";
    case AgentLoopStreamEventType::ToolCallArgumentDelta:
      return "tool-call-argument-delta";
    case AgentLoopStreamEventType::ToolStart:
      return "tool-start";
    case AgentLoopStreamEventType::ToolComplete:
      return "tool-complete";
    case AgentLoopStreamEventType::Tools:
      return "tools";
    case AgentLoopStreamEventType::Done:
      return "done";
  }
  return "iteration-start";
}

std::string to_string(AgentRunnerStreamEventType type) {
  switch (type) {
    case AgentRunnerStreamEventType::Status:
      return "status";
    case AgentRunnerStreamEventType::KnowledgeRetrieval:
      return "knowledge-retrieval";
    case AgentRunnerStreamEventType::MemoryRetrieval:
      return "memory-retrieval";
    case AgentRunnerStreamEventType::Planning:
      return "plan";
    case AgentRunnerStreamEventType::Loop:
      return "loop";
    case AgentRunnerStreamEventType::ToolCallArgumentDelta:
      return "tool-call-argument-delta";
    case AgentRunnerStreamEventType::Done:
      return "done";
    case AgentRunnerStreamEventType::Error:
      return "error";
  }
  return "loop";
}

AgentLoop::AgentLoop(AgentLoopConfig config) : config_(std::move(config)) {
  if (!config_.model) {
    throw ConfigurationError("AgentLoop requires a model adapter.");
  }
  if (!config_.tool_registry) {
    throw ConfigurationError("AgentLoop requires a tool registry.");
  }
  if (!config_.tool_executor) {
    throw ConfigurationError("AgentLoop requires a tool executor.");
  }
  if (!config_.context_manager) {
    throw ConfigurationError("AgentLoop requires a context manager.");
  }
}

std::vector<AgentMessage> AgentLoop::build_prompt_messages(SessionMemory& session, const std::string& input,
                                                           const Value& input_value,
                                                           int iteration,
                                                           const std::vector<AgentTraceEntry>&,
                                                           const std::vector<AgentMessage>& preface_messages,
                                                           const Value& runtime_context) const {
  std::vector<AgentMessage> messages;
  if (!config_.system_prompt.empty()) {
    messages.push_back(create_message(MessageRole::System, config_.system_prompt,
                                      Value::object({{"source", "runner"}})));
  }
  messages.insert(messages.end(), preface_messages.begin(), preface_messages.end());
  Value runtime = runtime_context.is_object() ? runtime_context : Value::object({});
  runtime["input"] = input_value.is_null() ? Value(input) : input_value;
  runtime["iteration"] = iteration;
  const auto context_message = config_.context_manager->build_message(
      runtime);
  if (context_message) {
    messages.push_back(*context_message);
  }
  auto session_messages = session.get_messages();
  messages.insert(messages.end(), session_messages.begin(), session_messages.end());
  return messages;
}

ModelResponse AgentLoop::call_model(int iteration, const std::vector<AgentMessage>& messages,
                                    const ModelSettings& model_settings,
                                    CancellationToken* cancellation) {
  const auto model_trace = child_or_root_trace_context(config_.trace_context);
  const Value request = Value::object({
      {"iteration", iteration},
      {"messageCount", messages.size()},
      {"toolCount", config_.tool_registry->descriptors().size()},
      {"model", model_settings.model},
  });
  if (config_.hooks.before_model) {
    ModelHookContext hook_context;
    hook_context.target = ExecutionTarget::Model;
    hook_context.trace_id = model_trace.trace_id;
    hook_context.run_id = model_trace.run_id;
    hook_context.workflow_run_id = model_trace.workflow_run_id;
    hook_context.request = request;
    config_.hooks.before_model(hook_context);
  }
  if (config_.event_bus) {
    config_.event_bus->publish("model.started", ExecutionTarget::Model,
                               Value::object({{"iteration", iteration}, {"messageCount", messages.size()}}),
                               model_trace);
  }
  try {
    ModelResponse response = execute_with_policies(
        ExecutionTarget::Model, config_.execution_policies, Value::object({{"iteration", iteration}}), cancellation,
        [&]() {
          return config_.model->generate(GenerateParams{
              .messages = messages,
              .tools = config_.tool_registry->descriptors(),
              .settings = config_.model->resolve_settings(model_settings),
              .cancellation = cancellation,
          });
        },
        [&](const RetryScheduledContext& retry) {
          if (config_.event_bus) {
            config_.event_bus->publish(
                "retry.scheduled", ExecutionTarget::Model,
                retry_scheduled_event_payload(retry, Value::object({{"iteration", iteration}})),
                model_trace);
          }
        });
    if (config_.event_bus) {
      config_.event_bus->publish("model.completed", ExecutionTarget::Model,
                                 Value::object({{"iteration", iteration}, {"finishReason", response.finish_reason}}),
                                 model_trace);
      emit_cache_stats_event(*config_.event_bus, response, model_settings, model_trace);
    }
    if (config_.hooks.after_model) {
      ModelHookContext hook_context;
      hook_context.target = ExecutionTarget::Model;
      hook_context.trace_id = model_trace.trace_id;
      hook_context.run_id = model_trace.run_id;
      hook_context.workflow_run_id = model_trace.workflow_run_id;
      hook_context.request = request;
      hook_context.response = model_response_hook_value(response);
      config_.hooks.after_model(hook_context);
    }
    return response;
  } catch (const std::exception& error) {
    if (config_.hooks.on_model_error) {
      ModelHookContext hook_context;
      hook_context.target = ExecutionTarget::Model;
      hook_context.trace_id = model_trace.trace_id;
      hook_context.run_id = model_trace.run_id;
      hook_context.workflow_run_id = model_trace.workflow_run_id;
      hook_context.request = request;
      hook_context.error = error.what();
      config_.hooks.on_model_error(hook_context);
    }
    if (config_.event_bus) {
      config_.event_bus->publish("model.failed", ExecutionTarget::Model,
                                 Value::object({{"iteration", iteration}, {"error", error.what()}}),
                                 model_trace);
    }
    throw;
  }
}

AgentLoopRunResult AgentLoop::run(SessionMemory& session, const std::string& input,
                                  const ModelSettings& model_settings,
                                  const std::vector<AgentMessage>& preface_messages,
                                  ToolExecutionContext tool_context,
                                  Value runtime_context,
                                  AgentLoopDurableOptions durable_options,
                                  CancellationToken* cancellation) {
  return run_input(session, create_message(MessageRole::User, input), Value(input), model_settings,
                   preface_messages, std::move(tool_context), std::move(runtime_context),
                   std::move(durable_options), cancellation);
}

AgentLoopRunResult AgentLoop::run(SessionMemory& session, std::vector<MessageContentPart> input_parts,
                                  const ModelSettings& model_settings,
                                  const std::vector<AgentMessage>& preface_messages,
                                  ToolExecutionContext tool_context,
                                  Value runtime_context,
                                  AgentLoopDurableOptions durable_options,
                                  CancellationToken* cancellation) {
  const std::string input_text = extract_text_content(input_parts);
  return run_input(session, user_input_message(std::move(input_parts)), Value(input_text), model_settings,
                   preface_messages, std::move(tool_context), std::move(runtime_context),
                   std::move(durable_options), cancellation);
}

AgentLoopRunResult AgentLoop::run(SessionMemory& session, AgentMessage input_message,
                                  const ModelSettings& model_settings,
                                  const std::vector<AgentMessage>& preface_messages,
                                  ToolExecutionContext tool_context,
                                  Value runtime_context,
                                  AgentLoopDurableOptions durable_options,
                                  CancellationToken* cancellation) {
  const Value input_value = agent_message_to_value(input_message);
  return run_input(session, std::move(input_message), input_value, model_settings, preface_messages,
                   std::move(tool_context), std::move(runtime_context), std::move(durable_options),
                   cancellation);
}

AgentLoopRunResult AgentLoop::run_input(SessionMemory& session, AgentMessage input_message, Value input_value,
                                        const ModelSettings& model_settings,
                                        const std::vector<AgentMessage>& preface_messages,
                                        ToolExecutionContext tool_context,
                                        Value runtime_context,
                                        AgentLoopDurableOptions durable_options,
                                        CancellationToken* cancellation) {
  if (durable_options.resume_state) {
    input_message = input_message_from_durable_state(*durable_options.resume_state);
    input_value = durable_options.resume_state->input_value.is_null()
                      ? agent_message_to_value(input_message)
                      : durable_options.resume_state->input_value;
  }
  const std::string input_text = extract_text_content(input_message.content);
  const auto input_parts = input_message.content;
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  std::vector<AgentTraceEntry> trace = durable_options.resume_state ? durable_options.resume_state->trace
                                                                    : std::vector<AgentTraceEntry>{};
  ModelResponse last_response;
  bool has_last_response = false;
  std::string last_assistant_text = durable_options.resume_state
                                        ? durable_options.resume_state->last_assistant_text
                                        : std::string{};
  int start_iteration = 0;
  std::optional<ModelResponse> resume_pending_model_response;
  if (durable_options.resume_state) {
    const auto& resume = *durable_options.resume_state;
    session.restore(resume.session);
    start_iteration = std::max(0, resume.next_iteration);
    if (resume.last_response) {
      last_response = *resume.last_response;
      has_last_response = true;
      if (resume.phase == AgentLoopDurablePhase::ModelCompleted) {
        resume_pending_model_response = *resume.last_response;
      }
    }
  } else {
    session.add(input_message);
    if (cancellation) {
      cancellation->throw_if_cancelled(ExecutionTarget::Run);
    }
  }

  auto checkpoint = [&](AgentLoopDurablePhase phase,
                        int next_iteration,
                        std::optional<AgentLoopTerminationReason> termination_reason = std::nullopt) {
    if (!durable_options.on_checkpoint) {
      return;
    }
    AgentLoopDurableState state;
    state.version = 1;
    state.phase = phase;
    state.session_id = session.session_id();
    state.next_iteration = next_iteration;
    state.input_text = input_text;
    state.input_parts = input_parts;
    state.input_message = input_message;
    state.input_value = input_value;
    state.session = session.snapshot();
    state.trace = trace;
    if (has_last_response) {
      state.last_response = last_response;
    }
    state.last_assistant_text = last_assistant_text;
    state.termination_reason = termination_reason;
    durable_options.on_checkpoint(state);
  };

  if (!durable_options.resume_state) {
    checkpoint(AgentLoopDurablePhase::BeforeModel, 0);
  }

  for (int iteration = start_iteration; iteration < config_.max_iterations; ++iteration) {
    if (cancellation) {
      cancellation->throw_if_cancelled(ExecutionTarget::Run);
    }
    maybe_auto_compact_session(session, config_.event_bus, config_.trace_context);
    const auto prompt_messages = build_prompt_messages(session, input_text, input_value, iteration, trace,
                                                       preface_messages, runtime_context);
    checkpoint(AgentLoopDurablePhase::BeforeModel, iteration);
    const bool resumed_model_response = resume_pending_model_response.has_value();
    ModelResponse response = resumed_model_response ? *resume_pending_model_response
                                                    : call_model(iteration, prompt_messages, model_settings,
                                                                 cancellation);
    resume_pending_model_response.reset();
    last_response = response;
    has_last_response = true;
    if (!response.text.empty()) {
      last_assistant_text = response.text;
    }
    if (!resumed_model_response) {
      session.add(assistant_message_from_response(response));
      trace.push_back(AgentTraceEntry{.type = "model", .iteration = iteration, .response = response});
    }
    checkpoint(AgentLoopDurablePhase::ModelCompleted, iteration);

    if (response.tool_calls.empty()) {
      const auto termination = is_incomplete_finish_reason(response.finish_reason)
                                   ? AgentLoopTerminationReason::IncompleteResponse
                                   : AgentLoopTerminationReason::Completed;
      AgentLoopRunResult result{session.session_id(), iteration + 1, response.text, response, trace,
                                session.get_messages(), termination, {}};
      result.usage = aggregate_trace_usage(result.trace);
      checkpoint(AgentLoopDurablePhase::Completed, iteration + 1, termination);
      return result;
    }

    tool_context = normalize_tool_execution_context(std::move(tool_context));
    if (!tool_context.cancellation) {
      tool_context.cancellation = cancellation;
    }
    tool_context.iteration = iteration;
    tool_context.attributes["iteration"] = iteration;
    const auto tool_results = config_.tool_executor->execute_all(response.tool_calls, tool_context);
    std::vector<AgentMessage> tool_messages;
    tool_messages.reserve(tool_results.size());
    for (const auto& result : tool_results) {
      tool_messages.push_back(result.message);
    }
    session.add_many(tool_messages);
    trace.push_back(AgentTraceEntry{.type = "tools", .iteration = iteration, .tool_results = tool_results});
    checkpoint(AgentLoopDurablePhase::ToolsCompleted, iteration + 1);
  }

  if (!has_last_response) {
    throw AgentFrameworkError("Agent loop reached the max iteration limit ("
                              + std::to_string(config_.max_iterations)
                              + ") without producing a model response.");
  }
  AgentLoopRunResult result{session.session_id(),
                            config_.max_iterations,
                            last_assistant_text,
                            last_response,
                            trace,
                            session.get_messages(),
                            AgentLoopTerminationReason::MaxIterations, {}};
  result.usage = aggregate_trace_usage(result.trace);
  checkpoint(AgentLoopDurablePhase::Completed, config_.max_iterations, AgentLoopTerminationReason::MaxIterations);
  return result;
}

AgentLoopStreamResult AgentLoop::stream(SessionMemory& session, const std::string& input,
                                        const ModelSettings& model_settings,
                                        const std::vector<AgentMessage>& preface_messages,
                                        ToolExecutionContext tool_context,
                                        Value runtime_context,
                                        CancellationToken* cancellation) {
  return stream_input(session, create_message(MessageRole::User, input), Value(input), model_settings,
                      preface_messages, std::move(tool_context), std::move(runtime_context), cancellation);
}

AgentLoopStreamResult AgentLoop::stream(SessionMemory& session, std::vector<MessageContentPart> input_parts,
                                        const ModelSettings& model_settings,
                                        const std::vector<AgentMessage>& preface_messages,
                                        ToolExecutionContext tool_context,
                                        Value runtime_context,
                                        CancellationToken* cancellation) {
  const std::string input_text = extract_text_content(input_parts);
  return stream_input(session, user_input_message(std::move(input_parts)), Value(input_text), model_settings,
                      preface_messages, std::move(tool_context), std::move(runtime_context), cancellation);
}

AgentLoopStreamResult AgentLoop::stream(SessionMemory& session, AgentMessage input_message,
                                        const ModelSettings& model_settings,
                                        const std::vector<AgentMessage>& preface_messages,
                                        ToolExecutionContext tool_context,
                                        Value runtime_context,
                                        CancellationToken* cancellation) {
  const Value input_value = agent_message_to_value(input_message);
  return stream_input(session, std::move(input_message), input_value, model_settings, preface_messages,
                      std::move(tool_context), std::move(runtime_context), cancellation);
}

AgentLoopStreamResult AgentLoop::stream_input(SessionMemory& session, AgentMessage input_message, Value input_value,
                                              const ModelSettings& model_settings,
                                              const std::vector<AgentMessage>& preface_messages,
                                              ToolExecutionContext tool_context,
                                              Value runtime_context,
                                              CancellationToken* cancellation) {
  const std::string input_text = extract_text_content(input_message.content);
  std::vector<AgentLoopStreamEvent> events;
  std::vector<AgentTraceEntry> trace;
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  session.add(std::move(input_message));
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  ModelResponse last_response;
  std::string last_assistant_text;

  for (int iteration = 0; iteration < config_.max_iterations; ++iteration) {
    if (cancellation) {
      cancellation->throw_if_cancelled(ExecutionTarget::Run);
    }
    const auto model_trace = child_or_root_trace_context(config_.trace_context);
    events.push_back(AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::IterationStart,
        .iteration = iteration,
    });
    maybe_auto_compact_session(session, config_.event_bus, config_.trace_context);
    const auto prompt_messages = build_prompt_messages(session, input_text, input_value, iteration, trace,
                                                       preface_messages, runtime_context);
    const Value model_request = Value::object({
        {"iteration", iteration},
        {"messageCount", prompt_messages.size()},
        {"toolCount", config_.tool_registry->descriptors().size()},
        {"model", model_settings.model},
        {"stream", true},
    });
    if (config_.hooks.before_model) {
      ModelHookContext hook_context;
      hook_context.target = ExecutionTarget::Model;
      hook_context.trace_id = model_trace.trace_id;
      hook_context.run_id = model_trace.run_id;
      hook_context.workflow_run_id = model_trace.workflow_run_id;
      hook_context.request = model_request;
      config_.hooks.before_model(hook_context);
    }
    if (config_.event_bus) {
      config_.event_bus->publish("model.started", ExecutionTarget::Model,
                                 Value::object({{"iteration", iteration}, {"messageCount", prompt_messages.size()}}),
                                 model_trace);
    }

    ModelResponse response;
    bool saw_response = false;
    bool model_event_emitted = false;
    auto handle_model_event = [&](const ModelStreamEvent& event) {
      if (event.type == ModelStreamEventType::ResponseStart) {
        events.push_back(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::ModelStart,
            .iteration = iteration,
            .provider = event.provider,
            .model = event.model,
        });
        return;
      }
      if (event.type == ModelStreamEventType::TextDelta) {
        events.push_back(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::ModelTextDelta,
            .iteration = iteration,
            .provider = event.provider,
            .model = event.model,
            .delta = event.delta,
            .text = event.text,
        });
        if (config_.event_bus) {
          config_.event_bus->publish("model.delta", ExecutionTarget::Model,
                                     Value::object({{"iteration", iteration},
                                                    {"delta", event.delta},
                                                    {"text", event.text}}),
                                     model_trace);
        }
        return;
      }
      if (event.type == ModelStreamEventType::ReasoningDelta) {
        events.push_back(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::ModelReasoningDelta,
            .iteration = iteration,
            .provider = event.provider,
            .model = event.model,
            .delta = event.delta,
            .reasoning = event.reasoning,
        });
        if (config_.event_bus) {
          config_.event_bus->publish("model.reasoning_delta", ExecutionTarget::Model,
                                     Value::object({{"iteration", iteration},
                                                    {"delta", event.delta},
                                                    {"reasoning", event.reasoning}}),
                                     model_trace);
        }
        return;
      }
      if (event.type == ModelStreamEventType::ContentPart) {
        return;
      }
      if (event.type == ModelStreamEventType::ToolCallDelta) {
        events.push_back(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::ToolCallArgumentDelta,
            .iteration = iteration,
            .provider = event.provider,
            .model = event.model,
            .tool_call_id = event.tool_call_id,
            .tool_call_name = event.tool_call_name,
            .tool_call_args_delta = event.tool_call_args_delta,
            .tool_call_args_accumulated = event.tool_call_args_accumulated,
        });
        if (config_.event_bus) {
          config_.event_bus->publish("model.tool_call_delta", ExecutionTarget::Model,
                                     Value::object({
                                         {"provider", event.provider},
                                         {"model", event.model},
                                         {"toolCallId", event.tool_call_id},
                                         {"toolName", event.tool_call_name},
                                         {"argsDelta", event.tool_call_args_delta},
                                         {"argsAccumulated", event.tool_call_args_accumulated},
                                         {"iteration", static_cast<long long>(iteration)},
                                     }),
                                     model_trace);
        }
        return;
      }
      if (event.type == ModelStreamEventType::Response) {
        response = event.response;
        saw_response = true;
        events.push_back(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::ModelResponse,
            .iteration = iteration,
            .response = response,
        });
      }
    };

    auto stream_policies = config_.execution_policies;
    auto retry_it = stream_policies.retry.find(ExecutionTarget::Model);
    if (retry_it != stream_policies.retry.end()) {
      const auto retry_on = retry_it->second.retry_on;
      retry_it->second.retry_on = [retry_on, &model_event_emitted](const RetryContext& retry) {
        if (model_event_emitted) {
          return false;
        }
        return retry_on ? retry_on(retry) : false;
      };
    }

    try {
      (void)execute_with_policies(
          ExecutionTarget::Model, stream_policies, Value::object({{"iteration", iteration}}), cancellation,
          [&]() {
            std::vector<ModelStreamEvent> collected;
            config_.model->stream(GenerateParams{
                .messages = prompt_messages,
                .tools = config_.tool_registry->descriptors(),
                .settings = config_.model->resolve_settings(model_settings),
                .cancellation = cancellation,
            },
            [&](const ModelStreamEvent& event) {
              collected.push_back(event);
              model_event_emitted = true;
              handle_model_event(event);
            });
            return collected;
          },
          [&](const RetryScheduledContext& retry) {
            if (config_.event_bus) {
              config_.event_bus->publish(
                  "retry.scheduled", ExecutionTarget::Model,
                  retry_scheduled_event_payload(retry, Value::object({{"iteration", iteration},
                                                                      {"stream", true}})),
                  model_trace);
            }
          });
    } catch (const std::exception& error) {
      if (config_.hooks.on_model_error) {
        ModelHookContext hook_context;
        hook_context.target = ExecutionTarget::Model;
        hook_context.trace_id = model_trace.trace_id;
        hook_context.run_id = model_trace.run_id;
        hook_context.workflow_run_id = model_trace.workflow_run_id;
        hook_context.request = model_request;
        hook_context.error = error.what();
        config_.hooks.on_model_error(hook_context);
      }
      if (config_.event_bus) {
        config_.event_bus->publish("model.failed", ExecutionTarget::Model,
                                   Value::object({{"iteration", iteration}, {"error", error.what()}}),
                                   model_trace);
      }
      throw;
    }
    if (!saw_response) {
      throw AgentFrameworkError("Model stream completed without a final response.");
    }
    if (config_.event_bus) {
      config_.event_bus->publish("model.completed", ExecutionTarget::Model,
                                 Value::object({{"iteration", iteration}, {"finishReason", response.finish_reason}}),
                                 model_trace);
      emit_cache_stats_event(*config_.event_bus, response, model_settings, model_trace);
    }
    if (config_.hooks.after_model) {
      ModelHookContext hook_context;
      hook_context.target = ExecutionTarget::Model;
      hook_context.trace_id = model_trace.trace_id;
      hook_context.run_id = model_trace.run_id;
      hook_context.workflow_run_id = model_trace.workflow_run_id;
      hook_context.request = model_request;
      hook_context.response = model_response_hook_value(response);
      config_.hooks.after_model(hook_context);
    }

    last_response = response;
    if (!response.text.empty()) {
      last_assistant_text = response.text;
    }
    session.add(assistant_message_from_response(response));
    trace.push_back(AgentTraceEntry{.type = "model", .iteration = iteration, .response = response});

    if (response.tool_calls.empty()) {
      const auto termination = is_incomplete_finish_reason(response.finish_reason)
                                   ? AgentLoopTerminationReason::IncompleteResponse
                                   : AgentLoopTerminationReason::Completed;
      AgentLoopRunResult result{session.session_id(), iteration + 1, response.text, response, trace,
                                session.get_messages(), termination, {}};
      result.usage = aggregate_trace_usage(result.trace);
      events.push_back(AgentLoopStreamEvent{
          .type = AgentLoopStreamEventType::Done,
          .iteration = iteration,
          .result = result,
      });
      return AgentLoopStreamResult{std::move(events), std::move(result)};
    }

    tool_context = normalize_tool_execution_context(std::move(tool_context));
    if (!tool_context.cancellation) {
      tool_context.cancellation = cancellation;
    }
    tool_context.iteration = iteration;
    tool_context.attributes["iteration"] = iteration;
    std::vector<ToolExecutionResult> tool_results;
    std::vector<AgentMessage> tool_messages;
    tool_results.reserve(response.tool_calls.size());
    tool_messages.reserve(response.tool_calls.size());
    for (const auto& tool_call : response.tool_calls) {
      events.push_back(AgentLoopStreamEvent{
          .type = AgentLoopStreamEventType::ToolStart,
          .iteration = iteration,
          .tool_call = tool_call,
      });
      ToolExecutionResult tool_result;
      try {
        tool_result = config_.tool_executor->execute_tool_call(tool_call, tool_context);
      } catch (const std::exception& error) {
        tool_result = synthetic_tool_failure_result(tool_call,
                                                    synthetic_tool_failure_message(tool_call, error));
        if (cancellation && cancellation->cancelled()) {
          events.push_back(AgentLoopStreamEvent{
              .type = AgentLoopStreamEventType::ToolComplete,
              .iteration = iteration,
              .tool_result = tool_result,
          });
          throw;
        }
      } catch (...) {
        tool_result = synthetic_tool_failure_result(
            tool_call,
            "Tool \"" + tool_call.name + "\" threw an unexpected error.");
      }
      events.push_back(AgentLoopStreamEvent{
          .type = AgentLoopStreamEventType::ToolComplete,
          .iteration = iteration,
          .tool_result = tool_result,
      });
      tool_messages.push_back(tool_result.message);
      tool_results.push_back(std::move(tool_result));
    }
    session.add_many(tool_messages);
    trace.push_back(AgentTraceEntry{.type = "tools", .iteration = iteration, .tool_results = tool_results});
    events.push_back(AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::Tools,
        .iteration = iteration,
        .tool_results = tool_results,
    });
  }

  AgentLoopRunResult result{session.session_id(),
                            config_.max_iterations,
                            last_assistant_text,
                            last_response,
                            trace,
                            session.get_messages(),
                            AgentLoopTerminationReason::MaxIterations, {}};
  result.usage = aggregate_trace_usage(result.trace);
  events.push_back(AgentLoopStreamEvent{
      .type = AgentLoopStreamEventType::Done,
      .iteration = config_.max_iterations,
      .result = result,
  });
  return AgentLoopStreamResult{std::move(events), std::move(result)};
}

AgentRunner::AgentRunner(const AgentRunnerConfig& config)
    : config_(config),
      tool_registry_(config.tools),
      context_manager_(config.contexts),
      memory_store_(config.memory_store ? config.memory_store : std::make_shared<InMemorySessionStore>()),
      scratch_store_(config.scratch_store ? config.scratch_store : std::make_shared<InMemoryScratchStore>()) {
  if (!config_.adapter) {
    throw ConfigurationError("AgentRunner requires an explicit adapter instance.");
  }
  if (config_.lazy_tool_mode) {
    tool_registry_.set_lazy_mode(true);
    for (const auto& name : config_.forced_visible_tools) {
      tool_registry_.force_visible(name);
    }
  }
}

ToolDefinition& AgentRunner::register_tool(ToolDefinition tool) {
  return tool_registry_.register_tool(std::move(tool));
}

ContextSource& AgentRunner::register_context(ContextSource source) {
  return context_manager_.register_source(std::move(source));
}

void AgentRunner::set_approval_handler(PermissionApprovalHandler approval_handler) {
  config_.approval_handler = std::move(approval_handler);
}

std::size_t AgentRunner::register_event_sink(EventBus::Sink sink) {
  EventBus* event_bus = config_.event_bus ? config_.event_bus : &owned_event_bus_;
  return event_bus->register_sink(std::move(sink));
}

void AgentRunner::unregister_event_sink(std::size_t sink_id) {
  EventBus* event_bus = config_.event_bus ? config_.event_bus : &owned_event_bus_;
  event_bus->unregister_sink(sink_id);
}

std::shared_ptr<SessionMemory> AgentRunner::get_session(const std::string& session_id) {
  return memory_store_->get(session_id);
}

SessionStore* AgentRunner::session_store() const noexcept {
  return memory_store_.get();
}

ScratchStore* AgentRunner::scratch_store() const noexcept {
  return scratch_store_.get();
}

std::shared_ptr<ChatModelAdapter> AgentRunner::adapter() const noexcept {
  return config_.adapter;
}

std::shared_ptr<ChatModelAdapter> AgentRunner::thinking_adapter() const noexcept {
  return config_.thinking_adapter;
}

std::shared_ptr<ChatModelAdapter> AgentRunner::critique_adapter() const noexcept {
  return config_.critique_adapter;
}

EventBus* AgentRunner::event_bus() const noexcept {
  return config_.event_bus ? config_.event_bus : const_cast<EventBus*>(&owned_event_bus_);
}

std::vector<AgentRunnerStreamEvent> AgentRunner::last_stream_events() const {
  std::lock_guard<std::mutex> lock(last_stream_events_mutex_);
  return last_stream_events_;
}

void AgentRunner::clear_last_stream_events() {
  std::lock_guard<std::mutex> lock(last_stream_events_mutex_);
  last_stream_events_.clear();
}

void AgentRunner::store_last_stream_events(std::vector<AgentRunnerStreamEvent> events) {
  std::lock_guard<std::mutex> lock(last_stream_events_mutex_);
  last_stream_events_ = std::move(events);
}

AgentRunnerRunResult AgentRunner::run(const std::string& input, const std::string& session_id,
                                      const ModelSettings& model_settings,
                                      RunnerRetrievalOptions retrieval_options,
                                      RunnerWritebackOptions writeback_options,
                                      std::vector<SkillActivation> skill_activations,
                                      Value context,
                                      std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                      AgentRunnerDurableOptions durable_options,
                                      CancellationToken* cancellation,
                                      bool enable_planning) {
  return run_input(create_message(MessageRole::User, input), Value(input), true, session_id, model_settings,
                   std::move(retrieval_options), std::move(writeback_options), std::move(skill_activations),
                   std::move(context), std::move(knowledge_retrieval_options), std::move(durable_options),
                   cancellation, enable_planning);
}

AgentRunnerRunResult AgentRunner::run(std::vector<MessageContentPart> input_parts, const std::string& session_id,
                                      const ModelSettings& model_settings,
                                      RunnerRetrievalOptions retrieval_options,
                                      RunnerWritebackOptions writeback_options,
                                      std::vector<SkillActivation> skill_activations,
                                      Value context,
                                      std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                      AgentRunnerDurableOptions durable_options,
                                      CancellationToken* cancellation,
                                      bool enable_planning) {
  const std::string input_text = extract_text_content(input_parts);
  return run_input(user_input_message(std::move(input_parts)), Value(input_text), true, session_id,
                   model_settings, std::move(retrieval_options), std::move(writeback_options),
                   std::move(skill_activations), std::move(context), std::move(knowledge_retrieval_options),
                   std::move(durable_options), cancellation, enable_planning);
}

AgentRunnerRunResult AgentRunner::run(AgentMessage input_message, const std::string& session_id,
                                      const ModelSettings& model_settings,
                                      RunnerRetrievalOptions retrieval_options,
                                      RunnerWritebackOptions writeback_options,
                                      std::vector<SkillActivation> skill_activations,
                                      Value context,
                                      std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                      AgentRunnerDurableOptions durable_options,
                                      CancellationToken* cancellation,
                                      bool enable_planning) {
  const Value input_value = agent_message_to_value(input_message);
  return run_input(std::move(input_message), input_value, false, session_id, model_settings,
                   std::move(retrieval_options), std::move(writeback_options), std::move(skill_activations),
                   std::move(context), std::move(knowledge_retrieval_options), std::move(durable_options),
                   cancellation, enable_planning);
}

AgentRunnerRunResult AgentRunner::run_input(AgentMessage input_message, Value input_value,
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
                                            bool enable_planning) {
  const auto* resume_state = durable_options.resume_state ? &*durable_options.resume_state : nullptr;
  if (resume_state) {
    if (has_durable_input_message(resume_state->input_message)) {
      input_message = resume_state->input_message;
    } else if (!resume_state->input_parts.empty()) {
      input_message = user_input_message(resume_state->input_parts);
    }
    input_value = resume_state->input_value.is_null()
                      ? agent_message_to_value(input_message)
                      : resume_state->input_value;
  }
  const std::string input = extract_text_content(input_message.content);
  const auto input_parts = input_message.content;
  const Value run_input_value = input_value.is_null() ? Value(input) : input_value;
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  const std::string effective_session_id =
      resume_state && !resume_state->session_id.empty() ? resume_state->session_id : session_id;
  const std::string run_id =
      resume_state && !resume_state->run_id.empty() ? resume_state->run_id : generate_uuid();
  const Value run_hook_context_value = context;
  const auto run_trace = runner_trace_context(run_hook_context_value, run_id, "agent.run");
  EventBus* event_bus = config_.event_bus ? config_.event_bus : &owned_event_bus_;
  if (event_bus) {
    event_bus->publish("run.started", ExecutionTarget::Run,
                       run_started_event_payload(run_input_value, effective_session_id, run_hook_context_value, run_id),
                       run_trace);
  }
  try {
  if (config_.hooks.before_run) {
    RunHookContext hook_context;
    hook_context.target = ExecutionTarget::Run;
    hook_context.trace_id = run_trace.trace_id;
    hook_context.run_id = run_trace.run_id;
    hook_context.workflow_run_id = run_trace.workflow_run_id;
    hook_context.input = run_input_value;
    hook_context.context = run_hook_context_value;
    hook_context.metadata = Value::object({{"sessionId", effective_session_id}, {"runId", run_id}});
    config_.hooks.before_run(hook_context);
  }
  auto session = get_session(effective_session_id);
  std::vector<AgentMessage> preface_messages;
  std::vector<RetrievedMemory> memory_hits;
  std::vector<KnowledgeSearchHit> knowledge_hits;
  Value knowledge_debug = Value::object({});
  const auto base_model_settings = model_settings_empty(model_settings) ? config_.model_settings : model_settings;
  SkillResolveOptions skill_opts;
  skill_opts.input_text = input;
  skill_opts.session_id = effective_session_id;
  skill_opts.model_settings = base_model_settings;
  skill_opts.model_conflict = config_.skill_model_conflict_policy;
  skill_opts.effort_conflict = config_.skill_effort_conflict_policy;
  for (const auto& name : config_.default_skills) {
    skill_opts.activations.push_back(SkillActivation{name, "", SkillActivationSource::Host, 0});
  }
  for (auto& activation : skill_activations) {
    skill_opts.activations.push_back(std::move(activation));
  }
  const auto skill_state = resolve_skills_state(config_.skills.get(), std::move(skill_opts));
  const std::string resolved_effective_input =
      allow_skill_input_rewrite && !skill_state.effective_input_text.empty()
          ? skill_state.effective_input_text
          : input;
  const std::string effective_input =
      resume_state && !resume_state->effective_input.empty() ? resume_state->effective_input
                                                             : resolved_effective_input;
  const std::string input_text =
      resume_state && !resume_state->input_text.empty() ? resume_state->input_text : effective_input;
  const Value effective_input_value =
      resume_state && !resume_state->effective_input_value.is_null()
          ? resume_state->effective_input_value
          : input_value_for_effective_input(run_input_value, effective_input);
  const AgentMessage effective_input_message =
      resume_state && has_durable_input_message(resume_state->effective_input_message)
          ? resume_state->effective_input_message
          : (allow_skill_input_rewrite
                 ? input_message_for_effective_input(input_message, input, effective_input)
                 : input_message);
  RunnerKnowledgeQuery knowledge_query = resolve_runner_knowledge_query(effective_input_message);
  if (effective_input_value.is_string() && !trim_copy(effective_input).empty()) {
    knowledge_query = text_knowledge_query(effective_input);
  }
  SkillPrefaceBuildResult skill_preface;
  if (!resume_state) {
    skill_preface = build_skill_preface_messages(this, config_, skill_state, effective_input,
                                                 effective_session_id, context);
  }

  const RunnerRetrievalOptions retrieval =
      merge_runner_retrieval_options(config_.retrieval_options, retrieval_options);
  const RunnerWritebackOptions writeback =
      merge_runner_writeback_options(config_.writeback_options, writeback_options);
  const RunnerKnowledgeRetrievalOptions knowledge_retrieval =
      knowledge_retrieval_options ? *knowledge_retrieval_options : config_.knowledge_retrieval_options;
  std::optional<ExecutionPlan> plan;
  if (resume_state) {
    memory_hits = resume_state->memory_hits;
    knowledge_hits = resume_state->knowledge_hits;
    knowledge_debug = resume_state->knowledge_debug;
    preface_messages = resume_state->preface_messages;
    plan = resume_state->plan;
  } else {
    if (config_.advertise_skills && skill_state.available_message) {
      preface_messages.push_back(*skill_state.available_message);
    }
    preface_messages.insert(preface_messages.end(), skill_preface.messages.begin(), skill_preface.messages.end());

    if (config_.knowledge_base || config_.knowledge_base_manager) {
      auto knowledge_result = retrieve_knowledge_context(config_.knowledge_base, config_.knowledge_base_manager,
                                                         knowledge_query, knowledge_retrieval, config_.hooks,
                                                         event_bus, config_.execution_policies, cancellation);
      knowledge_hits = std::move(knowledge_result.hits);
      knowledge_debug = std::move(knowledge_result.debug);
      if (knowledge_result.message) {
        preface_messages.push_back(*knowledge_result.message);
      }
    }

    if (config_.long_term_memory && runner_retrieval_enabled(retrieval)) {
      auto retrieval_result = retrieve_long_term_memory_context(*config_.long_term_memory, effective_input,
                                                               retrieval, config_.hooks, event_bus,
                                                               config_.execution_policies, cancellation);
      memory_hits = std::move(retrieval_result.hits);
      if (retrieval_result.message) {
        preface_messages.push_back(*retrieval_result.message);
      }
    }

    plan = create_runner_execution_plan(config_, effective_input, session.get(),
                                        context, &tool_registry_, memory_hits,
                                        knowledge_hits,
                                        event_bus, cancellation, enable_planning);
    if (plan) {
      preface_messages.push_back(create_plan_message(*plan));
    }
  }

  ToolExecutor executor(tool_registry_, config_.permission_policy, config_.approval_handler, event_bus,
                        config_.execution_policies, config_.hooks);
  AgentLoop loop(AgentLoopConfig{config_.adapter,
                                 &tool_registry_,
                                 &executor,
                                 &context_manager_,
                                 config_.system_prompt,
                                 config_.max_iterations,
                                 event_bus,
                                 config_.execution_policies,
                                 config_.hooks,
                                 run_trace});
  std::optional<Value> skill_services;
  if (!skill_state.active_skills.empty() || !skill_state.allowed_tools.empty()
      || !skill_preface.fork_executions.empty()) {
    skill_services = skill_services_value(skill_state, skill_preface.fork_executions);
  }
  std::optional<AgentLoopDurableState> latest_loop_state = resume_state ? resume_state->loop : std::nullopt;
  auto checkpoint_runner = [&](AgentRunnerDurableStatus status,
                               std::optional<AgentLoopDurableState> loop_state) {
    latest_loop_state = std::move(loop_state);
    if (!durable_options.on_checkpoint) {
      return;
    }
    AgentRunnerDurableState state;
    state.version = 1;
    state.status = status;
    state.run_id = run_id;
    state.session_id = effective_session_id;
    state.input = input;
    state.input_value = run_input_value;
    state.input_message = input_message;
    state.model_settings = skill_state.model_settings;
    state.effective_input = effective_input;
    state.effective_input_value = effective_input_value;
    state.effective_input_message = effective_input_message;
    state.input_text = input_text;
    state.input_parts = input_parts;
    state.plan = plan;
    state.memory_hits = memory_hits;
    state.knowledge_hits = knowledge_hits;
    state.knowledge_debug = knowledge_debug;
    state.preface_messages = preface_messages;
    state.loop = latest_loop_state;
    state.updated_at = now_iso8601();
    durable_options.on_checkpoint(state);
  };
  if (!resume_state) {
    checkpoint_runner(AgentRunnerDurableStatus::Running, std::nullopt);
  }
  Value loop_context = runner_loop_context(context, plan, memory_hits, knowledge_hits, knowledge_debug);
  ToolExecutionContext base_tool_context;
  base_tool_context.cancellation = cancellation;
  base_tool_context.trace_context = run_trace;
  if (loop_context.at("services").is_object()) {
    base_tool_context.services = loop_context.at("services");
  }
  if (loop_context.is_object() && !loop_context.as_object().empty()) {
    base_tool_context.attributes["context"] = loop_context;
  }
  auto effective_runtime_services = runner_runtime_services(
      session.get(), effective_session_id, config_.long_term_memory.get(), config_.knowledge_base,
      config_.knowledge_base_manager, loop_context, skill_services);
  effective_runtime_services.scratch_store = scratch_store_.get();
  ToolExecutionContext tool_context = with_tool_execution_services(
      std::move(base_tool_context),
      config_.tool_services,
      effective_runtime_services);
  AgentLoopDurableOptions loop_durable_options;
  if (resume_state && resume_state->loop) {
    loop_durable_options.resume_state = resume_state->loop;
  }
  if (durable_options.on_checkpoint) {
    loop_durable_options.on_checkpoint = [&](const AgentLoopDurableState& loop_state) {
      checkpoint_runner(AgentRunnerDurableStatus::Running, loop_state);
    };
  }
  AgentLoopRunResult base =
      effective_input_value.is_string()
          ? loop.run(*session, effective_input_message.content, skill_state.model_settings,
                     preface_messages, tool_context, std::move(loop_context),
                     std::move(loop_durable_options), cancellation)
          : loop.run(*session, effective_input_message, skill_state.model_settings,
                     preface_messages, tool_context, std::move(loop_context),
                     std::move(loop_durable_options), cancellation);
  if (memory_store_) {
    memory_store_->flush(effective_session_id);
  }

  if (should_apply_runner_writeback(writeback, config_.long_term_memory.get(), input_text, base.text)) {
    config_.long_term_memory->remember_conversation_turn(effective_session_id, input_text, base.text,
                                                         writeback.metadata, writeback.namespace_id,
                                                         execution_plan_step_titles(plan));
  }

  auto result = runner_result_from_loop(std::move(base), std::move(memory_hits),
                                        std::move(knowledge_hits), std::move(knowledge_debug), plan);
  if (config_.hooks.after_run) {
    RunHookContext hook_context;
    hook_context.target = ExecutionTarget::Run;
    hook_context.trace_id = run_trace.trace_id;
    hook_context.run_id = run_trace.run_id;
    hook_context.workflow_run_id = run_trace.workflow_run_id;
    hook_context.input = run_input_value;
    hook_context.context = run_hook_context_value;
    hook_context.result = Value::object({
        {"sessionId", result.session_id},
        {"iterationCount", result.iteration_count},
        {"text", result.text},
        {"terminationReason", to_string(result.termination_reason)},
        {"memoryHitCount", result.memory_hits.size()},
        {"knowledgeHitCount", result.knowledge_hits.size()},
    });
    if (result.plan) {
      hook_context.result["plan"] = execution_plan_summary_value(*result.plan);
    }
    hook_context.metadata = Value::object({{"sessionId", effective_session_id}, {"runId", run_id}});
    config_.hooks.after_run(hook_context);
  }
  if (event_bus) {
    event_bus->publish("run.completed", ExecutionTarget::Run, run_completed_event_payload(result, run_id),
                       run_trace);
  }
  checkpoint_runner(AgentRunnerDurableStatus::Completed, latest_loop_state);
  return result;
  } catch (const std::exception& error) {
    if (config_.hooks.on_run_error) {
      RunHookContext hook_context;
      hook_context.target = ExecutionTarget::Run;
      hook_context.trace_id = run_trace.trace_id;
      hook_context.run_id = run_trace.run_id;
      hook_context.workflow_run_id = run_trace.workflow_run_id;
      hook_context.input = run_input_value;
      hook_context.context = run_hook_context_value;
      hook_context.error = error.what();
      hook_context.metadata = Value::object({{"sessionId", effective_session_id}, {"runId", run_id}});
      config_.hooks.on_run_error(hook_context);
    }
    if (event_bus) {
      event_bus->publish("run.failed", ExecutionTarget::Run,
                         run_failed_event_payload(run_input_value, effective_session_id, error.what(), run_id),
                         run_trace);
    }
    throw;
  }
}

AgentRunnerStreamResult AgentRunner::stream(const std::string& input, const std::string& session_id,
                                            const ModelSettings& model_settings,
                                            RunnerRetrievalOptions retrieval_options,
                                            RunnerWritebackOptions writeback_options,
                                            std::vector<SkillActivation> skill_activations,
                                            Value context,
                                            std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                            CancellationToken* cancellation,
                                            bool enable_planning) {
  return stream_input(create_message(MessageRole::User, input), Value(input), true, session_id, model_settings,
                      std::move(retrieval_options), std::move(writeback_options), std::move(skill_activations),
                      std::move(context), std::move(knowledge_retrieval_options), cancellation, enable_planning);
}

AgentRunnerStreamResult AgentRunner::stream(std::vector<MessageContentPart> input_parts,
                                            const std::string& session_id,
                                            const ModelSettings& model_settings,
                                            RunnerRetrievalOptions retrieval_options,
                                            RunnerWritebackOptions writeback_options,
                                            std::vector<SkillActivation> skill_activations,
                                            Value context,
                                            std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                            CancellationToken* cancellation,
                                            bool enable_planning) {
  const std::string input_text = extract_text_content(input_parts);
  return stream_input(user_input_message(std::move(input_parts)), Value(input_text), true, session_id,
                      model_settings, std::move(retrieval_options), std::move(writeback_options),
                      std::move(skill_activations), std::move(context), std::move(knowledge_retrieval_options),
                      cancellation, enable_planning);
}

AgentRunnerStreamResult AgentRunner::stream(AgentMessage input_message,
                                            const std::string& session_id,
                                            const ModelSettings& model_settings,
                                            RunnerRetrievalOptions retrieval_options,
                                            RunnerWritebackOptions writeback_options,
                                            std::vector<SkillActivation> skill_activations,
                                            Value context,
                                            std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                            CancellationToken* cancellation,
                                            bool enable_planning) {
  const Value input_value = agent_message_to_value(input_message);
  return stream_input(std::move(input_message), input_value, false, session_id, model_settings,
                      std::move(retrieval_options), std::move(writeback_options), std::move(skill_activations),
                      std::move(context), std::move(knowledge_retrieval_options), cancellation, enable_planning);
}

AgentRunnerStreamResult AgentRunner::stream_input(AgentMessage input_message, Value input_value,
                                                  bool allow_skill_input_rewrite,
                                                  const std::string& session_id,
                                                  const ModelSettings& model_settings,
                                                  RunnerRetrievalOptions retrieval_options,
                                                  RunnerWritebackOptions writeback_options,
                                                  std::vector<SkillActivation> skill_activations,
                                                  Value context,
                                                  std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                                  CancellationToken* cancellation,
                                                  bool enable_planning) {
  const std::string input = extract_text_content(input_message.content);
  std::vector<AgentRunnerStreamEvent> events;
  clear_last_stream_events();
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  const Value run_input_value = input_value.is_null() ? Value(input) : input_value;
  const std::string run_id = generate_uuid();
  const auto run_trace = runner_trace_context(context, run_id, "agent.stream");
  const Value run_hook_context_value = context;
  if (config_.hooks.before_run) {
    RunHookContext hook_context;
    hook_context.target = ExecutionTarget::Run;
    hook_context.trace_id = run_trace.trace_id;
    hook_context.run_id = run_trace.run_id;
    hook_context.workflow_run_id = run_trace.workflow_run_id;
    hook_context.input = run_input_value;
    hook_context.context = run_hook_context_value;
    hook_context.metadata = Value::object({{"sessionId", session_id}, {"runId", run_id}});
    config_.hooks.before_run(hook_context);
  }
  EventBus* event_bus = config_.event_bus ? config_.event_bus : &owned_event_bus_;
  if (event_bus) {
    event_bus->publish("run.started", ExecutionTarget::Run,
                       run_started_event_payload(run_input_value, session_id, context, run_id),
                       run_trace);
  }
  try {
  auto session = get_session(session_id);
  std::vector<AgentMessage> preface_messages;
  std::vector<RetrievedMemory> memory_hits;
  std::vector<KnowledgeSearchHit> knowledge_hits;
  Value knowledge_debug = Value::object({});
  const auto base_model_settings = model_settings_empty(model_settings) ? config_.model_settings : model_settings;
  SkillResolveOptions skill_opts;
  skill_opts.input_text = input;
  skill_opts.session_id = session_id;
  skill_opts.model_settings = base_model_settings;
  skill_opts.model_conflict = config_.skill_model_conflict_policy;
  skill_opts.effort_conflict = config_.skill_effort_conflict_policy;
  for (const auto& name : config_.default_skills) {
    skill_opts.activations.push_back(SkillActivation{name, "", SkillActivationSource::Host, 0});
  }
  for (auto& activation : skill_activations) {
    skill_opts.activations.push_back(std::move(activation));
  }
  const auto skill_state = resolve_skills_state(config_.skills.get(), std::move(skill_opts));
  const std::string effective_input =
      allow_skill_input_rewrite && !skill_state.effective_input_text.empty()
          ? skill_state.effective_input_text
          : input;
  const AgentMessage effective_input_message =
      allow_skill_input_rewrite ? input_message_for_effective_input(input_message, input, effective_input)
                                : input_message;
  RunnerKnowledgeQuery knowledge_query = resolve_runner_knowledge_query(effective_input_message);
  if (run_input_value.is_string() && !trim_copy(effective_input).empty()) {
    knowledge_query = text_knowledge_query(effective_input);
  }
  const auto skill_preface = build_skill_preface_messages(this, config_, skill_state, effective_input,
                                                          session_id, context);

  if (config_.advertise_skills && skill_state.available_message) {
    preface_messages.push_back(*skill_state.available_message);
  }
  preface_messages.insert(preface_messages.end(), skill_preface.messages.begin(), skill_preface.messages.end());

  const RunnerRetrievalOptions retrieval =
      merge_runner_retrieval_options(config_.retrieval_options, retrieval_options);
  const RunnerWritebackOptions writeback =
      merge_runner_writeback_options(config_.writeback_options, writeback_options);
  const RunnerKnowledgeRetrievalOptions knowledge_retrieval =
      knowledge_retrieval_options ? *knowledge_retrieval_options : config_.knowledge_retrieval_options;

  const bool has_knowledge_provider = config_.knowledge_base || config_.knowledge_base_manager;
  const bool will_retrieve_knowledge =
      has_knowledge_provider && knowledge_retrieval.enabled && !runner_knowledge_query_empty(knowledge_query);
  if (will_retrieve_knowledge) {
    events.push_back(AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Status,
        .status = runner_status("working", "knowledge-retrieval", "start",
                                "Retrieving knowledge context."),
    });
  }
  RunnerKnowledgeRetrievalResult knowledge_result;
  if (has_knowledge_provider) {
    knowledge_result = retrieve_knowledge_context(config_.knowledge_base, config_.knowledge_base_manager,
                                                  knowledge_query, knowledge_retrieval, config_.hooks,
                                                  event_bus, config_.execution_policies, cancellation);
    knowledge_hits = std::move(knowledge_result.hits);
    knowledge_debug = std::move(knowledge_result.debug);
  }
  events.push_back(AgentRunnerStreamEvent{
      .type = AgentRunnerStreamEventType::KnowledgeRetrieval,
      .knowledge_hits = knowledge_hits,
      .knowledge_message = knowledge_result.message,
      .knowledge_debug = knowledge_debug,
  });
  if (knowledge_result.message) {
    preface_messages.push_back(*knowledge_result.message);
  }
  if (will_retrieve_knowledge) {
    events.push_back(AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Status,
        .status = runner_status(
            "working", "knowledge-retrieval", "complete",
            knowledge_hits.empty() ? "Knowledge retrieval completed with no matches."
                                   : "Retrieved " + std::to_string(knowledge_hits.size()) + " knowledge hits.",
            Value::object({{"hits", knowledge_hits.size()}})),
    });
  }

  const bool will_retrieve_memory =
      config_.long_term_memory && runner_retrieval_enabled(retrieval) && !trim_copy(effective_input).empty();
  if (will_retrieve_memory) {
    events.push_back(AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Status,
        .status = runner_status("working", "memory-retrieval", "start",
                                "Retrieving long-term memory."),
    });
  }
  RunnerMemoryRetrievalResult retrieval_result;
  if (will_retrieve_memory) {
    retrieval_result = retrieve_long_term_memory_context(*config_.long_term_memory, effective_input,
                                                        retrieval, config_.hooks, event_bus,
                                                        config_.execution_policies, cancellation);
    memory_hits = std::move(retrieval_result.hits);
  }
  events.push_back(AgentRunnerStreamEvent{
      .type = AgentRunnerStreamEventType::MemoryRetrieval,
      .memory_hits = memory_hits,
      .memory_message = retrieval_result.message,
  });
  if (retrieval_result.message) {
    preface_messages.push_back(*retrieval_result.message);
  }
  if (will_retrieve_memory) {
    events.push_back(AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Status,
        .status = runner_status(
            "working", "memory-retrieval", "complete",
            memory_hits.empty() ? "Memory retrieval completed with no matches."
                                : "Retrieved " + std::to_string(memory_hits.size()) + " memory hits.",
            Value::object({{"hits", memory_hits.size()}})),
    });
  }

  const bool planning_enabled = enable_planning && config_.enable_planning && config_.planner;
  if (planning_enabled) {
    events.push_back(AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Status,
        .status = runner_status("working", "planning", "start",
                                "Building an execution plan."),
    });
  }
  std::optional<ExecutionPlan> plan = create_runner_execution_plan(config_, effective_input, session.get(),
                                                                   context, &tool_registry_, memory_hits,
                                                                   knowledge_hits,
                                                                   event_bus, cancellation, enable_planning);
  if (planning_enabled) {
    if (plan) {
      events.push_back(AgentRunnerStreamEvent{
          .type = AgentRunnerStreamEventType::Planning,
          .plan = plan,
      });
    }
    events.push_back(AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Status,
        .status = runner_status("working", "planning", "complete",
                                plan ? "Execution plan is ready."
                                     : "Planning completed without a structured plan.",
                                Value::object({{"hasPlan", plan.has_value()}})),
    });
  }
  if (plan) {
    preface_messages.push_back(create_plan_message(*plan));
  }

  ToolExecutor executor(tool_registry_, config_.permission_policy, config_.approval_handler, event_bus,
                        config_.execution_policies, config_.hooks);
  AgentLoop loop(AgentLoopConfig{config_.adapter,
                                 &tool_registry_,
                                 &executor,
                                 &context_manager_,
                                 config_.system_prompt,
                                 config_.max_iterations,
                                 event_bus,
                                 config_.execution_policies,
                                 config_.hooks,
                                 run_trace});
  std::optional<Value> skill_services;
  if (!skill_state.active_skills.empty() || !skill_state.allowed_tools.empty()
      || !skill_preface.fork_executions.empty()) {
    skill_services = skill_services_value(skill_state, skill_preface.fork_executions);
  }
  Value loop_context = runner_loop_context(context, plan, memory_hits, knowledge_hits, knowledge_debug);
  ToolExecutionContext base_tool_context;
  base_tool_context.cancellation = cancellation;
  base_tool_context.trace_context = run_trace;
  if (loop_context.at("services").is_object()) {
    base_tool_context.services = loop_context.at("services");
  }
  if (loop_context.is_object() && !loop_context.as_object().empty()) {
    base_tool_context.attributes["context"] = loop_context;
  }
  auto stream_runtime_services = runner_runtime_services(
      session.get(), session_id, config_.long_term_memory.get(), config_.knowledge_base,
      config_.knowledge_base_manager, loop_context, skill_services);
  stream_runtime_services.scratch_store = scratch_store_.get();
  ToolExecutionContext tool_context = with_tool_execution_services(
      std::move(base_tool_context),
      config_.tool_services,
      stream_runtime_services);
  auto loop_stream =
      run_input_value.is_string()
          ? loop.stream(*session, effective_input_message.content, skill_state.model_settings,
                        preface_messages, tool_context, std::move(loop_context), cancellation)
          : loop.stream(*session, effective_input_message, skill_state.model_settings,
                        preface_messages, tool_context, std::move(loop_context), cancellation);
  if (memory_store_) {
    memory_store_->flush(session_id);
  }

  if (should_apply_runner_writeback(writeback, config_.long_term_memory.get(), effective_input,
                                    loop_stream.result.text)) {
    config_.long_term_memory->remember_conversation_turn(session_id, effective_input, loop_stream.result.text,
                                                         writeback.metadata, writeback.namespace_id,
                                                         execution_plan_step_titles(plan));
  }

  auto result = runner_result_from_loop(std::move(loop_stream.result), memory_hits,
                                        knowledge_hits, knowledge_debug, plan);
  if (config_.hooks.after_run) {
    RunHookContext hook_context;
    hook_context.target = ExecutionTarget::Run;
    hook_context.trace_id = run_trace.trace_id;
    hook_context.run_id = run_trace.run_id;
    hook_context.workflow_run_id = run_trace.workflow_run_id;
    hook_context.input = run_input_value;
    hook_context.context = run_hook_context_value;
    hook_context.result = Value::object({
        {"sessionId", result.session_id},
        {"iterationCount", result.iteration_count},
        {"text", result.text},
        {"terminationReason", to_string(result.termination_reason)},
        {"memoryHitCount", result.memory_hits.size()},
        {"knowledgeHitCount", result.knowledge_hits.size()},
    });
    if (result.plan) {
      hook_context.result["plan"] = execution_plan_summary_value(*result.plan);
    }
    hook_context.metadata = Value::object({{"sessionId", session_id}, {"runId", run_id}});
    config_.hooks.after_run(hook_context);
  }
  if (event_bus) {
    event_bus->publish("run.completed", ExecutionTarget::Run, run_completed_event_payload(result, run_id),
                       run_trace);
  }
  for (auto& event : loop_stream.events) {
    auto status = status_from_loop_event(event);
    if (status) {
      events.push_back(AgentRunnerStreamEvent{
          .type = AgentRunnerStreamEventType::Status,
          .status = std::move(*status),
      });
    }
    if (event.type == AgentLoopStreamEventType::ToolCallArgumentDelta) {
      events.push_back(AgentRunnerStreamEvent{
          .type = AgentRunnerStreamEventType::ToolCallArgumentDelta,
          .tool_call_id = event.tool_call_id,
          .tool_call_name = event.tool_call_name,
          .tool_call_args_delta = event.tool_call_args_delta,
          .tool_call_args_accumulated = event.tool_call_args_accumulated,
      });
      continue;
    }
    events.push_back(AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Loop,
        .loop_event = std::move(event),
    });
  }
  events.push_back(AgentRunnerStreamEvent{
      .type = AgentRunnerStreamEventType::Done,
      .result = result,
  });
  AgentRunnerStreamResult stream_result{std::move(events), std::move(result)};
  store_last_stream_events(stream_result.events);
  return stream_result;
  } catch (const std::exception& error) {
    if (config_.hooks.on_run_error) {
      RunHookContext hook_context;
      hook_context.target = ExecutionTarget::Run;
      hook_context.trace_id = run_trace.trace_id;
      hook_context.run_id = run_trace.run_id;
      hook_context.workflow_run_id = run_trace.workflow_run_id;
      hook_context.input = run_input_value;
      hook_context.context = run_hook_context_value;
      hook_context.error = error.what();
      hook_context.metadata = Value::object({{"sessionId", session_id}, {"runId", run_id}});
      config_.hooks.on_run_error(hook_context);
    }
    if (event_bus) {
      event_bus->publish("run.failed", ExecutionTarget::Run,
                         run_failed_event_payload(run_input_value, session_id, error.what(), run_id),
                         run_trace);
    }
    events.push_back(AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Error,
        .error = stream_error_payload(error),
    });
    store_last_stream_events(events);
    throw;
  }
}
}  // namespace agent
