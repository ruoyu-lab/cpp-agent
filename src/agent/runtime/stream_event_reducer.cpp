#include "stream_event_reducer.hpp"

namespace agent {

StreamEventReducer::StreamEventReducer(EventBus* event_bus)
    : event_bus_(event_bus) {}

void StreamEventReducer::reduce_model_event(
    const ModelStreamEvent& event,
    int iteration,
    const TraceContext& model_trace,
    const AgentLoopStreamEventHandler& emit_stream_event,
    ModelStreamReductionState& state) const {
  if (event.type == ModelStreamEventType::ResponseStart) {
    emit_stream_event(AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::ModelStart,
        .iteration = iteration,
        .provider = event.provider,
        .model = event.model,
    });
    return;
  }
  if (event.type == ModelStreamEventType::TextDelta) {
    emit_stream_event(AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::ModelTextDelta,
        .iteration = iteration,
        .provider = event.provider,
        .model = event.model,
        .delta = event.delta,
        .text = event.text,
    });
    if (event_bus_) {
      event_bus_->publish("model.delta", ExecutionTarget::Model,
                          Value::object({{"iteration", iteration},
                                         {"delta", event.delta},
                                         {"text", event.text}}),
                          model_trace);
    }
    return;
  }
  if (event.type == ModelStreamEventType::ReasoningDelta) {
    emit_stream_event(AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::ModelReasoningDelta,
        .iteration = iteration,
        .provider = event.provider,
        .model = event.model,
        .delta = event.delta,
        .reasoning = event.reasoning,
    });
    if (event_bus_) {
      event_bus_->publish("model.reasoning_delta", ExecutionTarget::Model,
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
    emit_stream_event(AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::ToolCallArgumentDelta,
        .iteration = iteration,
        .provider = event.provider,
        .model = event.model,
        .tool_call_id = event.tool_call_id,
        .tool_call_name = event.tool_call_name,
        .tool_call_args_delta = event.tool_call_args_delta,
        .tool_call_args_accumulated = event.tool_call_args_accumulated,
    });
    if (event_bus_) {
      event_bus_->publish("model.tool_call_delta", ExecutionTarget::Model,
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
    state.response = event.response;
    state.saw_response = true;
    emit_stream_event(AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::AgentOutput,
        .iteration = iteration,
        .response = state.response,
    });
  }
}

}  // namespace agent
