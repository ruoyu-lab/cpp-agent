#include "agent/async.hpp"

#include "detail/helpers.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace agent {

namespace {

InMemoryTaskStore& require_async_task_store(InMemoryTaskStore* store) {
  if (!store) {
    throw ConfigurationError("AsyncAgentRun requires a task store.");
  }
  return *store;
}

InMemoryTaskQueue& require_async_queue(InMemoryTaskQueue* queue) {
  if (!queue) {
    throw ConfigurationError("AsyncAgentRun requires a task queue.");
  }
  return *queue;
}

AsyncAgentRunStore& require_async_store(AsyncAgentRunStore* store) {
  if (!store) {
    throw ConfigurationError("AsyncAgentRun requires an async run store.");
  }
  return *store;
}

TaskStatus task_status_from_async_status(AsyncAgentRunStatus status) {
  switch (status) {
    case AsyncAgentRunStatus::Queued:
      return TaskStatus::Queued;
    case AsyncAgentRunStatus::Running:
      return TaskStatus::Running;
    case AsyncAgentRunStatus::Waiting:
      return TaskStatus::Waiting;
    case AsyncAgentRunStatus::Completed:
      return TaskStatus::Completed;
    case AsyncAgentRunStatus::Failed:
      return TaskStatus::Failed;
    case AsyncAgentRunStatus::Cancelled:
      return TaskStatus::Cancelled;
  }
  return TaskStatus::Queued;
}

AsyncAgentRunStatus async_status_from_task_status(TaskStatus status) {
  switch (status) {
    case TaskStatus::Queued:
      return AsyncAgentRunStatus::Queued;
    case TaskStatus::Running:
      return AsyncAgentRunStatus::Running;
    case TaskStatus::Waiting:
      return AsyncAgentRunStatus::Waiting;
    case TaskStatus::Completed:
      return AsyncAgentRunStatus::Completed;
    case TaskStatus::Failed:
    case TaskStatus::Interrupted:
      return AsyncAgentRunStatus::Failed;
    case TaskStatus::Cancelled:
      return AsyncAgentRunStatus::Cancelled;
  }
  return AsyncAgentRunStatus::Queued;
}

Value string_list_to_value(const std::vector<std::string>& values) {
  Value::Array output;
  output.reserve(values.size());
  for (const auto& value : values) {
    output.push_back(value);
  }
  return Value(std::move(output));
}

std::vector<std::string> string_list_from_value(const Value& value) {
  std::vector<std::string> output;
  if (!value.is_array()) {
    return output;
  }
  for (const auto& item : value.as_array()) {
    output.push_back(item.as_string());
  }
  return output;
}

long long now_ms() {
  return time_point_to_ms(std::chrono::system_clock::now());
}

bool is_child_kind(const std::string& kind) {
  return kind == kAsyncAgentRunKindSubagent;
}

bool is_child_start_input(const AsyncAgentRunStartInput& input) {
  return !input.parent_run_id.empty() || is_child_kind(input.kind);
}

bool is_child_run(const AsyncAgentRun& run) {
  return !run.parent_run_id.empty() || is_child_kind(run.kind);
}

bool is_active_child_status(AsyncAgentRunStatus status) {
  return status == AsyncAgentRunStatus::Queued ||
         status == AsyncAgentRunStatus::Running ||
         status == AsyncAgentRunStatus::Waiting;
}

std::string normalized_role(std::string role, std::string default_role) {
  if (role.empty()) {
    return default_role;
  }
  if (role != "leaf" && role != "orchestrator") {
    throw ConfigurationError("Async agent run role must be either leaf or orchestrator.");
  }
  return role;
}

std::string normalized_cancel_propagation(std::string value) {
  if (value.empty()) {
    return "none";
  }
  if (value != "none" && value != "direct-children" && value != "subtree") {
    throw ConfigurationError(
        "ChildAgentPolicy.cancelPropagation must be none, direct-children, or subtree.");
  }
  return value;
}

void validate_policy_limit(const std::optional<long long>& limit, const std::string& field) {
  if (limit && *limit < 0) {
    throw ConfigurationError("ChildAgentPolicy." + field + " must be greater than or equal to 0.");
  }
}

ChildAgentPolicy merge_child_agent_policy(const ChildAgentPolicy& base,
                                          const ChildAgentPolicy& overlay) {
  ChildAgentPolicy merged = base;
  if (overlay.max_global_child_runs) {
    merged.max_global_child_runs = overlay.max_global_child_runs;
  }
  if (overlay.max_child_runs_per_parent) {
    merged.max_child_runs_per_parent = overlay.max_child_runs_per_parent;
  }
  if (overlay.max_spawn_depth) {
    merged.max_spawn_depth = overlay.max_spawn_depth;
  }
  if (overlay.max_children_per_run) {
    merged.max_children_per_run = overlay.max_children_per_run;
  }
  if (overlay.allow_child_spawn) {
    merged.allow_child_spawn = overlay.allow_child_spawn;
  }
  if (overlay.cancel_propagation) {
    merged.cancel_propagation = overlay.cancel_propagation;
  }
  validate_policy_limit(merged.max_global_child_runs, "maxGlobalChildRuns");
  validate_policy_limit(merged.max_child_runs_per_parent, "maxChildRunsPerParent");
  validate_policy_limit(merged.max_spawn_depth, "maxSpawnDepth");
  validate_policy_limit(merged.max_children_per_run, "maxChildrenPerRun");
  if (merged.cancel_propagation) {
    merged.cancel_propagation = normalized_cancel_propagation(*merged.cancel_propagation);
  }
  return merged;
}

void enforce_limit(const std::optional<long long>& limit,
                   long long next_value,
                   const std::string& field,
                   const std::string& context) {
  if (limit && next_value > *limit) {
    throw AgentFrameworkError("ChildAgentPolicy violation: " + field +
                              " exceeded for " + context +
                              " (limit " + std::to_string(*limit) +
                              ", requested " + std::to_string(next_value) + ").");
  }
}

AsyncRunActivity lifecycle_activity(const std::string& phase,
                                    const std::string& target = "run",
                                    long long at_ms = 0) {
  AsyncRunActivity activity;
  activity.phase = phase;
  activity.current_target = target;
  activity.current_iteration = -1;
  activity.interruptible = phase != "completed" && phase != "failed" && phase != "cancelled";
  activity.last_activity_at_ms = at_ms > 0 ? at_ms : now_ms();
  return activity;
}

std::string activity_phase_for_status(AsyncAgentRunStatus status) {
  switch (status) {
    case AsyncAgentRunStatus::Queued:
      return "queued";
    case AsyncAgentRunStatus::Running:
      return "starting";
    case AsyncAgentRunStatus::Waiting:
      return "external_wait";
    case AsyncAgentRunStatus::Completed:
      return "completed";
    case AsyncAgentRunStatus::Failed:
      return "failed";
    case AsyncAgentRunStatus::Cancelled:
      return "cancelled";
  }
  return "queued";
}

std::string iso8601_from_epoch_ms(long long epoch_ms) {
  if (epoch_ms <= 0) {
    return "";
  }
  const auto seconds = static_cast<std::time_t>(epoch_ms / 1000);
  const std::tm* utc = std::gmtime(&seconds);
  if (!utc) {
    return "";
  }
  const std::tm tm = *utc;
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

Value empty_array_value() {
  return Value::array({});
}

Value topology_payload_from_start(const AsyncAgentRunStartInput& input) {
  return Value::object({{"kind", input.kind},
                        {"rootRunId", input.root_run_id},
                        {"parentRunId", input.parent_run_id},
                        {"depth", input.depth < 0 ? 0 : input.depth},
                        {"role", input.role.empty() ? "leaf" : input.role},
                        {"parentChildRelation", input.parent_child_relation},
                        {"spawnedByToolCallId", input.spawned_by_tool_call_id}});
}

Value topology_payload_from_run(const AsyncAgentRun& run) {
  return Value::object({{"kind", run.kind},
                        {"rootRunId", run.root_run_id},
                        {"parentRunId", run.parent_run_id},
                        {"depth", run.depth},
                        {"role", run.role},
                        {"parentChildRelation", run.parent_child_relation},
                        {"spawnedByToolCallId", run.spawned_by_tool_call_id}});
}

long long direct_child_count(InMemoryTaskStore& task_store,
                             const std::string& parent_run_id,
                             bool active_only = false) {
  if (parent_run_id.empty()) {
    return 0;
  }
  long long count = 0;
  for (const auto& task : task_store.list_tasks(TaskScopeFilter{.type = kAsyncAgentRunTaskType})) {
    auto child = async_agent_run_from_task(task);
    if (child.parent_run_id == parent_run_id &&
        (!active_only || is_active_child_status(child.status))) {
      ++count;
    }
  }
  return count;
}

ResourceLedger resource_ledger_from_runner_result(const AgentRunnerRunResult& result) {
  ResourceLedger ledger;
  ledger.input_tokens = result.usage.input_tokens;
  ledger.output_tokens = result.usage.output_tokens;
  ledger.total_tokens = result.usage.total_tokens;
  ledger.input_tokens_source = to_string(result.usage.input_tokens_source);
  ledger.output_tokens_source = to_string(result.usage.output_tokens_source);
  ledger.total_tokens_source = to_string(result.usage.total_tokens_source);
  ledger.token_usage_quality = to_string(result.usage.quality);
  ledger.cached_input_tokens = result.usage.cached_input_tokens;
  ledger.cached_input_tokens_source = to_string(result.usage.cached_input_tokens_source);
  ledger.reasoning_tokens = result.usage.reasoning_tokens;
  ledger.reasoning_source = to_string(result.usage.reasoning_source);
  ledger.provider = result.usage.provider;
  ledger.iteration_count = result.iteration_count;

  Value::Array tools;
  for (const auto& entry : result.trace) {
    for (const auto& tool_result : entry.tool_results) {
      ++ledger.tool_call_count;
      if (tool_result.ok) {
        ++ledger.tool_success_count;
      } else {
        ++ledger.tool_error_count;
      }
      tools.push_back(Value::object({{"toolName", tool_result.tool_call.name},
                                     {"toolCallId", tool_result.tool_call.id},
                                     {"ok", tool_result.ok},
                                     {"iteration", entry.iteration},
                                     {"error", tool_result.error}}));
    }
  }
  ledger.details = Value::object({{"toolResults", Value(std::move(tools))}});
  return ledger;
}

void merge_tool_event_into_ledger(ResourceLedger& ledger, const AgentRunnerStreamEvent& event) {
  if (event.type != AgentRunnerStreamEventType::Loop ||
      event.loop_event.type != AgentLoopStreamEventType::ToolComplete) {
    return;
  }
  const auto& tool_result = event.loop_event.tool_result;
  ++ledger.tool_call_count;
  if (tool_result.ok) {
    ++ledger.tool_success_count;
  } else {
    ++ledger.tool_error_count;
  }
}

std::optional<AsyncRunActivity> activity_from_runner_stream_event(const AgentRunnerStreamEvent& event,
                                                                  AsyncRunActivity current) {
  if (event.type == AgentRunnerStreamEventType::Status) {
    const auto target = event.status.stage.empty() ? event.status.kind : event.status.stage;
    if (target.find("tool") != std::string::npos) {
      current.phase = "tool_execution";
    } else if (target.find("permission") != std::string::npos ||
               target.find("approval") != std::string::npos) {
      current.phase = "permission_wait";
    } else if (target.find("child") != std::string::npos) {
      current.phase = "child_wait";
    } else if (target.find("retry") != std::string::npos) {
      current.phase = "retry_sleep";
    } else if (target.find("wait") != std::string::npos) {
      current.phase = "external_wait";
    } else {
      current.phase = "model_request";
    }
    current.current_target = target.empty() ? "run" : target;
    current.current_tool_name = event.status.tool_name;
    current.current_iteration = event.status.iteration;
    current.interruptible = true;
    current.stall_reason.clear();
    return current;
  }
  if (event.type == AgentRunnerStreamEventType::KnowledgeRetrieval) {
    current.phase = "model_request";
    current.current_target = "retrieval";
    current.current_tool_name.clear();
    current.interruptible = true;
    current.stall_reason.clear();
    return current;
  }
  if (event.type == AgentRunnerStreamEventType::MemoryRetrieval) {
    current.phase = "model_request";
    current.current_target = "retrieval";
    current.current_tool_name.clear();
    current.interruptible = true;
    current.stall_reason.clear();
    return current;
  }
  if (event.type == AgentRunnerStreamEventType::Planning) {
    current.phase = "model_request";
    current.current_target = "planning";
    current.current_tool_name.clear();
    current.interruptible = true;
    current.stall_reason.clear();
    return current;
  }
  if (event.type == AgentRunnerStreamEventType::ToolCallArgumentDelta) {
    current.phase = "model_request";
    current.current_target = "model";
    current.current_tool_name = event.tool_call_name;
    current.interruptible = true;
    current.stall_reason.clear();
    return current;
  }
  if (event.type == AgentRunnerStreamEventType::Loop) {
    switch (event.loop_event.type) {
      case AgentLoopStreamEventType::IterationStart:
        current.phase = "starting";
        current.current_target = "run";
        current.current_iteration = event.loop_event.iteration;
        return current;
      case AgentLoopStreamEventType::ModelStart:
      case AgentLoopStreamEventType::ModelTextDelta:
      case AgentLoopStreamEventType::ModelReasoningDelta:
      case AgentLoopStreamEventType::ModelReasoningCompleted:
      case AgentLoopStreamEventType::AgentOutput:
        current.phase = "model_request";
        current.current_target = "model";
        current.current_iteration = event.loop_event.iteration;
        return current;
      case AgentLoopStreamEventType::ToolStart:
        current.phase = "tool_execution";
        current.current_target = "tool";
        current.current_tool_name = event.loop_event.tool_call.name;
        current.current_iteration = event.loop_event.iteration;
        return current;
      case AgentLoopStreamEventType::ToolComplete:
        current.phase = "tool_execution";
        current.current_target = "tool";
        current.current_tool_name = event.loop_event.tool_result.tool_call.name;
        current.current_iteration = event.loop_event.iteration;
        return current;
      case AgentLoopStreamEventType::Done:
        current.phase = "completed";
        current.current_target = "run";
        current.current_tool_name.clear();
        current.current_iteration = event.loop_event.iteration;
        current.interruptible = false;
        return current;
      default:
        return std::nullopt;
    }
  }
  if (event.type == AgentRunnerStreamEventType::Done) {
    current.phase = "completed";
    current.current_target = "run";
    current.current_tool_name.clear();
    current.current_iteration = event.result.iteration_count;
    current.interruptible = false;
    return current;
  }
  if (event.type == AgentRunnerStreamEventType::Cancelled) {
    current.phase = "cancelled";
    current.current_target = "run";
    current.current_tool_name.clear();
    current.interruptible = false;
    current.stall_reason = event.cancellation.at("message").as_string();
    return current;
  }
  if (event.type == AgentRunnerStreamEventType::Error) {
    current.phase = "failed";
    current.current_target = "run";
    current.current_tool_name.clear();
    current.interruptible = false;
    current.stall_reason = event.error.at("message").as_string(event.error.at("error").as_string());
    return current;
  }
  return std::nullopt;
}

AsyncRunActivity derive_activity_from_events(const AsyncAgentRun& run,
                                             const std::vector<AsyncAgentRunEvent>& events,
                                             const std::vector<AsyncAgentRunAttempt>& attempts) {
  AsyncRunActivity activity = lifecycle_activity(activity_phase_for_status(run.status), "run", run.updated_at_ms);
  for (const auto& event : events) {
    if (event.type == "async_run.activity") {
      activity = async_run_activity_from_value(event.payload);
      if (activity.last_activity_at_ms <= 0) {
        activity.last_activity_at_ms = event.created_at_ms;
      }
    }
  }
  for (const auto& attempt : attempts) {
    activity.last_heartbeat_at_ms = std::max(activity.last_heartbeat_at_ms, attempt.heartbeat_at_ms);
  }
  if (activity.last_activity_at_ms <= 0) {
    activity.last_activity_at_ms = run.updated_at_ms;
  }
  if (run.status == AsyncAgentRunStatus::Completed ||
      run.status == AsyncAgentRunStatus::Failed ||
      run.status == AsyncAgentRunStatus::Cancelled) {
    activity.phase = to_string(run.status);
    activity.current_target = "run";
    activity.current_tool_name.clear();
    activity.interruptible = false;
    if (run.status == AsyncAgentRunStatus::Failed) {
      activity.stall_reason = run.error;
    }
  }
  return activity;
}

ChildAgentResult child_agent_result_from_run_output(const AsyncAgentRun& run,
                                                    const Value& output) {
  ChildAgentResult result;
  result.summary = output.at("summary").as_string(output.at("text").as_string());
  result.output_text = output.at("outputText").as_string(output.at("text").as_string());
  result.evidence = output.at("evidence").is_array() ? output.at("evidence") : empty_array_value();
  result.modified_resources = output.at("modifiedResources").is_array()
                                  ? output.at("modifiedResources")
                                  : empty_array_value();
  result.open_questions = string_list_from_value(output.at("openQuestions"));
  result.next_actions = string_list_from_value(output.at("nextActions"));
  result.run_id = run.id;
  result.kind = run.kind;
  result.status = run.status;
  result.root_run_id = run.root_run_id;
  result.parent_run_id = run.parent_run_id;
  result.depth = run.depth;
  result.role = run.role;
  result.parent_child_relation = run.parent_child_relation;
  result.spawned_by_tool_call_id = run.spawned_by_tool_call_id;
  result.text = output.at("text").as_string();
  result.output = output;
  result.error = run.error;
  result.resource_ledger = run.resource_ledger;
  result.metadata = run.metadata;
  return result;
}

Value child_event_payload(const AsyncAgentRun& run,
                          std::string lifecycle,
                          Value detail = Value::object({})) {
  Value payload = Value::object({{"runId", run.id},
                                 {"childRunId", run.id},
                                 {"parentRunId", run.parent_run_id},
                                 {"rootRunId", run.root_run_id},
                                 {"kind", run.kind},
                                 {"depth", run.depth},
                                 {"role", run.role},
                                 {"parentChildRelation", run.parent_child_relation},
                                 {"relation", run.parent_child_relation},
                                 {"spawnedByToolCallId", run.spawned_by_tool_call_id},
                                 {"lifecycle", std::move(lifecycle)}});
  if (detail.is_object()) {
    for (const auto& [key, value] : detail.as_object()) {
      payload[key] = value;
    }
  } else if (!detail.is_null()) {
    payload["detail"] = std::move(detail);
  }
  return payload;
}

Value retrieval_options_to_value(const RunnerRetrievalOptions& options) {
  Value value = Value::object({});
  if (options.enabled) {
    value["enabled"] = *options.enabled;
  }
  if (options.top_k) {
    value["topK"] = static_cast<long long>(*options.top_k);
  }
  if (options.min_score) {
    value["minScore"] = *options.min_score;
  }
  if (!options.namespace_id.empty()) {
    value["namespaceId"] = options.namespace_id;
  }
  return value;
}

RunnerRetrievalOptions retrieval_options_from_value(const Value& value) {
  RunnerRetrievalOptions options;
  if (!value.is_object()) {
    return options;
  }
  if (value.contains("enabled")) {
    options.enabled = value.at("enabled").as_bool();
  }
  if (value.contains("topK")) {
    options.top_k = static_cast<std::size_t>(std::max<long long>(0, value.at("topK").as_integer()));
  } else if (value.contains("top_k")) {
    options.top_k = static_cast<std::size_t>(std::max<long long>(0, value.at("top_k").as_integer()));
  }
  if (value.contains("minScore")) {
    options.min_score = value.at("minScore").as_number();
  } else if (value.contains("min_score")) {
    options.min_score = value.at("min_score").as_number();
  }
  options.namespace_id = value.at("namespaceId").as_string(value.at("namespace_id").as_string());
  return options;
}

Value writeback_options_to_value(const RunnerWritebackOptions& options) {
  Value value = Value::object({{"metadata", options.metadata}});
  if (options.enabled) {
    value["enabled"] = *options.enabled;
  }
  if (!options.namespace_id.empty()) {
    value["namespaceId"] = options.namespace_id;
  }
  return value;
}

RunnerWritebackOptions writeback_options_from_value(const Value& value) {
  RunnerWritebackOptions options;
  if (!value.is_object()) {
    return options;
  }
  if (value.contains("enabled")) {
    options.enabled = value.at("enabled").as_bool();
  }
  options.namespace_id = value.at("namespaceId").as_string(value.at("namespace_id").as_string());
  options.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return options;
}

Value skill_activation_to_value(const SkillActivation& activation) {
  return Value::object({{"name", activation.name},
                        {"argumentsText", activation.arguments_text},
                        {"source", to_string(activation.source)},
                        {"priority", activation.priority}});
}

SkillActivation skill_activation_from_value(const Value& value) {
  SkillActivation activation;
  if (!value.is_object()) {
    return activation;
  }
  activation.name = value.at("name").as_string();
  activation.arguments_text =
      value.at("argumentsText").as_string(value.at("arguments_text").as_string());
  activation.source = skill_activation_source_from_string(value.at("source").as_string("host"));
  activation.priority = static_cast<int>(value.at("priority").as_integer());
  return activation;
}

Value skill_activations_to_value(const std::vector<SkillActivation>& activations) {
  Value::Array output;
  output.reserve(activations.size());
  for (const auto& activation : activations) {
    output.push_back(skill_activation_to_value(activation));
  }
  return Value(std::move(output));
}

std::vector<SkillActivation> skill_activations_from_value(const Value& value) {
  std::vector<SkillActivation> output;
  if (!value.is_array()) {
    return output;
  }
  for (const auto& item : value.as_array()) {
    auto activation = skill_activation_from_value(item);
    if (!activation.name.empty()) {
      output.push_back(std::move(activation));
    }
  }
  return output;
}

Value knowledge_retrieval_options_to_value(
    const std::optional<RunnerKnowledgeRetrievalOptions>& options) {
  if (!options) {
    return Value();
  }
  Value value = Value::object({{"enabled", options->enabled},
                              {"topK", static_cast<long long>(options->top_k)},
                              {"vectorTopK", static_cast<long long>(options->vector_top_k)},
                              {"lexicalTopK", static_cast<long long>(options->lexical_top_k)},
                              {"rerankTopK", static_cast<long long>(options->rerank_top_k)},
                              {"retrievalMode", options->retrieval_mode},
                              {"fusion", options->fusion},
                              {"uriPrefix", options->uri_prefix},
                              {"spaceId", options->space_id},
                              {"tenantId", options->tenant_id},
                              {"documentIds", string_list_to_value(options->document_ids)},
                              {"sourceTypes", string_list_to_value(options->source_types)},
                              {"chunkIds", string_list_to_value(options->chunk_ids)},
                              {"knowledgeBaseIds", string_list_to_value(options->knowledge_base_ids)},
                              {"metadata", options->metadata}});
  if (!std::isnan(options->min_score)) {
    value["minScore"] = options->min_score;
  }
  if (!std::isnan(options->hybrid_alpha)) {
    value["hybridAlpha"] = options->hybrid_alpha;
  }
  if (options->oversample_factor > 0.0) {
    value["oversampleFactor"] = options->oversample_factor;
  }
  return value;
}

std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options_from_value(
    const Value& value) {
  if (!value.is_object()) {
    return std::nullopt;
  }
  RunnerKnowledgeRetrievalOptions options;
  if (value.contains("enabled")) {
    options.enabled = value.at("enabled").as_bool();
  }
  options.top_k = static_cast<std::size_t>(std::max<long long>(0, value.at("topK").as_integer()));
  options.vector_top_k =
      static_cast<std::size_t>(std::max<long long>(0, value.at("vectorTopK").as_integer()));
  options.lexical_top_k =
      static_cast<std::size_t>(std::max<long long>(0, value.at("lexicalTopK").as_integer()));
  if (value.contains("minScore")) {
    options.min_score = value.at("minScore").as_number();
  }
  if (value.contains("hybridAlpha")) {
    options.hybrid_alpha = value.at("hybridAlpha").as_number();
  }
  options.rerank_top_k =
      static_cast<std::size_t>(std::max<long long>(0, value.at("rerankTopK").as_integer()));
  options.retrieval_mode = value.at("retrievalMode").as_string();
  options.oversample_factor = value.at("oversampleFactor").as_number();
  options.fusion = value.at("fusion").as_string();
  options.uri_prefix = value.at("uriPrefix").as_string();
  options.document_ids = string_list_from_value(value.at("documentIds"));
  options.space_id = value.at("spaceId").as_string();
  options.source_types = string_list_from_value(value.at("sourceTypes"));
  options.chunk_ids = string_list_from_value(value.at("chunkIds"));
  options.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  options.tenant_id = value.at("tenantId").as_string();
  options.knowledge_base_ids = string_list_from_value(value.at("knowledgeBaseIds"));
  return options;
}

Value async_task_input_from_start(const AsyncAgentRunStartInput& input,
                                  std::optional<AgentRunnerDurableState> resume_state = std::nullopt) {
  Value value = Value::object({{"schemaVersion", 1},
                              {"input", input.input},
                              {"sessionId", input.session_id},
                              {"kind", input.kind},
                              {"rootRunId", input.root_run_id},
                              {"parentRunId", input.parent_run_id},
                              {"depth", input.depth < 0 ? 0 : input.depth},
                              {"role", input.role.empty() ? "leaf" : input.role},
                              {"parentChildRelation", input.parent_child_relation},
                              {"spawnedByToolCallId", input.spawned_by_tool_call_id},
                              {"childAgentPolicy", child_agent_policy_to_value(input.child_agent_policy)},
                              {"modelSettings", model_settings_to_json_value(input.model_settings)},
                              {"retrievalOptions", retrieval_options_to_value(input.retrieval_options)},
                              {"writebackOptions", writeback_options_to_value(input.writeback_options)},
                              {"skillActivations", skill_activations_to_value(input.skill_activations)},
                              {"context", input.context},
                              {"knowledgeRetrievalOptions",
                               knowledge_retrieval_options_to_value(input.knowledge_retrieval_options)},
                              {"enablePlanning", input.enable_planning}});
  if (resume_state) {
    value["resumeState"] = agent_runner_durable_state_to_value(*resume_state);
  }
  return value;
}

AsyncAgentRunStartInput start_input_from_task(const AgentTask& task) {
  auto input = async_agent_run_start_input_from_value(task.input);
  input.id = task.id;
  input.idempotency_key = task.idempotency_key;
  input.owner_api_key_id = task.owner_api_key_id;
  input.tenant_id = task.tenant_id;
  input.metadata = task.metadata;
  return input;
}

AgentMessage async_input_message_from_value(const Value& input) {
  if (input.is_object() && (input.contains("role") || input.contains("content"))) {
    auto message = input;
    if (!message.contains("role")) {
      message["role"] = "user";
    }
    return agent_message_from_value(message);
  }
  return agent_message_from_value(Value::object({{"role", "user"}, {"content", input}}));
}

Value model_usage_to_value(const ModelUsage& usage) {
  return Value::object({{"inputTokens", usage.input_tokens},
                        {"outputTokens", usage.output_tokens},
                        {"totalTokens", usage.total_tokens},
                        {"inputTokensSource", to_string(usage.input_tokens_source)},
                        {"outputTokensSource", to_string(usage.output_tokens_source)},
                        {"totalTokensSource", to_string(usage.total_tokens_source)},
                        {"quality", to_string(usage.quality)},
                        {"cachedInputTokens", usage.cached_input_tokens},
                        {"cachedInputTokensSource", to_string(usage.cached_input_tokens_source)},
                        {"reasoningTokens", usage.reasoning_tokens},
                        {"reasoningSource", to_string(usage.reasoning_source)},
                        {"provider", usage.provider}});
}

Value model_response_to_async_value(const AgentOutput& response) {
  return agent_output_to_value(response);
}

Value runner_result_to_async_value(const AgentRunnerRunResult& result) {
  Value::Array messages;
  for (const auto& message : result.messages) {
    messages.push_back(agent_message_to_value(message));
  }
  return Value::object({{"sessionId", result.session_id},
                        {"iterationCount", result.iteration_count},
                        {"text", result.text},
                        {"response", model_response_to_async_value(result.response)},
                        {"messages", Value(std::move(messages))},
                        {"terminationReason", to_string(result.termination_reason)},
                        {"usage", model_usage_to_value(result.usage)},
                        {"resourceLedger", resource_ledger_to_value(resource_ledger_from_runner_result(result))},
                        {"latestNonEmptyReasoning", result.latest_non_empty_reasoning},
                        {"knowledgeDebug", result.knowledge_debug}});
}

Value runner_stream_event_to_async_value(const AgentRunnerStreamEvent& event) {
  Value payload = Value::object({{"schemaVersion", event.schema_version},
                                {"sequence", static_cast<long long>(event.sequence)},
                                {"type", to_string(event.type)}});
  if (!event.delta.empty()) {
    payload["delta"] = event.delta;
  }
  if (!event.text.empty()) {
    payload["text"] = event.text;
  }
  switch (event.type) {
    case AgentRunnerStreamEventType::Status:
      payload["status"] = Value::object({{"kind", event.status.kind},
                                         {"stage", event.status.stage},
                                         {"state", event.status.state},
                                         {"message", event.status.message},
                                         {"iteration", event.status.iteration},
                                         {"provider", event.status.provider},
                                         {"model", event.status.model},
                                         {"toolName", event.status.tool_name},
                                         {"toolCallId", event.status.tool_call_id},
                                         {"details", event.status.details}});
      break;
    case AgentRunnerStreamEventType::Loop:
      payload["loop"] = Value::object({{"schemaVersion", event.loop_event.schema_version},
                                       {"sequence", static_cast<long long>(event.loop_event.sequence)},
                                       {"type", to_string(event.loop_event.type)},
                                       {"iteration", event.loop_event.iteration},
                                       {"delta", event.loop_event.delta},
                                       {"text", event.loop_event.text},
                                       {"provider", event.loop_event.provider},
                                       {"model", event.loop_event.model},
                                       {"runId", event.loop_event.run_id}});
      break;
    case AgentRunnerStreamEventType::ToolCallArgumentDelta:
      payload["iteration"] = static_cast<long long>(event.tool_call_iteration);
      payload["provider"] = event.tool_call_provider;
      payload["model"] = event.tool_call_model;
      payload["toolCallId"] = event.tool_call_id;
      payload["toolName"] = event.tool_call_name;
      payload["argsDelta"] = event.tool_call_args_delta;
      payload["argsAccumulated"] = event.tool_call_args_accumulated;
      break;
    case AgentRunnerStreamEventType::Done:
      payload["result"] = runner_result_to_async_value(event.result);
      break;
    case AgentRunnerStreamEventType::Cancelled:
      payload["cancellation"] = event.cancellation;
      break;
    case AgentRunnerStreamEventType::Error:
      payload["error"] = event.error;
      break;
    default:
      break;
  }
  return payload;
}

Value async_stream_event_payload(const AsyncAgentRun& run,
                                 const AsyncAgentRunAttempt& attempt,
                                 const Value& stream_event) {
  Value payload = Value::object({{"runId", run.id},
                                 {"kind", run.kind},
                                 {"rootRunId", run.root_run_id},
                                 {"parentRunId", run.parent_run_id},
                                 {"depth", run.depth},
                                 {"role", run.role},
                                 {"parentChildRelation", run.parent_child_relation},
                                 {"spawnedByToolCallId", run.spawned_by_tool_call_id},
                                 {"attempt", attempt.attempt},
                                 {"eventType", stream_event.at("type").as_string()},
                                 {"streamEvent", stream_event}});
  const auto copy_if_present = [&](const std::string& key) {
    if (stream_event.contains(key)) {
      payload[key] = stream_event.at(key);
    }
  };
  copy_if_present("iteration");
  copy_if_present("provider");
  copy_if_present("model");
  copy_if_present("toolCallId");
  copy_if_present("toolName");
  copy_if_present("argsDelta");
  copy_if_present("argsAccumulated");
  return payload;
}

ToolCall tool_call_from_value(const Value& value) {
  ToolCall tool_call;
  if (!value.is_object()) {
    return tool_call;
  }
  tool_call.id = value.at("id").as_string();
  tool_call.name = value.at("name").as_string();
  tool_call.arguments = value.at("arguments").is_object() ? value.at("arguments") : Value::object({});
  return tool_call;
}

AgentOutput durable_model_response_from_value(const Value& value) {
  return agent_output_from_value(value);
}

ToolExecutionResult durable_tool_execution_result_from_value(const Value& value) {
  ToolExecutionResult result;
  if (!value.is_object()) {
    return result;
  }
  result.tool_call = tool_call_from_value(value.at("toolCall"));
  result.ok = value.at("ok").as_bool();
  result.error = value.at("error").as_string();
  result.output = value.at("output").as_string();
  if (value.at("message").is_object()) {
    result.message = agent_message_from_value(value.at("message"));
  }
  if (!value.at("result").is_null()) {
    result.result = value.at("result");
  } else if (!value.at("structuredOutput").is_null()) {
    result.result = value.at("structuredOutput");
  }
  return result;
}

AgentTraceEntry durable_trace_entry_from_value(const Value& value) {
  AgentTraceEntry entry;
  if (!value.is_object()) {
    return entry;
  }
  entry.type = value.at("type").as_string();
  entry.iteration = static_cast<int>(value.at("iteration").as_integer());
  if (value.at("response").is_object()) {
    entry.response = durable_model_response_from_value(value.at("response"));
  }
  if (value.at("toolResults").is_array()) {
    for (const auto& item : value.at("toolResults").as_array()) {
      entry.tool_results.push_back(durable_tool_execution_result_from_value(item));
    }
  }
  return entry;
}

std::vector<AgentTraceEntry> durable_trace_from_value(const Value& value) {
  std::vector<AgentTraceEntry> output;
  if (!value.is_array()) {
    return output;
  }
  for (const auto& item : value.as_array()) {
    output.push_back(durable_trace_entry_from_value(item));
  }
  return output;
}

ReActStepType react_step_type_from_string(const std::string& value) {
  if (value == "final") return ReActStepType::Final;
  if (value == "final-rejected") return ReActStepType::FinalRejected;
  if (value == "reasoning-protocol-leak") return ReActStepType::ReasoningProtocolLeak;
  if (value == "parse-error") return ReActStepType::ParseError;
  return ReActStepType::ActionBatch;
}

ReActAction react_action_from_value(const Value& value) {
  ReActAction action;
  if (!value.is_object()) {
    return action;
  }
  action.id = value.at("id").as_string();
  action.index = static_cast<int>(value.at("index").as_integer());
  action.tool = value.at("tool").as_string();
  action.input = value.at("input").is_object() ? value.at("input") : Value::object({});
  return action;
}

ReActTraceEntry react_trace_entry_from_value(const Value& value) {
  ReActTraceEntry entry;
  if (!value.is_object()) {
    return entry;
  }
  entry.type = react_step_type_from_string(value.at("type").as_string());
  entry.iteration = static_cast<int>(value.at("iteration").as_integer());
  entry.thought = value.at("thought").as_string();
  entry.visible_message = value.at("visibleMessage").as_string();
  if (value.at("actions").is_array()) {
    for (const auto& item : value.at("actions").as_array()) {
      entry.actions.push_back(react_action_from_value(item));
    }
  }
  entry.observation = value.at("observation").as_string();
  entry.final_answer = value.at("finalAnswer").as_string();
  entry.error = value.at("error").as_string();
  entry.ok = value.at("ok").as_bool(true);
  return entry;
}

std::vector<ReActTraceEntry> react_trace_from_value(const Value& value) {
  std::vector<ReActTraceEntry> output;
  if (!value.is_array()) {
    return output;
  }
  for (const auto& item : value.as_array()) {
    output.push_back(react_trace_entry_from_value(item));
  }
  return output;
}

std::vector<AgentMessage> messages_from_value(const Value& value) {
  std::vector<AgentMessage> output;
  if (!value.is_array()) {
    return output;
  }
  for (const auto& item : value.as_array()) {
    output.push_back(agent_message_from_value(item));
  }
  return output;
}

std::optional<AgentRunnerDurableState> resume_state_from_task_input(const Value& input) {
  if (!input.is_object() || !input.at("resumeState").is_object()) {
    return std::nullopt;
  }
  return agent_runner_durable_state_from_value(input.at("resumeState"));
}

std::optional<AgentRunnerDurableState> latest_runner_state_checkpoint(
    const AsyncAgentRunSnapshot& snapshot) {
  for (auto it = snapshot.checkpoints.rbegin(); it != snapshot.checkpoints.rend(); ++it) {
    if (it->name == "runner.state" && it->state.is_object()) {
      return agent_runner_durable_state_from_value(it->state);
    }
  }
  return std::nullopt;
}

}  // namespace

std::string to_string(AsyncAgentRunStatus status) {
  switch (status) {
    case AsyncAgentRunStatus::Queued:
      return "queued";
    case AsyncAgentRunStatus::Running:
      return "running";
    case AsyncAgentRunStatus::Waiting:
      return "waiting";
    case AsyncAgentRunStatus::Completed:
      return "completed";
    case AsyncAgentRunStatus::Failed:
      return "failed";
    case AsyncAgentRunStatus::Cancelled:
      return "cancelled";
  }
  return "queued";
}

AsyncAgentRunStatus async_agent_run_status_from_string(const std::string& value,
                                                       AsyncAgentRunStatus fallback) {
  if (value == "queued") return AsyncAgentRunStatus::Queued;
  if (value == "running") return AsyncAgentRunStatus::Running;
  if (value == "waiting") return AsyncAgentRunStatus::Waiting;
  if (value == "completed") return AsyncAgentRunStatus::Completed;
  if (value == "failed") return AsyncAgentRunStatus::Failed;
  if (value == "cancelled") return AsyncAgentRunStatus::Cancelled;
  return fallback;
}

TaskBackedAsyncAgentRunStore::TaskBackedAsyncAgentRunStore(TaskBackedAsyncAgentRunStoreConfig config)
    : task_store_(require_async_task_store(config.task_store)),
      transcript_(config.transcript) {}

TaskBackedAsyncAgentRunStore::TaskBackedAsyncAgentRunStore(InMemoryTaskStore& task_store,
                                                           RunTranscript* transcript)
    : task_store_(task_store), transcript_(transcript) {}

AsyncAgentRun TaskBackedAsyncAgentRunStore::create(AsyncAgentRunStartInput input) {
  const std::string id = input.id;
  AgentTask task = task_store_.create_task(CreateTaskInput{
      .id = id,
      .type = kAsyncAgentRunTaskType,
      .input = async_task_input_from_start(input),
      .idempotency_key = input.idempotency_key,
      .owner_api_key_id = input.owner_api_key_id,
      .tenant_id = input.tenant_id,
      .metadata = input.metadata,
  });
  auto run = async_agent_run_from_task(task);
  const auto topology = topology_payload_from_start(input);
  append_event(task.id, {}, "async_run.created",
               Value::object({{"sessionId", input.session_id}, {"topology", topology}}));
  append_event(task.id, {}, "async_run.queued",
               Value::object({{"sessionId", input.session_id}, {"topology", topology}}));
  append_event(task.id, {}, "async_run.activity",
               async_run_activity_to_value(lifecycle_activity("queued", "run", time_point_to_ms(task.updated_at))));
  if (is_child_run(run)) {
    const auto payload = child_event_payload(run, "queued");
    append_event(task.id, {}, "child.run.queued", payload);
    if (!run.parent_run_id.empty() && task_store_.get_task(run.parent_run_id)) {
      append_event(run.parent_run_id, {}, "child.run.queued", payload);
    }
  }
  return run;
}

std::optional<AsyncAgentRunSnapshot> TaskBackedAsyncAgentRunStore::get(
    const std::string& run_id) const {
  auto snapshot = task_store_.get_task(run_id);
  if (!snapshot) {
    return std::nullopt;
  }
  AsyncAgentRunSnapshot output;
  output.run = async_agent_run_from_task(snapshot->task);
  for (const auto& attempt : snapshot->runs) {
    output.attempts.push_back(async_agent_run_attempt_from_task_run(attempt));
  }
  for (const auto& event : snapshot->events) {
    output.events.push_back(async_agent_run_event_from_task_event(event));
  }
  for (const auto& checkpoint : snapshot->checkpoints) {
    output.checkpoints.push_back(async_agent_run_checkpoint_from_task_checkpoint(checkpoint));
  }
  if (transcript_) {
    output.transcript = transcript_->list(RunTranscriptListOptions{.run_id = run_id});
  }
  output.run.activity = derive_activity_from_events(output.run, output.events, output.attempts);
  output.run.resource_ledger.child_run_count =
      std::max(output.run.resource_ledger.child_run_count, direct_child_count(task_store_, run_id));
  if (output.run.resource_ledger.child_run_count > 0 && output.run.role == "leaf") {
    output.run.role = "orchestrator";
  }
  output.run.result = child_agent_result_from_run_output(output.run, output.run.output);
  return output;
}

std::vector<AsyncAgentRun> TaskBackedAsyncAgentRunStore::list(AsyncAgentRunFilter filter) const {
  TaskScopeFilter task_filter;
  task_filter.type = kAsyncAgentRunTaskType;
  if (filter.status) {
    task_filter.status = task_status_from_async_status(*filter.status);
  }
  if (!filter.owner_api_key_id.empty()) {
    task_filter.owner_api_key_id = filter.owner_api_key_id;
  }
  if (!filter.tenant_id.empty()) {
    task_filter.tenant_id = filter.tenant_id;
  }
  std::vector<AsyncAgentRun> output;
  for (const auto& task : task_store_.list_tasks(task_filter)) {
    auto run = async_agent_run_from_task(task);
    if (auto snapshot = task_store_.get_task(task.id)) {
      std::vector<AsyncAgentRunEvent> events;
      events.reserve(snapshot->events.size());
      for (const auto& event : snapshot->events) {
        events.push_back(async_agent_run_event_from_task_event(event));
      }
      std::vector<AsyncAgentRunAttempt> attempts;
      attempts.reserve(snapshot->runs.size());
      for (const auto& attempt : snapshot->runs) {
        attempts.push_back(async_agent_run_attempt_from_task_run(attempt));
      }
      run.activity = derive_activity_from_events(run, events, attempts);
    }
    run.resource_ledger.child_run_count =
        std::max(run.resource_ledger.child_run_count, direct_child_count(task_store_, run.id));
    if (run.resource_ledger.child_run_count > 0 && run.role == "leaf") {
      run.role = "orchestrator";
    }
    run.result = child_agent_result_from_run_output(run, run.output);
    output.push_back(std::move(run));
  }
  return output;
}

AsyncAgentRun TaskBackedAsyncAgentRunStore::update_status(const std::string& run_id,
                                                          AsyncAgentRunStatus status,
                                                          Value output,
                                                          std::string error) {
  return async_agent_run_from_task(task_store_.update_task_status(
      run_id, task_status_from_async_status(status), std::move(output), std::move(error)));
}

AsyncAgentRunEvent TaskBackedAsyncAgentRunStore::append_event(const std::string& run_id,
                                                              const std::string& attempt_id,
                                                              std::string type,
                                                              Value payload) {
  return async_agent_run_event_from_task_event(
      task_store_.append_event(run_id, attempt_id, std::move(type), std::move(payload)));
}

AsyncAgentRunCheckpoint TaskBackedAsyncAgentRunStore::append_checkpoint(
    const std::string& run_id,
    const std::string& attempt_id,
    std::string name,
    Value state) {
  return async_agent_run_checkpoint_from_task_checkpoint(
      task_store_.append_checkpoint(run_id, attempt_id, std::move(name), std::move(state)));
}

AsyncAgentRunEvent AsyncAgentRunWorkerContext::event(std::string type, Value payload) const {
  if (!store) {
    throw ConfigurationError("AsyncAgentRunWorkerContext.event requires a store.");
  }
  return store->append_event(run.id, attempt.id, std::move(type), std::move(payload));
}

AsyncAgentRunCheckpoint AsyncAgentRunWorkerContext::checkpoint(std::string name, Value state) const {
  if (!store) {
    throw ConfigurationError("AsyncAgentRunWorkerContext.checkpoint requires a store.");
  }
  auto checkpoint = store->append_checkpoint(run.id, attempt.id, name, std::move(state));
  store->append_event(run.id, attempt.id, "async_run.checkpoint.created",
                      Value::object({{"checkpointId", checkpoint.id},
                                     {"name", std::move(name)}}));
  return checkpoint;
}

RunTranscriptEntry AsyncAgentRunWorkerContext::transcript_entry(std::string kind,
                                                                Value payload,
                                                                Value metadata) const {
  if (!transcript) {
    throw ConfigurationError("AsyncAgentRunWorkerContext.transcript_entry requires a transcript.");
  }
  if (!metadata.is_object()) {
    metadata = Value::object({});
  }
  metadata["sessionId"] = run.session_id;
  metadata["attemptId"] = attempt.id;
  auto entry = transcript->append(RunTranscriptAppendInput{
      .run_id = run.id,
      .kind = std::move(kind),
      .payload = std::move(payload),
      .metadata = std::move(metadata),
  });
  if (store) {
    store->append_event(run.id, attempt.id, "async_run.transcript.appended",
                        Value::object({{"entryId", entry.id},
                                       {"sequence", static_cast<long long>(entry.sequence)},
                                       {"kind", entry.kind}}));
  }
  return entry;
}

AsyncAgentRunWorker::AsyncAgentRunWorker(AsyncAgentRunWorkerConfig config)
    : task_store_(require_async_task_store(config.task_store)),
      queue_(require_async_queue(config.queue)),
      store_(require_async_store(config.store)),
      transcript_(config.transcript),
      runner_(config.runner),
      resolve_runner_(std::move(config.resolve_runner)),
      on_stream_event_(std::move(config.on_stream_event)),
      lease_ms_(config.lease_ms) {
  if (!runner_ && !resolve_runner_) {
    throw ConfigurationError("AsyncAgentRunWorker requires a runner or runner resolver.");
  }
}

std::shared_ptr<AgentRunner> AsyncAgentRunWorker::resolve_runner(const AsyncAgentRun& run,
                                                                 const Value& input) const {
  if (resolve_runner_) {
    auto resolved = resolve_runner_(run, input);
    if (!resolved) {
      throw ConfigurationError("AsyncAgentRun runner resolver returned null.");
    }
    return resolved;
  }
  return std::shared_ptr<AgentRunner>(runner_, [](AgentRunner*) {});
}

bool AsyncAgentRunWorker::is_cancelled(const std::string& run_id) const {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  return cancelled_run_reasons_.contains(run_id);
}

void AsyncAgentRunWorker::mark_cancelled(const std::string& run_id, const std::string& reason) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  cancelled_run_reasons_[run_id] = reason;
  if (auto found = active_cancellations_.find(run_id); found != active_cancellations_.end() && found->second) {
    found->second->cancel(reason);
  }
}

