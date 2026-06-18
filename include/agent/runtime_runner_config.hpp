#pragma once

#include "agent/context.hpp"
#include "agent/context_stats.hpp"
#include "agent/execution.hpp"
#include "agent/knowledge_runtime.hpp"
#include "agent/memory_retrieval.hpp"
#include "agent/memory_session.hpp"
#include "agent/react_types.hpp"
#include "agent/runtime_runner_durable.hpp"
#include "agent/runtime_runner_stream.hpp"
#include "agent/skills_core.hpp"
#include "agent/tools.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agent {

struct ContextStatsOptions {
  ModelSettings model_settings;
  std::vector<SkillActivation> skill_activations;
  Value context = Value::object({});
  std::optional<std::size_t> context_window_tokens;
  ContextTokenCounter token_counter;
  bool allow_skill_input_rewrite = true;
};

class AgentRunner;
struct SkillForkRequest {
  ResolvedSkillUse skill;
  std::string input_text;
  std::string session_id;
  ModelSettings model_settings;
  Value context = Value::object({});
};

struct SkillForkResult {
  std::string text;
  Value metadata = Value::object({});
};

using SkillForkHandler = std::function<std::optional<SkillForkResult>(const SkillForkRequest&)>;

enum class AgentToolCallingStrategy {
  TextReAct,
  NativeToolCalling,
};

std::string to_string(AgentToolCallingStrategy strategy);

struct ModelRuntimeConfig {
  std::shared_ptr<ChatModelAdapter> adapter;
  std::shared_ptr<ChatModelAdapter> thinking_adapter;
  std::shared_ptr<ChatModelAdapter> critique_adapter;
  ModelSettings settings;
};

struct ToolRuntimeConfig {
  std::optional<std::vector<ToolDefinition>> definitions;
  PermissionPolicy permission_policy;
  PermissionApprovalHandler approval_handler;
  ToolExecutionServices services;
  std::optional<bool> lazy_mode;
  std::vector<std::string> forced_visible_tools;
  std::optional<AgentToolCallingStrategy> calling_strategy;
};

struct ContextRuntimeConfig {
  std::optional<std::vector<ContextSource>> sources;
  std::optional<std::string> system_prompt;
  std::optional<int> max_iterations;
  std::shared_ptr<SkillRegistry> skills;
  std::vector<std::string> default_skills;
  std::optional<bool> advertise_skills;
  std::optional<SkillConflictPolicy> skill_model_conflict_policy;
  std::optional<SkillConflictPolicy> skill_effort_conflict_policy;
  SkillForkHandler skill_fork_handler;
  std::map<std::string, AgentRunner*> skill_subagents;
};

struct MemoryRuntimeConfig {
  std::shared_ptr<SessionStore> session_store;
  std::shared_ptr<ScratchStore> scratch_store;
  std::shared_ptr<LongTermMemoryPort> long_term_memory;
  RunnerRetrievalOptions retrieval_options;
  RunnerWritebackOptions writeback_options;
};

struct KnowledgeRuntimeConfig {
  KnowledgeContextProvider* provider = nullptr;
  RunnerKnowledgeRetrievalOptions retrieval_options;
};

struct GovernanceConfig {
  std::shared_ptr<Planner> planner;
  std::optional<bool> enable_planning;
  ExecutionPolicies execution_policies;
  PermissionPolicy permission_policy;
  PermissionApprovalHandler approval_handler;
};

struct ObservabilityConfig {
  EventBus* event_bus = nullptr;
  HookSet hooks;
  std::vector<ContextStatsBucketInput> prompt_stats_buckets;
  std::optional<std::size_t> context_window_tokens;
  ContextTokenCounter context_token_counter;
};

struct ReActRuntimeConfig {
  std::optional<int> max_parse_errors;
  std::optional<ReActPromptMode> prompt_mode;
  std::optional<ReActParserOptions> parser_options;
  std::shared_ptr<ReActPromptBuilder> prompt_builder;
  std::optional<bool> persist_visible_messages;
};

struct AgentRunnerConfig {
  ModelRuntimeConfig model_runtime;
  ToolRuntimeConfig tool_runtime;
  ContextRuntimeConfig context_runtime;
  MemoryRuntimeConfig memory_runtime;
  KnowledgeRuntimeConfig knowledge_runtime;
  GovernanceConfig governance;
  ObservabilityConfig observability;
  ReActRuntimeConfig react_runtime;
};

}  // namespace agent
