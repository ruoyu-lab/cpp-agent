#pragma once

#include "internal.hpp"

namespace agent {

class UsageAggregator {
 public:
  static void emit_cache_stats(EventBus& bus,
                               const AgentOutput& response,
                               const ModelSettings& settings,
                               const TraceContext& trace);
  static ModelUsage from_trace(const std::vector<AgentTraceEntry>& trace);
};

}  // namespace agent
