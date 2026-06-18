#include "internal.hpp"

#include <utility>

namespace agent {

Value runner_loop_context(Value context,
                          const std::optional<ExecutionPlan>& plan,
                          const std::vector<RetrievedMemory>& memory_hits,
                          const std::vector<KnowledgeSearchHit>& knowledge_hits,
                          const Value& knowledge_debug) {
  if (!context.is_object()) {
    context = Value::object({});
  }
  if (plan) {
    context["plan"] = execution_plan_to_value(*plan);
  }
  context["memoryHits"] = retrieved_memories_hook_value(memory_hits);
  context["knowledgeHits"] = knowledge_hits_value(knowledge_hits);
  if (knowledge_debug.is_object() && !knowledge_debug.as_object().empty()) {
    context["knowledgeDebug"] = knowledge_debug;
  }
  return context;
}

}  // namespace agent
