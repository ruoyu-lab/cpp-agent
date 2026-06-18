#pragma once

#include "agent/model_providers.hpp"
#include "agent/memory_vector.hpp"
#include "agent/runtime.hpp"

namespace agent {

struct StandardRuntimePresetOptions {
  std::shared_ptr<ChatModelAdapter> model;
  ModelSettings model_settings;
  std::vector<std::string> tool_bundles = {"core"};
  std::vector<ToolDefinition> tools;
  std::vector<ContextSource> context_sources;
  std::string system_prompt;
  int max_iterations = 4;
  bool enable_planning = true;
  std::shared_ptr<SessionStore> session_store;
  std::shared_ptr<ScratchStore> scratch_store;
  std::shared_ptr<LongTermMemory> long_term_memory;
  KnowledgeContextProvider* knowledge = nullptr;
  RunnerKnowledgeRetrievalOptions knowledge_retrieval;
  PermissionPolicy permission_policy;
  PermissionApprovalHandler approval_handler;
  ToolExecutionServices tool_services;
  EventBus* event_bus = nullptr;
  HookSet hooks;
  std::shared_ptr<Planner> planner;
  ReActRuntimeConfig react;
};

struct LocalModelRuntimePresetOptions {
  StandardRuntimePresetOptions runtime;
  LlamaCppNativeChatModelAdapterConfig llama_cpp;
};

AgentRuntimeBuilder create_standard_runtime(StandardRuntimePresetOptions options = {});
AgentRuntimeBuilder create_local_model_runtime(LocalModelRuntimePresetOptions options = {});

}  // namespace agent
