#include "compaction_planner.hpp"

namespace agent {

void CompactionPlanner::maybe_auto_compact(SessionMemory& session,
                                           EventBus* event_bus,
                                           const TraceContext& trace_context) {
  if (!session.should_auto_compact()) {
    return;
  }
  const std::size_t before_tokens = session.estimated_token_count();
  const std::size_t budget = session.token_budget();
  session.compact();
  if (event_bus) {
    event_bus->publish("session.auto_compact", ExecutionTarget::Run,
                       compaction_event_payload(session, before_tokens, budget), trace_context);
  }
}

SessionMemorySnapshot CompactionPlanner::compact_session(SessionMemory& session, EventBus* event_bus) {
  const std::size_t before_tokens = session.estimated_token_count();
  session.compact();
  const auto snapshot = session.snapshot();
  if (event_bus) {
    event_bus->publish("session.compact", ExecutionTarget::Run,
                       compaction_event_payload(session, before_tokens, session.token_budget()));
  }
  return snapshot;
}

Value CompactionPlanner::compaction_event_payload(const SessionMemory& session,
                                                  std::size_t before_tokens,
                                                  std::size_t budget) {
  return Value::object({
      {"sessionId", session.session_id()},
      {"tokensBefore", static_cast<long long>(before_tokens)},
      {"tokensAfter", static_cast<long long>(session.estimated_token_count())},
      {"tokenBudget", static_cast<long long>(budget)},
  });
}

}  // namespace agent
