#include "agent/memory_retrieval.hpp"

namespace agent {

Value retrieved_memory_to_value(const RetrievedMemory& memory) {
  return Value::object({{"id", memory.id},
                        {"content", memory.content},
                        {"score", memory.score},
                        {"metadata", memory.metadata},
                        {"namespace", memory.namespace_id}});
}

Value long_term_memory_context_result_to_value(const LongTermMemoryContextResult& result) {
  Value::Array hits;
  for (const auto& hit : result.hits) {
    hits.push_back(retrieved_memory_to_value(hit));
  }
  return Value::object({{"hits", Value(hits)},
                        {"message", result.message ? agent_message_to_value(*result.message) : Value()}});
}

}  // namespace agent
