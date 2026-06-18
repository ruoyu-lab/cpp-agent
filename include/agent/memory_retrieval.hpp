#pragma once

#include "agent/core.hpp"
#include "agent/execution.hpp"
#include "agent/messages.hpp"

#include <cstddef>
#include <optional>

namespace agent {

struct RetrievedMemory {
  std::string id;
  std::string content;
  double score = 0;
  Value metadata = Value::object({});
  std::string namespace_id = "default";
};

struct SearchMemoryOptions {
  std::optional<std::size_t> top_k;
  std::optional<double> min_score;
  std::string namespace_id;
};

struct LongTermMemoryContextResult {
  std::vector<RetrievedMemory> hits;
  std::optional<AgentMessage> message;
};

struct LongTermMemoryWritebackInput {
  std::string session_id;
  std::string input;
  std::string output;
  std::string namespace_id;
  Value metadata = Value::object({});
  std::vector<std::string> plan_steps;
};

Value retrieved_memory_to_value(const RetrievedMemory& memory);
Value long_term_memory_context_result_to_value(const LongTermMemoryContextResult& result);

class LongTermMemoryPort {
 public:
  virtual ~LongTermMemoryPort() = default;
  virtual LongTermMemoryContextResult build_context_message(
      const std::string& query,
      const SearchMemoryOptions& options = {},
      CancellationToken* cancellation = nullptr) = 0;
  [[nodiscard]] virtual bool auto_remember() const noexcept = 0;
  virtual void remember_conversation_turn(const LongTermMemoryWritebackInput& input) = 0;
};

}  // namespace agent