void AsyncAgentRunWorker::clear_cancelled(const std::string& run_id) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  cancelled_run_reasons_.erase(run_id);
}

void AsyncAgentRunWorker::register_active_token(const std::string& run_id, CancellationToken& token) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  active_cancellations_[run_id] = &token;
  if (auto found = cancelled_run_reasons_.find(run_id); found != cancelled_run_reasons_.end()) {
    token.cancel(found->second);
  }
}

void AsyncAgentRunWorker::unregister_active_token(const std::string& run_id, CancellationToken& token) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  if (auto found = active_cancellations_.find(run_id); found != active_cancellations_.end() &&
      found->second == &token) {
    active_cancellations_.erase(found);
  }
}

bool AsyncAgentRunWorker::run_once() {
  auto claim = queue_.claim(lease_ms_);
  if (!claim) {
    return false;
  }
  if (claim->task.type != kAsyncAgentRunTaskType) {
    task_store_.update_task_status(claim->task.id, TaskStatus::Queued);
    return true;
  }

  CancellationToken cancellation;
  register_active_token(claim->task.id, cancellation);
  const auto run = async_agent_run_from_task(claim->task);
  const auto attempt = async_agent_run_attempt_from_task_run(claim->run);
  AsyncAgentRunWorkerContext context{run, attempt, &store_, transcript_, &cancellation};
  AsyncRunActivity current_activity = lifecycle_activity("starting", "run", now_ms());
  current_activity.last_heartbeat_at_ms = attempt.heartbeat_at_ms;
  ResourceLedger partial_ledger;
  const auto emit_activity = [&](AsyncRunActivity activity) {
    current_activity = std::move(activity);
    context.event("async_run.activity", async_run_activity_to_value(current_activity));
  };
  const auto emit_child_lifecycle = [&](const std::string& type, const std::string& lifecycle, Value detail) {
    if (!is_child_run(run)) {
      return;
    }
    const auto payload = child_event_payload(run, lifecycle, std::move(detail));
    context.event(type, payload);
    if (!run.parent_run_id.empty() && store_.get(run.parent_run_id)) {
      store_.append_event(run.parent_run_id, {}, type, payload);
    }
  };
  try {
    auto start = start_input_from_task(claim->task);
    auto runner = resolve_runner(run, claim->task.input);
    context.event("async_run.started",
                  Value::object({{"attempt", claim->run.attempt},
                                 {"topology", topology_payload_from_run(run)}}));
    emit_activity(current_activity);
    emit_child_lifecycle("child.run.started", "started",
                         Value::object({{"attempt", claim->run.attempt}}));
    if (transcript_) {
      context.transcript_entry("run-started", Value::object({{"input", start.input}}));
    }

    AgentRunnerDurableOptions durable_options;
    durable_options.resume_state = resume_state_from_task_input(claim->task.input);
    if (!durable_options.resume_state) {
      if (auto current = store_.get(claim->task.id)) {
        durable_options.resume_state = latest_runner_state_checkpoint(*current);
      }
    }
    durable_options.on_checkpoint = [&](const AgentRunnerDurableState& state) {
      context.checkpoint("runner.state", agent_runner_durable_state_to_value(state));
      if (transcript_) {
        context.transcript_entry("checkpoint", agent_runner_durable_state_to_value(state),
                                 Value::object({{"checkpoint", "runner.state"}}));
      }
    };

    auto message = async_input_message_from_value(start.input);
    auto stream = runner->streaming().stream(message,
                                 [&](const AgentRunnerStreamEvent& event) {
                                   const auto payload = runner_stream_event_to_async_value(event);
                                   context.event("async_run.stream_event",
                                                 async_stream_event_payload(run, attempt, payload));
                                   if (on_stream_event_) {
                                     on_stream_event_(context, event, payload);
                                   }
                                   merge_tool_event_into_ledger(partial_ledger, event);
                                   if (auto activity = activity_from_runner_stream_event(event, current_activity)) {
                                     auto heartbeat = queue_.heartbeat(claim->task.id, claim->run.id, lease_ms_);
                                     activity->last_activity_at_ms = now_ms();
                                     activity->last_heartbeat_at_ms = time_point_to_ms(heartbeat.heartbeat_at);
                                     emit_activity(std::move(*activity));
                                   }
                                   if (transcript_) {
                                     context.transcript_entry(
                                         "stream-event",
                                         payload,
                                         Value::object({{"eventType", payload.at("type")}}));
                                   }
                                 },
                                 start.session_id,
                                 start.model_settings,
                                 start.retrieval_options,
                                 start.writeback_options,
                                 start.skill_activations,
                                 start.context,
                                 start.knowledge_retrieval_options,
                                 &cancellation,
                                 start.enable_planning);
    auto output = runner_result_to_async_value(stream.result);
    auto ledger = resource_ledger_from_runner_result(stream.result);
    ledger.child_run_count = direct_child_count(task_store_, run.id);
    output["resourceLedger"] = resource_ledger_to_value(ledger);
    auto completed_run = run;
    completed_run.status = AsyncAgentRunStatus::Completed;
    completed_run.output = output;
    completed_run.resource_ledger = ledger;
    completed_run.activity = lifecycle_activity("completed", "run", now_ms());
    completed_run.activity.last_heartbeat_at_ms = current_activity.last_heartbeat_at_ms;
    const auto result = child_agent_result_from_run_output(completed_run, output);
    output["result"] = child_agent_result_to_value(result);
    context.checkpoint("result", child_agent_result_to_value(result));
    context.checkpoint("output", output);
    if (transcript_) {
      context.transcript_entry("run-completed", output);
    }
    queue_.complete(claim->task.id, claim->run.id, output);
    emit_activity(completed_run.activity);
    context.event("async_run.completed",
                  Value::object({{"output", output},
                                 {"result", child_agent_result_to_value(result)},
                                 {"resourceLedger", resource_ledger_to_value(ledger)}}));
    emit_child_lifecycle("child.run.ledger.updated", "ledger_updated",
                         Value::object({{"resourceLedger", resource_ledger_to_value(ledger)}}));
    emit_child_lifecycle("child.run.completed", "completed",
                         Value::object({{"result", child_agent_result_to_value(result)},
                                        {"resourceLedger", resource_ledger_to_value(ledger)}}));
    unregister_active_token(claim->task.id, cancellation);
    clear_cancelled(claim->task.id);
    return true;
  } catch (const CancellationError& error) {
    queue_.cancel(claim->task.id);
    const auto payload = Value::object({{"message", std::string(error.what())},
                                       {"name", "CancellationError"},
                                       {"category", "cancelled"},
                                       {"code", "cancelled"}});
    auto cancelled_activity = lifecycle_activity("cancelled", "run", now_ms());
    cancelled_activity.stall_reason = error.what();
    cancelled_activity.last_heartbeat_at_ms = current_activity.last_heartbeat_at_ms;
    emit_activity(cancelled_activity);
    context.event("async_run.cancelled",
                  Value::object({{"cancellation", payload},
                                 {"resourceLedger", resource_ledger_to_value(partial_ledger)}}));
    emit_child_lifecycle("child.run.cancelled", "cancelled", payload);
    if (transcript_) {
      context.transcript_entry("run-cancelled", payload);
    }
  } catch (const std::exception& error) {
    if (is_cancelled(claim->task.id) || cancellation.cancelled()) {
      queue_.cancel(claim->task.id);
      const auto payload = Value::object({{"message", std::string(error.what())},
                                         {"name", "CancellationError"},
                                         {"category", "cancelled"},
                                         {"code", "cancelled"}});
      auto cancelled_activity = lifecycle_activity("cancelled", "run", now_ms());
      cancelled_activity.stall_reason = error.what();
      cancelled_activity.last_heartbeat_at_ms = current_activity.last_heartbeat_at_ms;
      emit_activity(cancelled_activity);
      context.event("async_run.cancelled",
                    Value::object({{"cancellation", payload},
                                   {"resourceLedger", resource_ledger_to_value(partial_ledger)}}));
      emit_child_lifecycle("child.run.cancelled", "cancelled", payload);
      if (transcript_) {
        context.transcript_entry("run-cancelled", payload);
      }
    } else {
      queue_.fail(claim->task.id, claim->run.id, error.what());
      auto failed_activity = lifecycle_activity("failed", "run", now_ms());
      failed_activity.stall_reason = error.what();
      failed_activity.last_heartbeat_at_ms = current_activity.last_heartbeat_at_ms;
      emit_activity(failed_activity);
      auto payload = Value::object({{"error", error.what()},
                                   {"resourceLedger", resource_ledger_to_value(partial_ledger)}});
      context.event("async_run.failed", payload);
      emit_child_lifecycle("child.run.failed", "failed", payload);
      if (transcript_) {
        context.transcript_entry("run-failed", Value::object({{"error", error.what()}}));
      }
    }
  } catch (...) {
    unregister_active_token(claim->task.id, cancellation);
    throw;
  }
  unregister_active_token(claim->task.id, cancellation);
  clear_cancelled(claim->task.id);
  return true;
}

