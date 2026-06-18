#pragma once

#include "internal.hpp"

namespace agent {

struct ToolBatchExecutionResult {
  std::vector<ToolExecutionResult> tool_results;
};

class ToolBatchExecutor {
 public:
  explicit ToolBatchExecutor(ToolExecutor& tool_executor);

  ToolBatchExecutionResult execute_batch(const std::vector<ToolCall>& tool_calls,
                                         ToolExecutionContext tool_context) const;
  ToolBatchExecutionResult stream_batch(const std::vector<ToolCall>& tool_calls,
                                        ToolExecutionContext tool_context,
                                        int iteration,
                                        AgentLoopStreamEventHandler emit_stream_event,
                                        CancellationToken* cancellation) const;

 private:
  ToolExecutionResult execute_tool_call_safely(const ToolCall& tool_call,
                                               ToolExecutionContext& tool_context,
                                               int iteration,
                                               const AgentLoopStreamEventHandler& emit_stream_event,
                                               CancellationToken* cancellation) const;

  ToolExecutor& tool_executor_;
};

}  // namespace agent
