#pragma once

#include "agent/runtime_runner_types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace agent {

struct AgentRunnerStatus {
  std::string kind;
  std::string stage;
  std::string state;
  std::string message;
  int iteration = -1;
  std::string provider;
  std::string model;
  std::string tool_name;
  std::string tool_call_id;
  Value details = Value::object({});
};

enum class AgentRunnerStreamEventType {
  Status,
  KnowledgeRetrieval,
  MemoryRetrieval,
  Planning,
  UserVisibleDelta,
  Loop,
  ToolCallArgumentDelta,
  Done,
  Cancelled,
  Error,
};

std::string to_string(AgentRunnerStreamEventType type);

struct AgentRunnerStreamEvent {
  int schema_version = kAgentStreamEventSchemaVersion;
  std::uint64_t sequence = 0;
  AgentRunnerStreamEventType type = AgentRunnerStreamEventType::Loop;
  std::string delta;
  std::string text;
  AgentRunnerStatus status;
  std::vector<RetrievedMemory> memory_hits;
  std::optional<AgentMessage> memory_message;
  std::vector<KnowledgeSearchHit> knowledge_hits;
  std::optional<AgentMessage> knowledge_message;
  Value knowledge_debug = Value::object({});
  std::optional<ExecutionPlan> plan;
  AgentLoopStreamEvent loop_event;
  AgentRunnerRunResult result;
  Value cancellation = Value::object({});
  Value error = Value::object({});
  // Populated only when type == ToolCallArgumentDelta.
  int tool_call_iteration = -1;
  std::string tool_call_provider;
  std::string tool_call_model;
  std::string tool_call_id;
  std::string tool_call_name;
  std::string tool_call_args_delta;
  std::string tool_call_args_accumulated;
};

using AgentRunnerStreamEventHandler = std::function<void(const AgentRunnerStreamEvent&)>;

struct AgentRunnerStreamResult {
  AgentRunnerRunResult result;
};

class AgentRunnerEventStream {
 public:
  AgentRunnerEventStream() = default;
  AgentRunnerEventStream(BoundedStreamQueue<AgentRunnerStreamEvent> queue,
                         std::thread producer,
                         std::shared_ptr<CancellationToken> owned_cancellation = {});
  AgentRunnerEventStream(const AgentRunnerEventStream&) = delete;
  AgentRunnerEventStream& operator=(const AgentRunnerEventStream&) = delete;
  AgentRunnerEventStream(AgentRunnerEventStream&& other) noexcept;
  AgentRunnerEventStream& operator=(AgentRunnerEventStream&& other) noexcept;
  ~AgentRunnerEventStream();

  bool next(AgentRunnerStreamEvent& event);
  void close();
  void cancel(std::string reason = "Stream cancelled.");

 private:
  void join();

  BoundedStreamQueue<AgentRunnerStreamEvent> queue_;
  std::thread producer_;
  std::shared_ptr<CancellationToken> owned_cancellation_;
  bool active_ = false;
};

}  // namespace agent
