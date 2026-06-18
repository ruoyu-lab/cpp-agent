#pragma once

#include "agent/runtime_runner_api.hpp"

#include <memory>
#include <vector>

namespace agent {

class AgentRuntimeBuilder {
 public:
  AgentRuntimeBuilder() = default;
  explicit AgentRuntimeBuilder(AgentRunnerConfig config);

  AgentRuntimeBuilder& model(std::shared_ptr<ChatModelAdapter> adapter,
                             ModelSettings settings = {});
  AgentRuntimeBuilder& thinking_model(std::shared_ptr<ChatModelAdapter> adapter);
  AgentRuntimeBuilder& critique_model(std::shared_ptr<ChatModelAdapter> adapter);
  AgentRuntimeBuilder& tools(std::vector<ToolDefinition> definitions);
  AgentRuntimeBuilder& context_sources(std::vector<ContextSource> sources);
  AgentRuntimeBuilder& system_prompt(std::string prompt);
  AgentRuntimeBuilder& max_iterations(int value);
  AgentRuntimeBuilder& session_store(std::shared_ptr<SessionStore> store);
  AgentRuntimeBuilder& scratch_store(std::shared_ptr<ScratchStore> store);
  AgentRuntimeBuilder& long_term_memory(std::shared_ptr<LongTermMemoryPort> memory);
  AgentRuntimeBuilder& knowledge(KnowledgeContextProvider* provider,
                                 RunnerKnowledgeRetrievalOptions retrieval = {});
  AgentRuntimeBuilder& permission_policy(PermissionPolicy policy);
  AgentRuntimeBuilder& approval_handler(PermissionApprovalHandler handler);
  AgentRuntimeBuilder& tool_services(ToolExecutionServices services);
  AgentRuntimeBuilder& event_bus(EventBus* bus);
  AgentRuntimeBuilder& hooks(HookSet hooks);
  AgentRuntimeBuilder& planner(std::shared_ptr<Planner> planner);
  AgentRuntimeBuilder& enable_planning(bool enabled);
  AgentRuntimeBuilder& react(ReActRuntimeConfig config);

  [[nodiscard]] AgentRunnerConfig build_config() const;
  [[nodiscard]] AgentRunner build() const;

 private:
  AgentRunnerConfig config_;
};

}  // namespace agent
