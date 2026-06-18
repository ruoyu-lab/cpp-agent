#include "agent/realtime.hpp"

#include <utility>

namespace agent {

namespace {

Value strings_to_value_array(const std::vector<std::string>& values) {
  Value::Array out;
  out.reserve(values.size());
  for (const auto& value : values) {
    out.emplace_back(value);
  }
  return Value(std::move(out));
}

Value optional_error_to_value(const std::string& message) {
  if (message.empty()) {
    return Value();
  }
  return Value::object({{"message", message}});
}

bool likely_json_prefix(const std::string& value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return false;
  }
  return value[first] == '{' || value[first] == '[';
}

Value tool_result_output_value(const ToolExecutionResult& result) {
  if (!result.result) {
    return result.output.empty() ? Value() : Value(result.output);
  }
  if (std::holds_alternative<Value>(*result.result)) {
    return std::get<Value>(*result.result);
  }
  const auto& envelope = std::get<ToolResultEnvelope>(*result.result);
  if (envelope.value) {
    return *envelope.value;
  }
  return result.output.empty() ? Value() : Value(result.output);
}

}  // namespace

std::string to_string(RealtimeSessionState state) {
  switch (state) {
    case RealtimeSessionState::Idle:
      return "idle";
    case RealtimeSessionState::Connecting:
      return "connecting";
    case RealtimeSessionState::Open:
      return "open";
    case RealtimeSessionState::Listening:
      return "listening";
    case RealtimeSessionState::Responding:
      return "responding";
    case RealtimeSessionState::ToolWaiting:
      return "tool_waiting";
    case RealtimeSessionState::Interrupted:
      return "interrupted";
    case RealtimeSessionState::Closing:
      return "closing";
    case RealtimeSessionState::Closed:
      return "closed";
    case RealtimeSessionState::Failed:
      return "failed";
  }
  return "idle";
}

RealtimeSessionState realtime_session_state_from_string(const std::string& value,
                                                        RealtimeSessionState fallback) {
  if (value == "idle") return RealtimeSessionState::Idle;
  if (value == "connecting") return RealtimeSessionState::Connecting;
  if (value == "open") return RealtimeSessionState::Open;
  if (value == "listening") return RealtimeSessionState::Listening;
  if (value == "responding") return RealtimeSessionState::Responding;
  if (value == "tool_waiting") return RealtimeSessionState::ToolWaiting;
  if (value == "interrupted") return RealtimeSessionState::Interrupted;
  if (value == "closing") return RealtimeSessionState::Closing;
  if (value == "closed") return RealtimeSessionState::Closed;
  if (value == "failed") return RealtimeSessionState::Failed;
  return fallback;
}

std::string to_string(RealtimeSessionEventType type) {
  switch (type) {
    case RealtimeSessionEventType::SessionOpened:
      return "session.opened";
    case RealtimeSessionEventType::SessionUpdated:
      return "session.updated";
    case RealtimeSessionEventType::SessionClosed:
      return "session.closed";
    case RealtimeSessionEventType::SessionError:
      return "session.error";
    case RealtimeSessionEventType::InputText:
      return "input.text";
    case RealtimeSessionEventType::InputAudioDelta:
      return "input.audio.delta";
    case RealtimeSessionEventType::InputAudioCommitted:
      return "input.audio.committed";
    case RealtimeSessionEventType::InputTranscriptDelta:
      return "input.transcript.delta";
    case RealtimeSessionEventType::InputTranscriptDone:
      return "input.transcript.done";
    case RealtimeSessionEventType::ResponseStarted:
      return "response.started";
    case RealtimeSessionEventType::ResponseDone:
      return "response.done";
    case RealtimeSessionEventType::ResponseInterrupted:
      return "response.interrupted";
    case RealtimeSessionEventType::OutputTextDelta:
      return "output.text.delta";
    case RealtimeSessionEventType::OutputTextDone:
      return "output.text.done";
    case RealtimeSessionEventType::OutputAudioDelta:
      return "output.audio.delta";
    case RealtimeSessionEventType::OutputAudioDone:
      return "output.audio.done";
    case RealtimeSessionEventType::OutputTranscriptDelta:
      return "output.transcript.delta";
    case RealtimeSessionEventType::OutputTranscriptDone:
      return "output.transcript.done";
    case RealtimeSessionEventType::ToolCallStarted:
      return "tool.call.started";
    case RealtimeSessionEventType::ToolCallArgumentsDelta:
      return "tool.call.arguments.delta";
    case RealtimeSessionEventType::ToolCallReady:
      return "tool.call.ready";
    case RealtimeSessionEventType::ToolCallResult:
      return "tool.call.result";
    case RealtimeSessionEventType::ToolCallError:
      return "tool.call.error";
    case RealtimeSessionEventType::Custom:
      return "custom";
  }
  return "custom";
}

