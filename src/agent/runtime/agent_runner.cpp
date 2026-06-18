#include "internal.hpp"
#include "compaction_planner.hpp"
#include "memory_writeback.hpp"
#include "runner_config.hpp"
#include "runner_kernel.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>

namespace agent {

AgentRunner::AgentRunner(const AgentRunnerConfig& config)
    : kernel_(std::make_unique<AgentRunnerKernel>(normalize_agent_runner_config(config))) {
  auto& kernel = *kernel_;
  if (!kernel.config->adapter) {
    throw ConfigurationError("AgentRunner requires an explicit adapter instance.");
  }
  if (kernel.config->lazy_tool_mode) {
    kernel.tool_registry.set_lazy_mode(true);
    for (const auto& name : kernel.config->forced_visible_tools) {
      kernel.tool_registry.force_visible(name);
    }
  }
}

AgentRunner::~AgentRunner() = default;

}  // namespace agent
