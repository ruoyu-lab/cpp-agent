#pragma once

#include "internal.hpp"

namespace agent {

class CompactionPlanner {
 public:
  static void maybe_auto_compact(SessionMemory& session,
                                 EventBus* event_bus,
                                 const TraceContext& trace_context);
  static SessionMemorySnapshot compact_session(SessionMemory& session, EventBus* event_bus);

 private:
  static Value compaction_event_payload(const SessionMemory& session,
                                        std::size_t before_tokens,
                                        std::size_t budget);
};

}  // namespace agent
