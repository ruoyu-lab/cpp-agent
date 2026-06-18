#include "memory_writeback.hpp"

#include <utility>

namespace agent {

MemoryWriteback::MemoryWriteback(LongTermMemoryPort* memory, RunnerWritebackOptions options)
    : memory_(memory), options_(std::move(options)) {}

RunnerWritebackOptions MemoryWriteback::merge_options(const RunnerWritebackOptions& base,
                                                      const RunnerWritebackOptions& override) {
  RunnerWritebackOptions merged = base;
  if (override.enabled.has_value()) {
    merged.enabled = override.enabled;
  }
  if (!override.namespace_id.empty()) {
    merged.namespace_id = override.namespace_id;
  }
  if (!override.metadata.is_object() || !override.metadata.as_object().empty()) {
    merged.metadata = override.metadata;
  }
  return merged;
}

bool MemoryWriteback::apply_conversation_turn(const std::string& session_id,
                                              const std::string& input,
                                              const std::string& output,
                                              const std::optional<ExecutionPlan>& plan) const {
  if (!should_apply(input, output)) {
    return false;
  }
  memory_->remember_conversation_turn(LongTermMemoryWritebackInput{
      .session_id = session_id,
      .input = input,
      .output = output,
      .namespace_id = options_.namespace_id,
      .metadata = options_.metadata,
      .plan_steps = plan_step_titles(plan),
  });
  return true;
}

bool MemoryWriteback::enabled() const {
  return options_.enabled.has_value() ? *options_.enabled : memory_->auto_remember();
}

bool MemoryWriteback::should_apply(const std::string& input, const std::string& output) const {
  return memory_ && enabled() && !trim_copy(input).empty() && !trim_copy(output).empty();
}

std::vector<std::string> MemoryWriteback::plan_step_titles(const std::optional<ExecutionPlan>& plan) {
  std::vector<std::string> titles;
  if (!plan) {
    return titles;
  }
  titles.reserve(plan->steps.size());
  for (const auto& step : plan->steps) {
    titles.push_back(step.title);
  }
  return titles;
}

}  // namespace agent
