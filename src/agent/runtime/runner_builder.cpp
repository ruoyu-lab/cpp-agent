#include "internal.hpp"
#include "compaction_planner.hpp"
#include "memory_writeback.hpp"
#include "runner_kernel.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace agent {

AgentRuntimeBuilder::AgentRuntimeBuilder(AgentRunnerConfig config)
    : config_(std::move(config)) {}

AgentRuntimeBuilder& AgentRuntimeBuilder::model(std::shared_ptr<ChatModelAdapter> adapter,
                                                ModelSettings settings) {
  config_.model_runtime.adapter = std::move(adapter);
  config_.model_runtime.settings = std::move(settings);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::thinking_model(std::shared_ptr<ChatModelAdapter> adapter) {
  config_.model_runtime.thinking_adapter = std::move(adapter);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::critique_model(std::shared_ptr<ChatModelAdapter> adapter) {
  config_.model_runtime.critique_adapter = std::move(adapter);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::tools(std::vector<ToolDefinition> definitions) {
  config_.tool_runtime.definitions = std::move(definitions);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::context_sources(std::vector<ContextSource> sources) {
  config_.context_runtime.sources = std::move(sources);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::system_prompt(std::string prompt) {
  config_.context_runtime.system_prompt = std::move(prompt);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::max_iterations(int value) {
  config_.context_runtime.max_iterations = value;
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::session_store(std::shared_ptr<SessionStore> store) {
  config_.memory_runtime.session_store = std::move(store);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::scratch_store(std::shared_ptr<ScratchStore> store) {
  config_.memory_runtime.scratch_store = std::move(store);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::long_term_memory(std::shared_ptr<LongTermMemoryPort> memory) {
  config_.memory_runtime.long_term_memory = std::move(memory);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::knowledge(KnowledgeContextProvider* provider,
                                                    RunnerKnowledgeRetrievalOptions retrieval) {
  config_.knowledge_runtime.provider = provider;
  config_.knowledge_runtime.retrieval_options = std::move(retrieval);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::permission_policy(PermissionPolicy policy) {
  config_.tool_runtime.permission_policy = std::move(policy);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::approval_handler(PermissionApprovalHandler handler) {
  config_.tool_runtime.approval_handler = std::move(handler);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::tool_services(ToolExecutionServices services) {
  config_.tool_runtime.services = std::move(services);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::event_bus(EventBus* bus) {
  config_.observability.event_bus = bus;
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::hooks(HookSet hooks) {
  config_.observability.hooks = std::move(hooks);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::planner(std::shared_ptr<Planner> planner) {
  config_.governance.planner = std::move(planner);
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::enable_planning(bool enabled) {
  config_.governance.enable_planning = enabled;
  return *this;
}

AgentRuntimeBuilder& AgentRuntimeBuilder::react(ReActRuntimeConfig config) {
  config_.react_runtime = std::move(config);
  return *this;
}

AgentRunnerConfig AgentRuntimeBuilder::build_config() const {
  return config_;
}

AgentRunner AgentRuntimeBuilder::build() const {
  return AgentRunner(build_config());
}

}  // namespace agent