bool AsyncAgentRunWorker::cancel(const std::string& run_id, std::string reason) {
  if (!task_store_.get_task(run_id)) {
    return false;
  }
  mark_cancelled(run_id, reason);
  store_.append_event(run_id, {}, "async_run.cancel.requested",
                      Value::object({{"reason", reason}, {"source", "worker.cancel"}}));
  queue_.cancel(run_id);
  store_.append_event(run_id, {}, "async_run.activity",
                      async_run_activity_to_value(lifecycle_activity("cancelled", "run", now_ms())));
  store_.append_event(run_id, {}, "async_run.cancelled",
                      Value::object({{"reason", reason}, {"source", "worker.cancel"}}));
  return true;
}

AsyncAgentRunController::AsyncAgentRunController(AsyncAgentRunControllerConfig config)
    : store_(require_async_store(config.store)),
      queue_(config.queue),
      worker_(config.worker),
      transcript_(config.transcript),
      child_agent_policy_(std::move(config.child_agent_policy)) {}

AsyncAgentRunSnapshot AsyncAgentRunController::start(AsyncAgentRunStartInput input) {
  normalize_topology(input);
  enforce_child_agent_policy(input);
  auto run = store_.create(input);
  if (queue_) {
    queue_->enqueue(run.id);
  }
  return require_snapshot(run.id);
}

