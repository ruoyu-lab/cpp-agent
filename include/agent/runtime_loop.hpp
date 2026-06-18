#pragma once

#include "agent/memory_session.hpp"
#include "agent/model.hpp"
#include "agent/react_types.hpp"
#include "agent/streaming.hpp"
#include "agent/tools.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace agent {

class ReActPromptBuilder;

struct AgentTraceEntry {
  std::string type;
  int iteration = 0;
  AgentOutput response;
  std::vector<ToolExecutionResult> tool_results;
};

enum class AgentLoopTerminationReason {
  Completed,
  MaxIterations,
  IncompleteResponse,
};

std::string to_string(AgentLoopTerminationReason reason);

enum class AgentLoopDurablePhase {
  BeforeModel,
  ModelCompleted,
  ActionBatchParsed,
  ToolBatchCompleted,
  Completed,
};

std::string to_string(AgentLoopDurablePhase phase);

struct AgentLoopRunResult {
  std::string session_id;
  int iteration_count = 0;
  std::string text;
  AgentOutput response;
  std::vector<AgentTraceEntry> trace;
  std::vector<ReActTraceEntry> react_trace;
  std::vector<AgentMessage> messages;
  AgentLoopTerminationReason termination_reason = AgentLoopTerminationReason::Completed;
  ModelUsage usage;
  std::string latest_non_empty_reasoning;
};

struct AgentLoopDurableState {
  int version = 1;
  AgentLoopDurablePhase phase = AgentLoopDurablePhase::BeforeModel;
  std::string session_id;
  int next_iteration = 0;
  std::string input_text;
  std::vector<MessageContentPart> input_parts;
  AgentMessage input_message;
  Value input_value;
  SessionMemorySnapshot session;
  std::vector<AgentTraceEntry> trace;
  std::vector<ReActTraceEntry> react_trace;
  std::optional<AgentOutput> last_response;
  std::string last_assistant_text;
  std::string latest_non_empty_reasoning;
  int consecutive_parse_errors = 0;
  int consecutive_reasoning_protocol_leaks = 0;
  std::optional<AgentLoopTerminationReason> termination_reason;
};

using AgentLoopCheckpointHandler = std::function<void(const AgentLoopDurableState&)>;

struct AgentLoopDurableOptions {
  std::optional<AgentLoopDurableState> resume_state;
  AgentLoopCheckpointHandler on_checkpoint;
};

Value agent_loop_durable_state_to_value(const AgentLoopDurableState& state);

enum class AgentLoopStreamEventType {
  IterationStart,
  ModelStart,
  ModelTextDelta,
  UserVisibleDelta,
  ModelReasoningDelta,
  ModelReasoningCompleted,
  AgentOutput,
  ToolCallArgumentDelta,
  ReActActionBatch,
  ToolBatchStart,
  ToolStart,
  ToolDelta,
  ToolComplete,
  ToolBatchComplete,
  ReActMessage,
  ReActObservation,
  ReActFinal,
  ReActFinalRejected,
  ReActReasoningProtocolLeak,
  ReActParseError,
  Done,
};

std::string to_string(AgentLoopStreamEventType type);

struct AgentLoopStreamEvent {
  int schema_version = kAgentStreamEventSchemaVersion;
  std::uint64_t sequence = 0;
  AgentLoopStreamEventType type = AgentLoopStreamEventType::IterationStart;
  int iteration = 0;
  std::string provider;
  std::string model;
  std::string delta;
  std::string text;
  std::string reasoning;
  std::string reasoning_id;
  std::string reasoning_scope;
  std::string run_id;
  AgentOutput response;
  ToolCall tool_call;
  std::vector<ToolCall> tool_calls;
  ToolExecutionResult tool_result;
  std::vector<ToolExecutionResult> tool_results;
  ReActTraceEntry react_step;
  AgentLoopRunResult result;
  // Populated only when type == ToolCallArgumentDelta.
  std::string tool_call_id;
  std::string tool_call_name;
  std::string tool_call_args_delta;
  std::string tool_call_args_accumulated;
};

using AgentLoopStreamEventHandler = std::function<void(const AgentLoopStreamEvent&)>;

struct AgentLoopStreamResult {
  AgentLoopRunResult result;
};

}  // namespace agent
