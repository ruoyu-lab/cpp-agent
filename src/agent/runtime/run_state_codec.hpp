#pragma once

#include "internal.hpp"

namespace agent {

class RunStateCodec {
 public:
  static const char* loop_phase_to_string(AgentLoopDurablePhase phase);
  static const char* runner_status_to_string(AgentRunnerDurableStatus status);
  static AgentLoopDurableState build_loop_checkpoint(
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
      std::optional<AgentLoopTerminationReason> termination_reason);
  static Value loop_to_value(const AgentLoopDurableState& state);
  static Value runner_to_value(const AgentRunnerDurableState& state);

 private:
  static Value tool_call_to_value(const ToolCall& tool_call);
  static Value model_response_to_value(const AgentOutput& response);
  static Value tool_execution_result_to_value(const ToolExecutionResult& result);
  static Value trace_entry_to_value(const AgentTraceEntry& entry);
  static Value reasoning_settings_to_value(const ReasoningSettings& reasoning);
  static Value model_settings_to_value(const ModelSettings& settings);
  static Value messages_to_value(const std::vector<AgentMessage>& messages);
  static Value memory_hits_to_value(const std::vector<RetrievedMemory>& hits);
};

}  // namespace agent
