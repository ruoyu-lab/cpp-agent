#pragma once

#include "internal.hpp"

#include <mutex>

namespace agent {

struct AgentRunnerKernel {
  explicit AgentRunnerKernel(AgentRunnerResolvedConfig resolved_config);

  std::unique_ptr<AgentRunnerResolvedConfig> config;
  ToolRegistry tool_registry;
  EmbeddedContextManager context_manager;
  std::shared_ptr<SessionStore> memory_store;
  std::shared_ptr<ScratchStore> scratch_store;
  EventBus owned_event_bus;
  mutable std::mutex last_context_stats_mutex;
  std::optional<ContextStatsSnapshot> last_context_stats;
};

}  // namespace agent