std::optional<AsyncAgentRunSnapshot> AsyncAgentRunController::get(const std::string& run_id) const {
  return store_.get(run_id);
}

std::vector<AsyncAgentRun> AsyncAgentRunController::list(AsyncAgentRunFilter filter) const {
  return store_.list(std::move(filter));
}

std::vector<AsyncAgentRunEvent> AsyncAgentRunController::events(const std::string& run_id) const {
  return require_snapshot(run_id).events;
}

std::vector<AsyncAgentRunCheckpoint> AsyncAgentRunController::checkpoints(const std::string& run_id) const {
  return require_snapshot(run_id).checkpoints;
}

std::vector<RunTranscriptEntry> AsyncAgentRunController::transcript(const std::string& run_id) const {
  if (transcript_) {
    return transcript_->list(RunTranscriptListOptions{.run_id = run_id});
  }
  return require_snapshot(run_id).transcript;
}

AsyncAgentRunSnapshot AsyncAgentRunController::resume(const std::string& run_id,
                                                      AsyncAgentRunResumeInput input) {
  auto snapshot = require_snapshot(run_id);
  if (snapshot.run.status != AsyncAgentRunStatus::Waiting &&
      snapshot.run.status != AsyncAgentRunStatus::Failed) {
    throw AgentFrameworkError("Async agent run is not resumable: " + run_id);
  }
  auto start = async_agent_run_start_input_from_value(snapshot.run.input);
  const auto resume_state = input.resume_state ? input.resume_state : latest_runner_state(snapshot);
  (void)start;
  if (input.resume_state) {
    store_.append_checkpoint(run_id, {}, "runner.state",
                             agent_runner_durable_state_to_value(*input.resume_state));
  }
  auto run = store_.update_status(run_id, AsyncAgentRunStatus::Queued);
  auto* task_store = dynamic_cast<TaskBackedAsyncAgentRunStore*>(&store_);
  (void)task_store;
  store_.append_event(run_id, {}, "async_run.resume.requested",
                      Value::object({{"reason", input.reason},
                                     {"hasRunnerState", resume_state.has_value()},
                                     {"metadata", input.metadata}}));
  if (queue_) {
    queue_->enqueue(run_id);
  }
  store_.append_event(run_id, {}, "async_run.activity",
                      async_run_activity_to_value(lifecycle_activity("queued", "run", now_ms())));
  store_.append_event(run_id, {}, "async_run.resumed",
                      Value::object({{"reason", input.reason},
                                     {"hasRunnerState", resume_state.has_value()},
                                     {"metadata", input.metadata}}));
  return require_snapshot(run_id);
}

