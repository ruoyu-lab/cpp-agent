#include "runner_kernel.hpp"

#include <utility>

namespace agent {

AgentRunnerKernel::AgentRunnerKernel(AgentRunnerResolvedConfig resolved_config)
    : config(std::make_unique<AgentRunnerResolvedConfig>(std::move(resolved_config))),
      tool_registry(config->tools),
      context_manager(config->contexts),
      memory_store(config->memory_store ? config->memory_store : std::make_shared<InMemorySessionStore>()),
      scratch_store(config->scratch_store ? config->scratch_store : std::make_shared<InMemoryScratchStore>()) {}

}  // namespace agent
