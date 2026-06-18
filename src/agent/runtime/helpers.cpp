#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>

namespace agent {

bool model_settings_empty(const ModelSettings& settings) {
  return settings.model.empty() && !settings.temperature && !settings.max_output_tokens && !settings.reasoning
         && settings.extra.as_object().empty();
}

bool has_trace_values(const TraceContext& trace) {
  return !trace.trace_id.empty() || !trace.span_id.empty() || !trace.parent_span_id.empty() ||
         !trace.span_name.empty() || !trace.run_id.empty() || !trace.workflow_run_id.empty();
}

TraceContext child_or_root_trace_context(const TraceContext& parent, std::string span_name,
                                         TraceContext overrides) {
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

Value message_content_parts_value(const std::vector<MessageContentPart>& parts) {
  return agent_message_to_value(create_message(MessageRole::User, parts)).at("content");
}

Value model_response_hook_value(const AgentOutput& response) {
  return Value::object({
      {"provider", response.provider},
      {"model", response.model},
      {"text", response.text},
      {"finishReason", response.finish_reason},
      {"raw", response.raw},
  });
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
                                const std::string& run_id) {
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
                                  const std::string& run_id) {
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
                               const std::string& run_id) {
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

Value run_cancelled_event_payload(Value input,
                                  const std::string& session_id,
                                  const CancellationError& error,
                                  const std::string& run_id) {
  Value payload = Value::object({
      {"sessionId", session_id},
      {"input", std::move(input)},
      {"message", std::string(error.what())},
      {"reason", error.reason().empty() ? Value() : Value(error.reason())},
      {"target", error.target().empty() ? Value() : Value(error.target())},
      {"code", "cancelled"},
  });
  if (!run_id.empty()) {
    payload["runId"] = run_id;
  }
  return payload;
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

Value tool_execution_structured_output_value(const ToolExecutionResult& result) {
  if (!result.result) {
    return Value();
  }
  if (std::holds_alternative<Value>(*result.result)) {
    return std::get<Value>(*result.result);
  }
  const auto& envelope = std::get<ToolResultEnvelope>(*result.result);
  return envelope.value ? *envelope.value : Value();
}

Value retry_scheduled_event_payload(const RetryScheduledContext& retry,
                                    Value extra) {
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

internal::AgentLoop::AgentLoop(internal::AgentLoopConfig config) : config_(std::move(config)) {
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


}  // namespace agent