AsyncAgentRunSnapshot AsyncAgentRunController::cancel(const std::string& run_id,
                                                      AsyncAgentRunCancelInput input) {
  auto snapshot = require_snapshot(run_id);
  if (snapshot.run.status == AsyncAgentRunStatus::Completed ||
      snapshot.run.status == AsyncAgentRunStatus::Cancelled) {
    throw AgentFrameworkError("Async agent run is not cancellable: " + run_id);
  }
  if (worker_ && worker_->cancel(run_id, input.reason)) {
    return require_snapshot(run_id);
  }
  auto run = store_.update_status(run_id, AsyncAgentRunStatus::Cancelled);
  (void)run;
  store_.append_event(run_id, {}, "async_run.cancel.requested",
                      Value::object({{"reason", input.reason}, {"metadata", input.metadata}}));
  store_.append_event(run_id, {}, "async_run.activity",
                      async_run_activity_to_value(lifecycle_activity("cancelled", "run", now_ms())));
  store_.append_event(run_id, {}, "async_run.cancelled",
                      Value::object({{"reason", input.reason}, {"metadata", input.metadata}}));
  return require_snapshot(run_id);
}

bool AsyncAgentRunController::run_once() {
  if (!worker_) {
    throw ConfigurationError("AsyncAgentRunController.run_once requires a worker.");
  }
  return worker_->run_once();
}

