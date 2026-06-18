#pragma once

#include "internal.hpp"

namespace agent {

class MemoryWriteback {
 public:
  MemoryWriteback(LongTermMemoryPort* memory, RunnerWritebackOptions options);

  static RunnerWritebackOptions merge_options(const RunnerWritebackOptions& base,
                                              const RunnerWritebackOptions& override);
  bool apply_conversation_turn(const std::string& session_id,
                               const std::string& input,
                               const std::string& output,
                               const std::optional<ExecutionPlan>& plan) const;

 private:
  bool enabled() const;
  bool should_apply(const std::string& input, const std::string& output) const;
  static std::vector<std::string> plan_step_titles(const std::optional<ExecutionPlan>& plan);

  LongTermMemoryPort* memory_;
  RunnerWritebackOptions options_;
};

}  // namespace agent