RealtimeSessionEventType realtime_session_event_type_from_string(const std::string& value,
                                                                  RealtimeSessionEventType fallback) {
  if (value == "session.opened") return RealtimeSessionEventType::SessionOpened;
  if (value == "session.updated") return RealtimeSessionEventType::SessionUpdated;
  if (value == "session.closed") return RealtimeSessionEventType::SessionClosed;
  if (value == "session.error") return RealtimeSessionEventType::SessionError;
  if (value == "input.text") return RealtimeSessionEventType::InputText;
  if (value == "input.audio.delta") return RealtimeSessionEventType::InputAudioDelta;
  if (value == "input.audio.committed") return RealtimeSessionEventType::InputAudioCommitted;
  if (value == "input.transcript.delta") return RealtimeSessionEventType::InputTranscriptDelta;
  if (value == "input.transcript.done") return RealtimeSessionEventType::InputTranscriptDone;
  if (value == "response.started") return RealtimeSessionEventType::ResponseStarted;
  if (value == "response.done") return RealtimeSessionEventType::ResponseDone;
  if (value == "response.interrupted") return RealtimeSessionEventType::ResponseInterrupted;
  if (value == "output.text.delta") return RealtimeSessionEventType::OutputTextDelta;
  if (value == "output.text.done") return RealtimeSessionEventType::OutputTextDone;
  if (value == "output.audio.delta") return RealtimeSessionEventType::OutputAudioDelta;
  if (value == "output.audio.done") return RealtimeSessionEventType::OutputAudioDone;
  if (value == "output.transcript.delta") return RealtimeSessionEventType::OutputTranscriptDelta;
  if (value == "output.transcript.done") return RealtimeSessionEventType::OutputTranscriptDone;
  if (value == "tool.call.started") return RealtimeSessionEventType::ToolCallStarted;
  if (value == "tool.call.arguments.delta") return RealtimeSessionEventType::ToolCallArgumentsDelta;
  if (value == "tool.call.ready") return RealtimeSessionEventType::ToolCallReady;
  if (value == "tool.call.result") return RealtimeSessionEventType::ToolCallResult;
  if (value == "tool.call.error") return RealtimeSessionEventType::ToolCallError;
  return fallback;
}

Value realtime_audio_chunk_to_value(const RealtimeAudioChunk& chunk) {
  return Value::object({
      {"data", chunk.data},
      {"encoding", chunk.encoding.empty() ? Value() : Value(chunk.encoding)},
      {"mimeType", chunk.mime_type.empty() ? Value() : Value(chunk.mime_type)},
      {"sampleRate", chunk.sample_rate > 0 ? Value(chunk.sample_rate) : Value()},
      {"channels", chunk.channels > 0 ? Value(chunk.channels) : Value()},
      {"sequence", chunk.sequence > 0 ? Value(static_cast<long long>(chunk.sequence)) : Value()},
      {"metadata", chunk.metadata.is_object() ? chunk.metadata : Value::object({})},
  });
}

Value realtime_tool_call_to_value(const RealtimeToolCall& call) {
  return Value::object({
      {"id", call.id},
      {"name", call.name},
      {"arguments", call.arguments.is_object() ? call.arguments : Value::object({})},
      {"argumentsText", call.arguments_text.empty() ? Value() : Value(call.arguments_text)},
      {"metadata", call.metadata.is_object() ? call.metadata : Value::object({})},
  });
}

Value realtime_tool_result_to_value(const RealtimeToolResult& result) {
  return Value::object({
      {"toolCallId", result.tool_call_id},
      {"name", result.name.empty() ? Value() : Value(result.name)},
      {"ok", result.ok},
      {"output", result.output},
      {"error", optional_error_to_value(result.error)},
      {"metadata", result.metadata.is_object() ? result.metadata : Value::object({})},
  });
}