AsyncAgentRunSnapshot AsyncAgentRunController::require_snapshot(const std::string& run_id) const {
  auto snapshot = store_.get(run_id);
  if (!snapshot) {
    throw AgentFrameworkError("Async agent run not found: " + run_id);
  }
  return *snapshot;
}

std::optional<AgentRunnerDurableState> AsyncAgentRunController::latest_runner_state(
    const AsyncAgentRunSnapshot& snapshot) const {
  return latest_runner_state_checkpoint(snapshot);
}

void AsyncAgentRunController::normalize_topology(AsyncAgentRunStartInput& input) const {
  if (input.id.empty()) {
    input.id = generate_uuid();
  }
  if (input.kind.empty()) {
    input.kind = kAsyncAgentRunKindAgent;
  }
  const bool child = is_child_start_input(input);
  if (!child) {
    input.root_run_id = input.id;
    input.depth = 0;
    input.role = normalized_role(std::move(input.role), "orchestrator");
    return;
  }
  if (input.parent_run_id.empty()) {
    throw ConfigurationError("Child agent runs require parentRunId.");
  }
  if (input.parent_run_id == input.id) {
    throw ConfigurationError("Child agent run parentRunId must not equal run id.");
  }
  auto parent = store_.get(input.parent_run_id);
  if (!parent) {
    throw AgentFrameworkError("Child agent parent run not found: " + input.parent_run_id);
  }
  if (parent->run.status == AsyncAgentRunStatus::Completed ||
      parent->run.status == AsyncAgentRunStatus::Cancelled) {
    throw AgentFrameworkError("Child agent parent run is closed: " + input.parent_run_id);
  }
  if (parent->run.role == "leaf") {
    throw AgentFrameworkError("ChildAgentPolicy violation: leaf parent runs cannot spawn child runs.");
  }
  if (input.kind == kAsyncAgentRunKindAgent) {
    input.kind = kAsyncAgentRunKindSubagent;
  }
  input.root_run_id = parent->run.root_run_id.empty() ? parent->run.id : parent->run.root_run_id;
  input.depth = parent->run.depth + 1;
  input.role = normalized_role(std::move(input.role), "leaf");
  if (input.parent_child_relation.empty()) {
    input.parent_child_relation = "delegated";
  }
}

void AsyncAgentRunController::enforce_child_agent_policy(const AsyncAgentRunStartInput& input) const {
  const auto policy = merge_child_agent_policy(child_agent_policy_, input.child_agent_policy);
  const bool child = is_child_start_input(input);
  if (!child) {
    enforce_limit(policy.max_spawn_depth, input.depth, "maxSpawnDepth", "root run " + input.id);
    return;
  }
  if (policy.allow_child_spawn && !*policy.allow_child_spawn) {
    throw AgentFrameworkError("ChildAgentPolicy violation: allowChildSpawn is false for parent " +
                              input.parent_run_id + ".");
  }
  enforce_limit(policy.max_spawn_depth, input.depth, "maxSpawnDepth", "run " + input.id);
  const auto runs = store_.list();
  const auto existing_active_global_children = static_cast<long long>(
      std::count_if(runs.begin(), runs.end(), [](const AsyncAgentRun& run) {
        return is_child_run(run) && is_active_child_status(run.status);
      }));
  enforce_limit(policy.max_global_child_runs, existing_active_global_children + 1,
                "maxGlobalChildRuns", "runtime active child runs");
  long long existing_parent_children = 0;
  long long existing_active_parent_children = 0;
  for (const auto& run : runs) {
    if (run.parent_run_id != input.parent_run_id) {
      continue;
    }
    ++existing_parent_children;
    if (is_active_child_status(run.status)) {
      ++existing_active_parent_children;
    }
  }
  enforce_limit(policy.max_child_runs_per_parent, existing_parent_children + 1,
                "maxChildRunsPerParent", "parent " + input.parent_run_id);
  enforce_limit(policy.max_children_per_run, existing_active_parent_children + 1,
                "maxChildrenPerRun", "parent " + input.parent_run_id);
}

Value async_run_activity_to_value(const AsyncRunActivity& activity) {
  return Value::object({{"phase", activity.phase},
                        {"lastHeartbeatAt", iso8601_from_epoch_ms(activity.last_heartbeat_at_ms)},
                        {"lastActivityAt", iso8601_from_epoch_ms(activity.last_activity_at_ms)},
                        {"currentTarget", activity.current_target},
                        {"currentToolName", activity.current_tool_name},
                        {"currentIteration", std::max(0, activity.current_iteration)},
                        {"interruptible", activity.interruptible},
                        {"stallReason", activity.stall_reason},
                        {"lastHeartbeatAtMs", activity.last_heartbeat_at_ms},
                        {"lastActivityAtMs", activity.last_activity_at_ms}});
}

AsyncRunActivity async_run_activity_from_value(const Value& value) {
  AsyncRunActivity activity;
  if (!value.is_object()) {
    return activity;
  }
  activity.phase = value.at("phase").as_string(activity.phase);
  activity.current_target = value.at("currentTarget").as_string();
  activity.current_tool_name = value.at("currentToolName").as_string();
  activity.current_iteration = static_cast<int>(value.at("currentIteration").as_integer(-1));
  activity.interruptible = value.contains("interruptible") ? value.at("interruptible").as_bool() : true;
  activity.stall_reason = value.at("stallReason").as_string();
  activity.last_heartbeat_at_ms = value.at("lastHeartbeatAtMs").as_integer();
  activity.last_activity_at_ms = value.at("lastActivityAtMs").as_integer();
  return activity;
}

Value resource_ledger_to_value(const ResourceLedger& ledger) {
  const auto files_read = ledger.details.at("filesRead").is_array()
                              ? ledger.details.at("filesRead")
                              : empty_array_value();
  const auto files_written = ledger.details.at("filesWritten").is_array()
                                 ? ledger.details.at("filesWritten")
                                 : empty_array_value();
  const auto artifacts = ledger.details.at("artifacts").is_array()
                             ? ledger.details.at("artifacts")
                             : empty_array_value();
  const auto network_requests = ledger.details.at("networkRequests").is_array()
                                    ? ledger.details.at("networkRequests")
                                    : empty_array_value();
  const auto approval_requests = ledger.details.at("approvalRequests").is_array()
                                     ? ledger.details.at("approvalRequests")
                                     : empty_array_value();
  const auto child_runs = ledger.details.at("childRuns").is_array()
                              ? ledger.details.at("childRuns")
                              : empty_array_value();
  return Value::object({{"tokenUsage", Value::object({
                            {"inputTokens", ledger.input_tokens},
                            {"outputTokens", ledger.output_tokens},
                            {"totalTokens", ledger.total_tokens},
                            {"inputTokensSource", ledger.input_tokens_source},
                            {"outputTokensSource", ledger.output_tokens_source},
                            {"totalTokensSource", ledger.total_tokens_source},
                            {"quality", ledger.token_usage_quality},
                            {"reasoningTokens", ledger.reasoning_tokens},
                            {"cachedInputTokens", ledger.cached_input_tokens},
                            {"cachedInputTokensSource", ledger.cached_input_tokens_source},
                            {"reasoningSource", ledger.reasoning_source},
                            {"provider", ledger.provider},
                        })},
                        {"toolCalls", ledger.tool_call_count},
                        {"filesRead", files_read},
                        {"filesWritten", files_written},
                        {"artifacts", artifacts},
                        {"networkRequests", network_requests},
                        {"approvalRequests", approval_requests},
                        {"childRuns", child_runs},
                        {"iterationCount", ledger.iteration_count},
                        {"toolSuccessCount", ledger.tool_success_count},
                        {"toolErrorCount", ledger.tool_error_count},
                        {"childRunCount", ledger.child_run_count},
                        {"details", ledger.details}});
}

