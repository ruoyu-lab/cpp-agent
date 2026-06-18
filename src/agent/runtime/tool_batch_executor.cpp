#include "tool_batch_executor.hpp"

#include <utility>

namespace agent {

ToolBatchExecutor::ToolBatchExecutor(ToolExecutor& tool_executor)
    : tool_executor_(tool_executor) {}

ToolBatchExecutionResult ToolBatchExecutor::execute_batch(const std::vector<ToolCall>& tool_calls,
                                                          ToolExecutionContext tool_context) const {
  return ToolBatchExecutionResult{
      .tool_results = tool_executor_.execute_all(tool_calls, std::move(tool_context)),
  };
}

ToolBatchExecutionResult ToolBatchExecutor::stream_batch(
    const std::vector<ToolCall>& tool_calls,
    ToolExecutionContext tool_context,
    int iteration,
    AgentLoopStreamEventHandler emit_stream_event,
    CancellationToken* cancellation) const {
  std::vector<ToolExecutionResult> tool_results;
  tool_results.reserve(tool_calls.size());
  emit_stream_event(AgentLoopStreamEvent{
      .type = AgentLoopStreamEventType::ToolBatchStart,
      .iteration = iteration,
      .tool_calls = tool_calls,
  });
  for (const auto& tool_call : tool_calls) {
    emit_stream_event(AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::ToolStart,
        .iteration = iteration,
        .tool_call = tool_call,
    });
    auto tool_result = execute_tool_call_safely(tool_call, tool_context, iteration,
                                                emit_stream_event, cancellation);
    tool_results.push_back(std::move(tool_result));
  }
  return ToolBatchExecutionResult{.tool_results = std::move(tool_results)};
}

ToolExecutionResult ToolBatchExecutor::execute_tool_call_safely(
    const ToolCall& tool_call,
    ToolExecutionContext& tool_context,
    int iteration,
    const AgentLoopStreamEventHandler& emit_stream_event,
    CancellationToken* cancellation) const {
  ToolExecutionResult tool_result;
  try {
    tool_result = tool_executor_.execute_tool_call(tool_call, tool_context);
  } catch (const std::exception& error) {
    tool_result = synthetic_tool_failure_result(tool_call,
                                                synthetic_tool_failure_message(tool_call, error));
    if (cancellation && cancellation->cancelled()) {
      emit_stream_event(AgentLoopStreamEvent{
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
  emit_stream_event(AgentLoopStreamEvent{
      .type = AgentLoopStreamEventType::ToolComplete,
      .iteration = iteration,
      .tool_result = tool_result,
  });
  return tool_result;
}

}  // namespace agent