Value realtime_session_event_to_value(const RealtimeSessionEvent& event) {
  const auto type = event.type == RealtimeSessionEventType::Custom && !event.custom_type.empty()
                        ? event.custom_type
                        : to_string(event.type);
  return Value::object({
      {"id", event.id},
      {"sessionId", event.session_id},
      {"sequence", static_cast<long long>(event.sequence)},
      {"timestamp", event.timestamp},
      {"type", type},
      {"turnId", event.turn_id.empty() ? Value() : Value(event.turn_id)},
      {"responseId", event.response_id.empty() ? Value() : Value(event.response_id)},
      {"itemId", event.item_id.empty() ? Value() : Value(event.item_id)},
      {"toolCallId", event.tool_call_id.empty() ? Value() : Value(event.tool_call_id)},
      {"state", event.state ? Value(to_string(*event.state)) : Value()},
      {"provider", event.provider.empty() ? Value() : Value(event.provider)},
      {"model", event.model.empty() ? Value() : Value(event.model)},
      {"text", event.text.empty() ? Value() : Value(event.text)},
      {"delta", event.delta.empty() ? Value() : Value(event.delta)},
      {"audio", event.audio ? realtime_audio_chunk_to_value(*event.audio) : Value()},
      {"toolCall", event.tool_call ? realtime_tool_call_to_value(*event.tool_call) : Value()},
      {"toolResult", event.tool_result ? realtime_tool_result_to_value(*event.tool_result) : Value()},
      {"error", optional_error_to_value(event.error)},
      {"traceContext", event.trace_context.is_object() ? event.trace_context : Value::object({})},
      {"metadata", event.metadata.is_object() ? event.metadata : Value::object({})},
  });
}

Value realtime_provider_capabilities_to_value(const RealtimeProviderCapabilities& capabilities) {
  return Value::object({
      {"provider", capabilities.provider},
      {"models", strings_to_value_array(capabilities.models)},
      {"modalities", strings_to_value_array(capabilities.modalities)},
      {"inputAudio", capabilities.input_audio},
      {"outputAudio", capabilities.output_audio},
      {"interruption", capabilities.interruption},
      {"toolCalling", capabilities.tool_calling},
      {"settings", capabilities.settings.is_object() ? capabilities.settings : Value::object({})},
  });
}

RealtimeToolBridge::RealtimeToolBridge(RealtimeToolBridgeOptions options)
    : executor_(options.executor), context_(std::move(options.context)) {}

RealtimeToolResult RealtimeToolBridge::execute(const RealtimeToolCall& call) {
  if (!executor_) {
    throw ConfigurationError("RealtimeToolBridge requires a ToolExecutor.");
  }
  ToolCall tool_call;
  tool_call.id = call.id;
  tool_call.name = call.name;
  if (call.arguments.is_object() && !call.arguments.as_object().empty()) {
    tool_call.arguments = call.arguments;
  } else if (!call.arguments_text.empty()) {
    auto parsed = parse_json(call.arguments_text);
    tool_call.arguments = parsed.is_object() ? parsed : Value::object({});
  } else {
    tool_call.arguments = Value::object({});
  }
  auto result = executor_->execute_tool_call(tool_call, context_);
  RealtimeToolResult output;
  output.tool_call_id = call.id;
  output.name = call.name;
  output.ok = result.ok;
  output.output = tool_result_output_value(result);
  output.error = result.error;
  output.metadata = Value::object({{"outputText", result.output}});
  return output;
}

bool RealtimeToolBridge::execute_event(const RealtimeSessionEvent& event,
                                       RealtimeToolResult& out_result) {
  if (event.type == RealtimeSessionEventType::ToolCallArgumentsDelta && event.tool_call) {
    auto& buffer = argument_buffers_[event.tool_call->id];
    buffer += event.delta.empty() ? event.tool_call->arguments_text : event.delta;
    return false;
  }
  if (event.type != RealtimeSessionEventType::ToolCallReady || !event.tool_call) {
    return false;
  }
  auto call = *event.tool_call;
  const auto found = argument_buffers_.find(call.id);
  if (found != argument_buffers_.end()) {
    const auto ready_text = event.delta.empty() ? call.arguments_text : event.delta;
    call.arguments_text = likely_json_prefix(ready_text) ? ready_text : found->second + ready_text;
    argument_buffers_.erase(found);
  }
  out_result = execute(call);
  return true;
}

}  // namespace agent