ResourceLedger resource_ledger_from_value(const Value& value) {
  ResourceLedger ledger;
  if (!value.is_object()) {
    return ledger;
  }
  const auto& token_usage = value.at("tokenUsage").is_object() ? value.at("tokenUsage") : value;
  ledger.input_tokens = token_usage.at("inputTokens").as_integer();
  ledger.output_tokens = token_usage.at("outputTokens").as_integer();
  ledger.total_tokens = token_usage.at("totalTokens").as_integer();
  ledger.input_tokens_source = token_usage.at("inputTokensSource").as_string();
  ledger.output_tokens_source = token_usage.at("outputTokensSource").as_string();
  ledger.total_tokens_source = token_usage.at("totalTokensSource").as_string();
  ledger.token_usage_quality = token_usage.at("quality").as_string();
  ledger.cached_input_tokens = token_usage.at("cachedInputTokens").as_integer();
  ledger.cached_input_tokens_source = token_usage.at("cachedInputTokensSource").as_string();
  ledger.reasoning_tokens = token_usage.at("reasoningTokens").as_integer();
  ledger.reasoning_source = token_usage.at("reasoningSource").as_string();
  ledger.provider = token_usage.at("provider").as_string();
  ledger.iteration_count = value.at("iterationCount").as_integer();
  ledger.tool_call_count = value.at("toolCalls").as_integer(value.at("toolCallCount").as_integer());
  ledger.tool_success_count = value.at("toolSuccessCount").as_integer();
  ledger.tool_error_count = value.at("toolErrorCount").as_integer();
  ledger.child_run_count = value.at("childRunCount").as_integer(
      value.at("childRuns").is_array() ? static_cast<long long>(value.at("childRuns").as_array().size()) : 0);
  ledger.details = value.at("details").is_object() ? value.at("details") : Value::object({});
  if (value.at("filesRead").is_array()) ledger.details["filesRead"] = value.at("filesRead");
  if (value.at("filesWritten").is_array()) ledger.details["filesWritten"] = value.at("filesWritten");
  if (value.at("artifacts").is_array()) ledger.details["artifacts"] = value.at("artifacts");
  if (value.at("networkRequests").is_array()) ledger.details["networkRequests"] = value.at("networkRequests");
  if (value.at("approvalRequests").is_array()) ledger.details["approvalRequests"] = value.at("approvalRequests");
  if (value.at("childRuns").is_array()) ledger.details["childRuns"] = value.at("childRuns");
  return ledger;
}

Value child_agent_result_to_value(const ChildAgentResult& result) {
  return Value::object({{"schemaVersion", result.schema_version},
                        {"summary", result.summary},
                        {"outputText", result.output_text},
                        {"evidence", result.evidence},
                        {"resourceLedger", resource_ledger_to_value(result.resource_ledger)},
                        {"modifiedResources", result.modified_resources},
                        {"openQuestions", string_list_to_value(result.open_questions)},
                        {"nextActions", string_list_to_value(result.next_actions)},
                        {"runId", result.run_id},
                        {"kind", result.kind},
                        {"status", to_string(result.status)},
                        {"rootRunId", result.root_run_id},
                        {"parentRunId", result.parent_run_id},
                        {"depth", result.depth},
                        {"role", result.role},
                        {"parentChildRelation", result.parent_child_relation},
                        {"spawnedByToolCallId", result.spawned_by_tool_call_id},
                        {"text", result.text},
                        {"output", result.output},
                        {"error", result.error},
                        {"metadata", result.metadata}});
}

ChildAgentResult child_agent_result_from_value(const Value& value) {
  ChildAgentResult result;
  if (!value.is_object()) {
    return result;
  }
  result.schema_version = static_cast<int>(value.at("schemaVersion").as_integer(1));
  result.summary = value.at("summary").as_string(value.at("text").as_string());
  result.output_text = value.at("outputText").as_string(value.at("text").as_string());
  result.evidence = value.at("evidence").is_array() ? value.at("evidence") : empty_array_value();
  result.modified_resources = value.at("modifiedResources").is_array()
                                  ? value.at("modifiedResources")
                                  : empty_array_value();
  result.open_questions = string_list_from_value(value.at("openQuestions"));
  result.next_actions = string_list_from_value(value.at("nextActions"));
  result.run_id = value.at("runId").as_string();
  result.kind = value.at("kind").as_string(kAsyncAgentRunKindAgent);
  result.status = async_agent_run_status_from_string(value.at("status").as_string());
  result.root_run_id = value.at("rootRunId").as_string();
  result.parent_run_id = value.at("parentRunId").as_string();
  result.depth = static_cast<int>(value.at("depth").as_integer());
  result.role = value.at("role").as_string("leaf");
  result.parent_child_relation = value.at("parentChildRelation").as_string();
  result.spawned_by_tool_call_id = value.at("spawnedByToolCallId").as_string();
  result.text = value.at("text").as_string();
  result.output = value.at("output");
  result.error = value.at("error").as_string();
  result.resource_ledger = resource_ledger_from_value(value.at("resourceLedger"));
  result.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return result;
}

Value child_agent_policy_to_value(const ChildAgentPolicy& policy) {
  Value value = Value::object({});
  if (policy.max_global_child_runs) value["maxGlobalChildRuns"] = *policy.max_global_child_runs;
  if (policy.max_child_runs_per_parent) value["maxChildRunsPerParent"] = *policy.max_child_runs_per_parent;
  if (policy.max_spawn_depth) value["maxSpawnDepth"] = *policy.max_spawn_depth;
  if (policy.max_children_per_run) value["maxChildrenPerRun"] = *policy.max_children_per_run;
  if (policy.allow_child_spawn) value["allowChildSpawn"] = *policy.allow_child_spawn;
  if (policy.cancel_propagation) value["cancelPropagation"] = *policy.cancel_propagation;
  return value;
}

ChildAgentPolicy child_agent_policy_from_value(const Value& value) {
  ChildAgentPolicy policy;
  if (!value.is_object()) {
    return policy;
  }
  if (value.at("maxGlobalChildRuns").is_number()) {
    policy.max_global_child_runs = value.at("maxGlobalChildRuns").as_integer();
  }
  if (value.at("maxChildRunsPerParent").is_number()) {
    policy.max_child_runs_per_parent = value.at("maxChildRunsPerParent").as_integer();
  }
  if (value.at("maxSpawnDepth").is_number()) {
    policy.max_spawn_depth = value.at("maxSpawnDepth").as_integer();
  }
  if (value.at("maxChildrenPerRun").is_number()) {
    policy.max_children_per_run = value.at("maxChildrenPerRun").as_integer();
  }
  if (value.at("allowChildSpawn").is_bool()) {
    policy.allow_child_spawn = value.at("allowChildSpawn").as_bool();
  }
  if (value.at("cancelPropagation").is_string()) {
    policy.cancel_propagation = normalized_cancel_propagation(value.at("cancelPropagation").as_string());
  }
  validate_policy_limit(policy.max_global_child_runs, "maxGlobalChildRuns");
  validate_policy_limit(policy.max_child_runs_per_parent, "maxChildRunsPerParent");
  validate_policy_limit(policy.max_spawn_depth, "maxSpawnDepth");
  validate_policy_limit(policy.max_children_per_run, "maxChildrenPerRun");
  return policy;
}

AsyncAgentRunStartInput async_agent_run_start_input_from_value(const Value& value) {
  if (!value.is_object()) {
    throw ConfigurationError("Async agent run start input must be a JSON object.");
  }
  AsyncAgentRunStartInput input;
  input.id = value.at("id").as_string();
  input.session_id = value.at("sessionId").as_string(value.at("session_id").as_string("default"));
  if (!value.contains("input")) {
    throw ConfigurationError("Async agent run start input requires an input field.");
  }
  input.input = value.at("input");
  input.model_settings = model_settings_from_json_value(value.at("modelSettings"));
  input.retrieval_options = retrieval_options_from_value(value.at("retrievalOptions"));
  input.writeback_options = writeback_options_from_value(value.at("writebackOptions"));
  input.skill_activations = skill_activations_from_value(value.at("skillActivations"));
  input.context = value.at("context").is_object() ? value.at("context") : Value::object({});
  input.knowledge_retrieval_options =
      knowledge_retrieval_options_from_value(value.at("knowledgeRetrievalOptions"));
  input.enable_planning = value.contains("enablePlanning") ? value.at("enablePlanning").as_bool() : true;
  input.kind = value.at("kind").as_string(kAsyncAgentRunKindAgent);
  input.root_run_id = value.at("rootRunId").as_string();
  input.parent_run_id = value.at("parentRunId").as_string();
  input.depth = value.contains("depth") ? static_cast<int>(value.at("depth").as_integer()) : -1;
  input.role = value.at("role").as_string();
  input.parent_child_relation = value.at("parentChildRelation").as_string();
  input.spawned_by_tool_call_id = value.at("spawnedByToolCallId").as_string();
  input.child_agent_policy = child_agent_policy_from_value(value.at("childAgentPolicy"));
  input.idempotency_key = value.at("idempotencyKey").as_string();
  input.owner_api_key_id = value.at("ownerApiKeyId").as_string();
  input.tenant_id = value.at("tenantId").as_string();
  input.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return input;
}

Value async_agent_run_start_input_to_value(const AsyncAgentRunStartInput& input) {
  auto value = async_task_input_from_start(input);
  if (!input.id.empty()) value["id"] = input.id;
  if (!input.idempotency_key.empty()) value["idempotencyKey"] = input.idempotency_key;
  if (!input.owner_api_key_id.empty()) value["ownerApiKeyId"] = input.owner_api_key_id;
  if (!input.tenant_id.empty()) value["tenantId"] = input.tenant_id;
  value["metadata"] = input.metadata;
  value["childAgentPolicy"] = child_agent_policy_to_value(input.child_agent_policy);
  return value;
}

AsyncAgentRunFilter async_agent_run_filter_from_value(const Value& value) {
  AsyncAgentRunFilter filter;
  if (!value.is_object()) {
    return filter;
  }
  if (value.contains("status")) {
    filter.status = async_agent_run_status_from_string(value.at("status").as_string());
  }
  filter.owner_api_key_id = value.at("ownerApiKeyId").as_string();
  filter.tenant_id = value.at("tenantId").as_string();
  return filter;
}

AsyncAgentRunResumeInput async_agent_run_resume_input_from_value(const Value& value) {
  AsyncAgentRunResumeInput input;
  if (!value.is_object()) {
    return input;
  }
  input.reason = value.at("reason").as_string(input.reason);
  input.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  if (value.at("resumeState").is_object()) {
    input.resume_state = agent_runner_durable_state_from_value(value.at("resumeState"));
  }
  return input;
}

AsyncAgentRunCancelInput async_agent_run_cancel_input_from_value(const Value& value) {
  AsyncAgentRunCancelInput input;
  if (!value.is_object()) {
    return input;
  }
  input.reason = value.at("reason").as_string(input.reason);
  input.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return input;
}

AsyncAgentRun async_agent_run_from_task(const AgentTask& task) {
  auto start = async_agent_run_start_input_from_value(task.input);
  if (start.kind.empty()) {
    start.kind = kAsyncAgentRunKindAgent;
  }
  const auto status = async_status_from_task_status(task.status);
  const auto updated_at_ms = time_point_to_ms(task.updated_at);
  ResourceLedger ledger = resource_ledger_from_value(task.output.at("resourceLedger"));
  ChildAgentResult result = child_agent_result_from_value(task.output.at("result"));
  AsyncAgentRun run{.id = task.id,
                    .type = task.type,
                    .kind = start.kind,
                    .status = status,
                    .session_id = start.session_id,
                    .root_run_id = start.root_run_id.empty() ? task.id : start.root_run_id,
                    .parent_run_id = start.parent_run_id,
                    .depth = start.depth < 0 ? 0 : start.depth,
                    .role = start.role.empty() ? (start.parent_run_id.empty() ? "orchestrator" : "leaf") : start.role,
                    .parent_child_relation = start.parent_child_relation,
                    .spawned_by_tool_call_id = start.spawned_by_tool_call_id,
                    .activity = lifecycle_activity(activity_phase_for_status(status), "run", updated_at_ms),
                    .resource_ledger = ledger,
                    .result = result,
                    .input = task.input,
                    .output = task.output,
                    .error = task.error,
                    .owner_api_key_id = task.owner_api_key_id,
                    .tenant_id = task.tenant_id,
                    .idempotency_key = task.idempotency_key,
                    .created_at_ms = time_point_to_ms(task.created_at),
                    .updated_at_ms = updated_at_ms,
                    .queued_at_ms = task.queued_at ? time_point_to_ms(*task.queued_at) : 0,
                    .started_at_ms = task.started_at ? time_point_to_ms(*task.started_at) : 0,
                    .completed_at_ms = task.completed_at ? time_point_to_ms(*task.completed_at) : 0,
                    .cancelled_at_ms = task.cancelled_at ? time_point_to_ms(*task.cancelled_at) : 0,
                    .metadata = task.metadata};
  if (run.result.run_id.empty()) {
    run.result = child_agent_result_from_run_output(run, task.output);
  }
  return run;
}

