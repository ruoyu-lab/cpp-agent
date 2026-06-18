#pragma once

#include "internal.hpp"
#include "tool_batch_executor.hpp"

namespace agent {

struct ToolCallOrchestrationResult {
  std::vector<ToolExecutionResult> tool_results;
  ToolExecutionContext tool_context;
};

class ToolCallOrchestrator {
 public:
  explicit ToolCallOrchestrator(ToolExecutor& tool_executor);

  ToolCallOrchestrationResult run_tool_calls(const std::vector<ToolCall>& tool_calls,
                                             SessionMemory& session,
                                             std::vector<AgentTraceEntry>& trace,
                                             ToolExecutionContext tool_context,
                                             int iteration,
                                             CancellationToken* cancellation) const;
  ToolCallOrchestrationResult stream_tool_calls(const std::vector<ToolCall>& tool_calls,
                                                SessionMemory& session,
                                                std::vector<AgentTraceEntry>& trace,
                                                ToolExecutionContext tool_context,
                                                int iteration,
                                                AgentLoopStreamEventHandler emit_stream_event,
                                                CancellationToken* cancellation) const;

 private:
  static ToolExecutionContext prepare_tool_context(ToolExecutionContext tool_context,
                                                   int iteration,
                                                   CancellationToken* cancellation);
  static void commit_tool_results(SessionMemory& session,
                                  std::vector<AgentTraceEntry>& trace,
                                  int iteration,
                                  const std::vector<ToolExecutionResult>& tool_results);

  ToolExecutor& tool_executor_;
};

}  // namespace agent
