#pragma once

#include "agent/runtime_runner_types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace agent {

enum class AgentRunnerDurableStatus {
  Running,
  Completed,
  Interrupted,
};

std::string to_string(AgentRunnerDurableStatus status);

struct AgentRunnerDurableState {
  int version = 1;
  AgentRunnerDurableStatus status = AgentRunnerDurableStatus::Running;
  std::string run_id;
  std::string session_id;
  std::string input;
  Value input_value;
  AgentMessage input_message;
  ModelSettings model_settings;
  std::string effective_input;
  Value effective_input_value;
  AgentMessage effective_input_message;
  std::string input_text;
  std::vector<MessageContentPart> input_parts;
  std::optional<ExecutionPlan> plan;
  std::vector<RetrievedMemory> memory_hits;
  std::vector<KnowledgeSearchHit> knowledge_hits;
  Value knowledge_debug = Value::object({});
  std::vector<AgentMessage> preface_messages;
  std::optional<AgentLoopDurableState> loop;
  std::string updated_at;
};

using AgentRunnerCheckpointHandler = std::function<void(const AgentRunnerDurableState&)>;

struct AgentRunnerDurableOptions {
  std::optional<AgentRunnerDurableState> resume_state;
  AgentRunnerCheckpointHandler on_checkpoint;
};

Value agent_runner_durable_state_to_value(const AgentRunnerDurableState& state);

}  // namespace agent