AsyncAgentRunAttempt async_agent_run_attempt_from_task_run(const AgentTaskRun& run) {
  return AsyncAgentRunAttempt{.id = run.id,
                              .run_id = run.task_id,
                              .status = async_status_from_task_status(run.status),
                              .attempt = run.attempt,
                              .started_at_ms = time_point_to_ms(run.started_at),
                              .completed_at_ms = run.completed_at ? time_point_to_ms(*run.completed_at) : 0,
                              .lease_expires_at_ms = time_point_to_ms(run.lease_expires_at),
                              .heartbeat_at_ms = time_point_to_ms(run.heartbeat_at),
                              .output = run.output,
                              .error = run.error};
}

AsyncAgentRunEvent async_agent_run_event_from_task_event(const TaskEvent& event) {
  return AsyncAgentRunEvent{.id = event.id,
                            .run_id = event.task_id,
                            .attempt_id = event.run_id,
                            .type = event.type,
                            .payload = event.payload,
                            .created_at_ms = time_point_to_ms(event.created_at)};
}

AsyncAgentRunCheckpoint async_agent_run_checkpoint_from_task_checkpoint(const TaskCheckpoint& checkpoint) {
  return AsyncAgentRunCheckpoint{.id = checkpoint.id,
                                 .run_id = checkpoint.task_id,
                                 .attempt_id = checkpoint.run_id,
                                 .name = checkpoint.name,
                                 .state = checkpoint.state,
                                 .created_at_ms = time_point_to_ms(checkpoint.created_at)};
}

Value async_agent_run_to_value(const AsyncAgentRun& run) {
  return Value::object({{"schemaVersion", 1},
                        {"id", run.id},
                        {"runId", run.id},
                        {"type", run.type},
                        {"kind", run.kind},
                        {"status", to_string(run.status)},
                        {"sessionId", run.session_id},
                        {"rootRunId", run.root_run_id},
                        {"parentRunId", run.parent_run_id},
                        {"depth", run.depth},
                        {"role", run.role},
                        {"parentChildRelation", run.parent_child_relation},
                        {"spawnedByToolCallId", run.spawned_by_tool_call_id},
                        {"topology", topology_payload_from_run(run)},
                        {"policy", run.input.at("childAgentPolicy").is_object()
                                       ? run.input.at("childAgentPolicy")
                                       : Value::object({})},
                        {"activity", async_run_activity_to_value(run.activity)},
                        {"resourceLedger", resource_ledger_to_value(run.resource_ledger)},
                        {"result", child_agent_result_to_value(run.result)},
                        {"input", run.input},
                        {"output", run.output},
                        {"error", run.error},
                        {"ownerApiKeyId", run.owner_api_key_id},
                        {"tenantId", run.tenant_id},
                        {"idempotencyKey", run.idempotency_key},
                        {"createdAt", iso8601_from_epoch_ms(run.created_at_ms)},
                        {"updatedAt", iso8601_from_epoch_ms(run.updated_at_ms)},
                        {"queuedAt", iso8601_from_epoch_ms(run.queued_at_ms)},
                        {"startedAt", iso8601_from_epoch_ms(run.started_at_ms)},
                        {"completedAt", iso8601_from_epoch_ms(run.completed_at_ms)},
                        {"cancelledAt", iso8601_from_epoch_ms(run.cancelled_at_ms)},
                        {"createdAtMs", run.created_at_ms},
                        {"updatedAtMs", run.updated_at_ms},
                        {"queuedAtMs", run.queued_at_ms},
                        {"startedAtMs", run.started_at_ms},
                        {"completedAtMs", run.completed_at_ms},
                        {"cancelledAtMs", run.cancelled_at_ms},
                        {"metadata", run.metadata}});
}

Value async_agent_run_attempt_to_value(const AsyncAgentRunAttempt& attempt) {
  return Value::object({{"id", attempt.id},
                        {"runId", attempt.run_id},
                        {"status", to_string(attempt.status)},
                        {"attempt", attempt.attempt},
                        {"startedAtMs", attempt.started_at_ms},
                        {"completedAtMs", attempt.completed_at_ms},
                        {"leaseExpiresAtMs", attempt.lease_expires_at_ms},
                        {"heartbeatAtMs", attempt.heartbeat_at_ms},
                        {"output", attempt.output},
                        {"error", attempt.error}});
}

Value async_agent_run_event_to_value(const AsyncAgentRunEvent& event) {
  return Value::object({{"id", event.id},
                        {"runId", event.run_id},
                        {"attemptId", event.attempt_id},
                        {"type", event.type},
                        {"payload", event.payload},
                        {"createdAtMs", event.created_at_ms}});
}

Value async_agent_run_checkpoint_to_value(const AsyncAgentRunCheckpoint& checkpoint) {
  return Value::object({{"id", checkpoint.id},
                        {"runId", checkpoint.run_id},
                        {"attemptId", checkpoint.attempt_id},
                        {"name", checkpoint.name},
                        {"state", checkpoint.state},
                        {"createdAtMs", checkpoint.created_at_ms}});
}

Value async_agent_run_snapshot_to_value(const AsyncAgentRunSnapshot& snapshot) {
  Value::Array attempts;
  for (const auto& attempt : snapshot.attempts) {
    attempts.push_back(async_agent_run_attempt_to_value(attempt));
  }
  Value::Array events;
  for (const auto& event : snapshot.events) {
    events.push_back(async_agent_run_event_to_value(event));
  }
  Value::Array checkpoints;
  for (const auto& checkpoint : snapshot.checkpoints) {
    checkpoints.push_back(async_agent_run_checkpoint_to_value(checkpoint));
  }
  Value::Array transcript;
  for (const auto& entry : snapshot.transcript) {
    transcript.push_back(run_transcript_entry_to_value(entry));
  }
  return Value::object({{"run", async_agent_run_to_value(snapshot.run)},
                        {"attempts", Value(std::move(attempts))},
                        {"events", Value(std::move(events))},
                        {"checkpoints", Value(std::move(checkpoints))},
                        {"transcript", Value(std::move(transcript))}});
}

AgentLoopDurablePhase agent_loop_durable_phase_from_string(const std::string& value,
                                                           AgentLoopDurablePhase fallback) {
  if (value == "before-model") return AgentLoopDurablePhase::BeforeModel;
  if (value == "model-completed") return AgentLoopDurablePhase::ModelCompleted;
  if (value == "action-batch-parsed") return AgentLoopDurablePhase::ActionBatchParsed;
  if (value == "tool-batch-completed") return AgentLoopDurablePhase::ToolBatchCompleted;
  if (value == "completed") return AgentLoopDurablePhase::Completed;
  return fallback;
}

AgentLoopTerminationReason agent_loop_termination_reason_from_string(
    const std::string& value,
    AgentLoopTerminationReason fallback) {
  if (value == "max-iterations") return AgentLoopTerminationReason::MaxIterations;
  if (value == "incomplete-response") return AgentLoopTerminationReason::IncompleteResponse;
  if (value == "completed") return AgentLoopTerminationReason::Completed;
  return fallback;
}

AgentRunnerDurableStatus agent_runner_durable_status_from_string(const std::string& value,
                                                                 AgentRunnerDurableStatus fallback) {
  if (value == "completed") return AgentRunnerDurableStatus::Completed;
  if (value == "interrupted") return AgentRunnerDurableStatus::Interrupted;
  if (value == "running") return AgentRunnerDurableStatus::Running;
  return fallback;
}

AgentLoopDurableState agent_loop_durable_state_from_value(const Value& value) {
  AgentLoopDurableState state;
  if (!value.is_object()) {
    return state;
  }
  state.version = static_cast<int>(value.at("version").as_integer(1));
  state.phase = agent_loop_durable_phase_from_string(value.at("phase").as_string());
  state.session_id = value.at("sessionId").as_string();
  state.next_iteration = static_cast<int>(value.at("nextIteration").as_integer());
  state.input_text = value.at("inputText").as_string();
  if (value.at("inputMessage").is_object()) {
    state.input_message = agent_message_from_value(value.at("inputMessage"));
  }
  state.input_value = value.at("inputValue");
  state.input_parts = state.input_message.content;
  if (value.at("session").is_object()) {
    state.session = session_memory_snapshot_from_value(value.at("session"));
  }
  state.trace = durable_trace_from_value(value.at("trace"));
  state.react_trace = react_trace_from_value(value.at("reactTrace"));
  if (value.at("lastResponse").is_object()) {
    state.last_response = durable_model_response_from_value(value.at("lastResponse"));
  }
  state.last_assistant_text = value.at("lastAssistantText").as_string();
  state.latest_non_empty_reasoning = value.at("latestNonEmptyReasoning").as_string();
  state.consecutive_parse_errors =
      static_cast<int>(value.at("consecutiveParseErrors").as_integer());
  state.consecutive_reasoning_protocol_leaks =
      static_cast<int>(value.at("consecutiveReasoningProtocolLeaks").as_integer());
  if (value.contains("terminationReason")) {
    state.termination_reason =
        agent_loop_termination_reason_from_string(value.at("terminationReason").as_string());
  }
  return state;
}

AgentRunnerDurableState agent_runner_durable_state_from_value(const Value& value) {
  AgentRunnerDurableState state;
  if (!value.is_object()) {
    return state;
  }
  state.version = static_cast<int>(value.at("version").as_integer(1));
  state.status = agent_runner_durable_status_from_string(value.at("status").as_string());
  state.run_id = value.at("runId").as_string();
  state.session_id = value.at("sessionId").as_string();
  state.input_value = value.at("input");
  state.input = state.input_value.is_string() ? state.input_value.as_string() : safe_json_stringify(state.input_value);
  if (value.at("inputMessage").is_object()) {
    state.input_message = agent_message_from_value(value.at("inputMessage"));
  }
  state.model_settings = model_settings_from_json_value(value.at("modelSettings"));
  state.effective_input_value = value.at("effectiveInput");
  state.effective_input = state.effective_input_value.is_string()
                              ? state.effective_input_value.as_string()
                              : safe_json_stringify(state.effective_input_value);
  if (value.at("effectiveInputMessage").is_object()) {
    state.effective_input_message = agent_message_from_value(value.at("effectiveInputMessage"));
  }
  state.input_text = value.at("inputText").as_string();
  state.input_parts = state.input_message.content;
  state.preface_messages = messages_from_value(value.at("prefaceMessages"));
  state.knowledge_debug = value.at("knowledgeDebug").is_object() ? value.at("knowledgeDebug") : Value::object({});
  if (value.at("loop").is_object()) {
    state.loop = agent_loop_durable_state_from_value(value.at("loop"));
  }
  state.updated_at = value.at("updatedAt").as_string();
  return state;
}

}  // namespace agent
