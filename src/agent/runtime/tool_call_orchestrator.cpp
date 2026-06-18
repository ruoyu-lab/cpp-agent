#include "tool_call_orchestrator.hpp"

#include <utility>

namespace agent {

ToolCallOrchestrator::ToolCallOrchestrator(ToolExecutor& tool_executor)
    : tool_executor_(tool_executor) {}

ToolCallOrchestrationResult ToolCallOrchestrator::run_tool_calls(
    const std::vector<ToolCall>& tool_calls,
    SessionMemory& session,
    std::vector<AgentTraceEntry>& trace,
    ToolExecutionContext tool_context,
    int iteration,
    CancellationToken* cancellation) const {
  tool_context = prepare_tool_context(std::move(tool_context), iteration, cancellation);
  auto batch = ToolBatchExecutor(tool_executor_).execute_batch(tool_calls, tool_context);
  commit_tool_results(session, trace, iteration, batch.tool_results);
  return ToolCallOrchestrationResult{
      .tool_results = std::move(batch.tool_results),
      .tool_context = std::move(tool_context),
  };
}

ToolCallOrchestrationResult ToolCallOrchestrator::stream_tool_calls(
    const std::vector<ToolCall>& tool_calls,
    SessionMemory& session,
    std::vector<AgentTraceEntry>& trace,
    ToolExecutionContext tool_context,
    int iteration,
    AgentLoopStreamEventHandler emit_stream_event,
    CancellationToken* cancellation) const {
  tool_context = prepare_tool_context(std::move(tool_context), iteration, cancellation);
  auto batch = ToolBatchExecutor(tool_executor_).stream_batch(tool_calls, tool_context, iteration,
                                                              emit_stream_event, cancellation);
  commit_tool_results(session, trace, iteration, batch.tool_results);
  emit_stream_event(AgentLoopStreamEvent{
      .type = AgentLoopStreamEventType::ToolBatchComplete,
      .iteration = iteration,
      .tool_calls = tool_calls,
      .tool_results = batch.tool_results,
  });
  return ToolCallOrchestrationResult{
      .tool_results = std::move(batch.tool_results),
      .tool_context = std::move(tool_context),
  };
}

ToolExecutionContext ToolCallOrchestrator::prepare_tool_context(ToolExecutionContext tool_context,
                                                                int iteration,
                                                                CancellationToken* cancellation) {
  tool_context = normalize_tool_execution_context(std::move(tool_context));
  if (!tool_context.cancellation) {
    tool_context.cancellation = cancellation;
  }
  tool_context.iteration = iteration;
  tool_context.attributes["iteration"] = iteration;
  return tool_context;
}

void ToolCallOrchestrator::commit_tool_results(
    SessionMemory& session,
    std::vector<AgentTraceEntry>& trace,
    int iteration,
    const std::vector<ToolExecutionResult>& tool_results) {
  std::vector<AgentMessage> tool_messages;
  tool_messages.reserve(tool_results.size());
  for (const auto& result : tool_results) {
    tool_messages.push_back(result.message);
  }
  session.add_many(tool_messages);
  trace.push_back(AgentTraceEntry{.type = "tools", .iteration = iteration, .tool_results = tool_results});
}

}  // namespace agent
