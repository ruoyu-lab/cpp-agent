#include "internal.hpp"

#include <utility>

namespace agent {

AgentRunnerStatus runner_status(std::string kind,
                                 std::string stage,
                                 std::string state,
                                 std::string message,
                                 Value details) {
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
    case AgentLoopStreamEventType::ModelReasoningCompleted:
      return AgentRunnerStatus{
          .kind = "thinking",
          .stage = "model",
          .state = "complete",
          .message = "Model reasoning completed.",
          .iteration = event.iteration,
          .provider = event.provider,
          .model = event.model,
          .details = Value::object({{"reasoning", event.reasoning},
                                    {"reasoningId", event.reasoning_id},
                                    {"scope", event.reasoning_scope}}),
      };
    case AgentLoopStreamEventType::AgentOutput: {
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
    case AgentLoopStreamEventType::ReActActionBatch:
      return AgentRunnerStatus{
          .kind = "working",
          .stage = "react",
          .state = "action-batch",
          .message = "ReAct action batch parsed.",
          .iteration = event.iteration,
          .details = react_trace_entry_to_value(event.react_step),
      };
    case AgentLoopStreamEventType::ToolBatchStart:
    {
      Value::Array calls;
      calls.reserve(event.tool_calls.size());
      for (const auto& call : event.tool_calls) {
        calls.push_back(Value::object({
            {"id", call.id},
            {"name", call.name},
            {"arguments", call.arguments},
        }));
      }
      return AgentRunnerStatus{
          .kind = "working",
          .stage = "tool",
          .state = "batch-start",
          .message = "Starting tool batch.",
          .iteration = event.iteration,
          .details = Value::object({{"count", static_cast<long long>(event.tool_calls.size())},
                                    {"toolCalls", Value(std::move(calls))}}),
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
    case AgentLoopStreamEventType::ToolDelta:
      return std::nullopt;
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
                                    {"structuredOutput", tool_execution_structured_output_value(event.tool_result)},
                                    {"output", event.tool_result.output}}),
      };
    case AgentLoopStreamEventType::ToolBatchComplete:
      return std::nullopt;
    case AgentLoopStreamEventType::ReActMessage:
      return AgentRunnerStatus{
          .kind = "answering",
          .stage = "react",
          .state = "message",
          .message = event.react_step.visible_message,
          .iteration = event.iteration,
          .details = react_trace_entry_to_value(event.react_step),
      };
    case AgentLoopStreamEventType::ReActObservation:
      return AgentRunnerStatus{
          .kind = "working",
          .stage = "react",
          .state = "observation",
          .message = event.react_step.ok ? "ReAct observation received."
                                         : "ReAct observation reported a failure.",
          .iteration = event.iteration,
          .details = react_trace_entry_to_value(event.react_step),
      };
    case AgentLoopStreamEventType::ReActFinal:
      return AgentRunnerStatus{
          .kind = "answering",
          .stage = "react",
          .state = "final",
          .message = "ReAct final answer produced.",
          .iteration = event.iteration,
          .details = react_trace_entry_to_value(event.react_step),
      };
    case AgentLoopStreamEventType::ReActFinalRejected:
      return AgentRunnerStatus{
          .kind = "working",
          .stage = "react",
          .state = "final-rejected",
          .message = event.react_step.error.empty()
                         ? "ReAct final answer rejected."
                         : "ReAct final answer rejected: " + event.react_step.error,
          .iteration = event.iteration,
          .details = react_trace_entry_to_value(event.react_step),
      };
    case AgentLoopStreamEventType::ReActReasoningProtocolLeak:
      return AgentRunnerStatus{
          .kind = "thinking",
          .stage = "react",
          .state = "reasoning-protocol-leak",
          .message = event.react_step.error.empty()
                         ? "ReAct protocol was emitted in reasoning channel."
                         : event.react_step.error,
          .iteration = event.iteration,
          .details = react_trace_entry_to_value(event.react_step),
      };
    case AgentLoopStreamEventType::ReActParseError:
      return AgentRunnerStatus{
          .kind = "thinking",
          .stage = "react",
          .state = "parse-error",
          .message = event.react_step.error,
          .iteration = event.iteration,
          .details = react_trace_entry_to_value(event.react_step),
      };
    case AgentLoopStreamEventType::IterationStart:
    case AgentLoopStreamEventType::ModelTextDelta:
    case AgentLoopStreamEventType::UserVisibleDelta:
    case AgentLoopStreamEventType::ToolCallArgumentDelta:
    case AgentLoopStreamEventType::Done:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace agent
