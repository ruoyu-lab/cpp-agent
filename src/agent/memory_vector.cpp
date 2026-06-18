#include "agent/memory_vector.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace agent {

namespace {

Value embedding_to_value(const EmbeddingVector& embedding) {
  Value::Array values;
  for (const auto value : embedding) {
    values.emplace_back(value);
  }
  return Value(values);
}

}  // namespace

Value vector_memory_record_to_value(const VectorMemoryRecord& record) {
  return Value::object({{"id", record.id},
                        {"content", record.content},
                        {"embedding", embedding_to_value(record.embedding)},
                        {"metadata", record.metadata},
                        {"namespace", record.namespace_id},
                        {"createdAt", record.created_at},
                        {"updatedAt", record.updated_at}});
}

#include "memory/vector_memory.inc"

void LongTermMemory::remember_conversation_turn(const LongTermMemoryWritebackInput& input) {
  remember_conversation_turn(input.session_id,
                             input.input,
                             input.output,
                             input.metadata,
                             input.namespace_id,
                             input.plan_steps);
}

}  // namespace agent
