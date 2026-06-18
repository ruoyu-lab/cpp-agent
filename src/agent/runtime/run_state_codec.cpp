#include "run_state_codec.hpp"

#include <utility>
#include <variant>

namespace agent {

const char* RunStateCodec::loop_phase_to_string(AgentLoopDurablePhase phase) {
  switch (phase) {
    case AgentLoopDurablePhase::BeforeModel:
      return "before-model";
    case AgentLoopDurablePhase::ModelCompleted:
      return "model-completed";
    case AgentLoopDurablePhase::ActionBatchParsed:
      return "action-batch-parsed";
    case AgentLoopDurablePhase::ToolBatchCompleted:
      return "tool-batch-completed";
    case AgentLoopDurablePhase::Completed:
      return "completed";
  }
  return "before-model";
}

const char* RunStateCodec::runner_status_to_string(AgentRunnerDurableStatus status) {
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

AgentLoopDurableState RunStateCodec::build_loop_checkpoint(
    AgentLoopDurablePhase phase,
    SessionMemory& session,
    int next_iteration,
    const std::string& input_text,
    const std::vector<MessageContentPart>& input_parts,
    const AgentMessage& input_message,
    const Value& input_value,
    const std::vector<AgentTraceEntry>& trace,
    const AgentOutput* last_response,
    const std::string& last_assistant_text,
    std::optional<AgentLoopTerminationReason> termination_reason) {
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
  if (last_response) {
    state.last_response = *last_response;
  }
  state.last_assistant_text = last_assistant_text;
  state.termination_reason = termination_reason;
  return state;
}

Value RunStateCodec::loop_to_value(const AgentLoopDurableState& state) {
  Value::Array trace;
  trace.reserve(state.trace.size());
  for (const auto& entry : state.trace) {
    trace.push_back(trace_entry_to_value(entry));
  }
  Value::Array react_trace;
  react_trace.reserve(state.react_trace.size());
  for (const auto& entry : state.react_trace) {
    react_trace.push_back(react_trace_entry_to_value(entry));
  }
  Value value = Value::object({
      {"version", state.version},
      {"phase", loop_phase_to_string(state.phase)},
      {"sessionId", state.session_id},
      {"nextIteration", state.next_iteration},
      {"inputText", state.input_text},
      {"inputParts", message_content_parts_value(state.input_parts)},
      {"inputMessage", agent_message_to_value(state.input_message)},
      {"inputValue", state.input_value},
      {"session", session_memory_snapshot_to_value(state.session)},
      {"trace", Value(std::move(trace))},
      {"reactTrace", Value(std::move(react_trace))},
      {"lastResponse", state.last_response ? model_response_to_value(*state.last_response) : Value()},
      {"lastAssistantText", state.last_assistant_text},
      {"latestNonEmptyReasoning", state.latest_non_empty_reasoning},
      {"consecutiveParseErrors", state.consecutive_parse_errors},
      {"consecutiveReasoningProtocolLeaks", state.consecutive_reasoning_protocol_leaks},
  });
  if (state.termination_reason) {
    value["terminationReason"] = to_string(*state.termination_reason);
  }
  return value;
}

Value RunStateCodec::runner_to_value(const AgentRunnerDurableState& state) {
  return Value::object({
      {"version", state.version},
      {"status", runner_status_to_string(state.status)},
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
      {"memoryHits", memory_hits_to_value(state.memory_hits)},
      {"knowledgeHits", knowledge_hits_value(state.knowledge_hits)},
      {"knowledgeDebug", state.knowledge_debug},
      {"prefaceMessages", messages_to_value(state.preface_messages)},
      {"loop", state.loop ? loop_to_value(*state.loop) : Value()},
      {"updatedAt", state.updated_at},
  });
}

Value RunStateCodec::tool_call_to_value(const ToolCall& tool_call) {
  return Value::object({
      {"id", tool_call.id},
      {"name", tool_call.name},
      {"arguments", tool_call.arguments},
  });
}

Value RunStateCodec::model_response_to_value(const AgentOutput& response) {
  return agent_output_to_value(response);
}

Value RunStateCodec::tool_execution_result_to_value(const ToolExecutionResult& result) {
  Value::Object object{
      {"toolCall", tool_call_to_value(result.tool_call)},
      {"ok", result.ok},
      {"error", result.error.empty() ? Value() : Value(result.error)},
      {"output", result.output},
      {"structuredOutput", tool_execution_structured_output_value(result)},
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

Value RunStateCodec::trace_entry_to_value(const AgentTraceEntry& entry) {
  Value::Object object{{"type", entry.type}, {"iteration", entry.iteration}};
  if (entry.type == "model") {
    object["response"] = model_response_to_value(entry.response);
  }
  if (!entry.tool_results.empty()) {
    Value::Array results;
    results.reserve(entry.tool_results.size());
    for (const auto& result : entry.tool_results) {
      results.push_back(tool_execution_result_to_value(result));
    }
    object["toolResults"] = Value(std::move(results));
  }
  return Value(std::move(object));
}

Value RunStateCodec::reasoning_settings_to_value(const ReasoningSettings& reasoning) {
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

Value RunStateCodec::model_settings_to_value(const ModelSettings& settings) {
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

Value RunStateCodec::messages_to_value(const std::vector<AgentMessage>& messages) {
  Value::Array values;
  values.reserve(messages.size());
  for (const auto& message : messages) {
    values.push_back(agent_message_to_value(message));
  }
  return Value(std::move(values));
}

Value RunStateCodec::memory_hits_to_value(const std::vector<RetrievedMemory>& hits) {
  Value::Array values;
  values.reserve(hits.size());
  for (const auto& hit : hits) {
    values.push_back(retrieved_memory_hook_value(hit));
  }
  return Value(std::move(values));
}

}  // namespace agent
